// PS4 window backend. The GLES2 context and the window surface come from Piglet (see
// port/src/ps4/ps4platform.c); SDL only provides events, timing and the game controller API
// (built with its dummy video driver).

#include "platform.h"

#ifdef PLATFORM_PS4

#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>

#include "system.h"
#include "ps4platform.h"

#include "gfx_window_manager_api.h"
#include "gfx_screen_config.h"

static SDL_Window *wnd;
static int window_width = PS4_DISPLAY_WIDTH;
static int window_height = PS4_DISPLAY_HEIGHT;
static void (*on_fullscreen_changed_callback)(bool is_now_fullscreen);

static int target_fps = 60;
static uint64_t previous_time;
static uint64_t qpc_freq;
static uint32_t frames_presented;

static bool system_splash_hidden;

#define FRAME_INTERVAL_US_NUMERATOR 1000000
#define FRAME_INTERVAL_US_DENOMINATOR (target_fps)

static void gfx_ps4_init(const struct GfxWindowInitSettings *set) {
    // Piglet always presents a 1080p surface; the system scales it to the actual video output.
    (void)set;
    window_width = PS4_DISPLAY_WIDTH;
    window_height = PS4_DISPLAY_HEIGHT;

    // the only video driver SDL has on this platform, and it has to be asked for by name
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        sysLogPrintf(LOG_ERROR, "SDL_Init failed: %s", SDL_GetError());
    }

    // only exists so SDL has a window to attach events to
    wnd = SDL_CreateWindow(set->title, 0, 0, window_width, window_height, SDL_WINDOW_SHOWN);
    if (!wnd) {
        sysLogPrintf(LOG_WARNING, "SDL_CreateWindow failed: %s", SDL_GetError());
    }

    if (ps4GfxInit(window_width, window_height) != 0) {
        sysFatalError("Could not create the OpenGL ES context.\nSee " PS4_DATA_DIR "/ps4_boot.log and pd.log.");
    }

    qpc_freq = SDL_GetPerformanceFrequency();
}

static void gfx_ps4_close(void) {
}

static int gfx_ps4_get_display_mode(int modenum, int *out_w, int *out_h) {
    if (modenum != 0) {
        return 0;
    }
    *out_w = window_width;
    *out_h = window_height;
    return 1;
}

static int gfx_ps4_get_current_display_mode(int *out_w, int *out_h) {
    *out_w = window_width;
    *out_h = window_height;
    return 1;
}

static int gfx_ps4_get_num_display_modes(void) {
    return 1;
}

static int32_t gfx_ps4_get_fullscreen_state(void) {
    return 1;
}

static void gfx_ps4_set_fullscreen_changed_callback(void (*cb)(bool is_now_fullscreen)) {
    on_fullscreen_changed_callback = cb;
}

static void gfx_ps4_set_fullscreen(bool enable) {
    (void)enable; // always full screen
}

static void gfx_ps4_set_fullscreen_exclusive(bool enable) {
    (void)enable;
}

static void gfx_ps4_set_fullscreen_flag(int32_t mode) {
    (void)mode;
}

static int32_t gfx_ps4_get_fullscreen_flag_mode(void) {
    return 1;
}

static int32_t gfx_ps4_get_maximized_state(void) {
    return 0;
}

static void gfx_ps4_set_maximize(bool enable) {
    (void)enable;
}

static void gfx_ps4_get_active_window_refresh_rate(uint32_t *refresh_rate) {
    *refresh_rate = 60;
}

static void gfx_ps4_set_cursor_visibility(bool visible) {
    (void)visible;
}

static void gfx_ps4_set_closest_resolution(int32_t width, int32_t height, bool should_center) {
    (void)width;
    (void)height;
    (void)should_center;
}

static void gfx_ps4_set_dimensions(uint32_t width, uint32_t height, int32_t posX, int32_t posY) {
    (void)width;
    (void)height;
    (void)posX;
    (void)posY;
}

static void gfx_ps4_get_dimensions(uint32_t *width, uint32_t *height, int32_t *posX, int32_t *posY) {
    *width = (uint32_t)window_width;
    *height = (uint32_t)window_height;
    *posX = 0;
    *posY = 0;
}

static void gfx_ps4_get_centered_positions(int32_t width, int32_t height, int32_t *posX, int32_t *posY) {
    *posX = window_width / 2 - width / 2;
    *posY = window_height / 2 - height / 2;
}

static void gfx_ps4_handle_events(void) {
    SDL_Event event;
    // controllers coming and going (users logging in/out, pads switched on/off)
    ps4PadPoll();
    // also runs the virtual joysticks' Update callbacks (scePad polling)
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            exit(0);
        }
    }
}

static bool gfx_ps4_start_frame(void) {
    return true;
}

static uint64_t qpc_to_100ns(uint64_t qpc) {
    return qpc / qpc_freq * 10000000 + qpc % qpc_freq * 10000000 / qpc_freq;
}

static inline void sync_framerate_with_timer(void) {
    uint64_t t = qpc_to_100ns(SDL_GetPerformanceCounter());

    const int64_t next = previous_time + 10 * FRAME_INTERVAL_US_NUMERATOR / FRAME_INTERVAL_US_DENOMINATOR;
    int64_t left = next - t;
    // exit a bit early and busy-wait the rest so the deadline is never missed
    left -= 15000UL;
    if (left > 0) {
        sysSleep(left);
    }

    do {
        sysCpuRelax();
        t = qpc_to_100ns(SDL_GetPerformanceCounter());
    } while ((int64_t)t < next);

    t = qpc_to_100ns(SDL_GetPerformanceCounter());
    if (left > 0 && t - next < 10000) {
        t = next;
    }
    previous_time = t;
}

static void gfx_ps4_swap_buffers_begin(void) {
    if (target_fps && !ps4GfxVsyncActive()) {
        sync_framerate_with_timer();
    }

    ps4GfxSwapBuffers();

    // The system shows the package's sce_sys/pic1.png from launch until it is told to stop:
    // keep it up until the first game frame is on screen, so loading isn't a black screen.
    if (!system_splash_hidden) {
        ps4HideSplashScreen();
        system_splash_hidden = true;
    }

    ++frames_presented;
    if (frames_presented <= 3) {
        sysLogPrintf(LOG_NOTE, "PS4: frame %u presented", frames_presented);
    } else if (frames_presented % 36000 == 0) {
        ps4LogMemory("while running");
    }
}

static void gfx_ps4_swap_buffers_end(void) {
}

static double gfx_ps4_get_time(void) {
    return SDL_GetPerformanceCounter() / (double)qpc_freq;
}

static int32_t gfx_ps4_get_target_fps(void) {
    return target_fps;
}

static void gfx_ps4_set_target_fps(int fps) {
    target_fps = fps;
}

static bool gfx_ps4_can_disable_vsync(void) {
    return true;
}

static void *gfx_ps4_get_window_handle(void) {
    return (void *)wnd;
}

static void gfx_ps4_set_window_title(const char *title) {
    (void)title;
}

static int gfx_ps4_get_swap_interval(void) {
    return ps4GfxVsyncActive();
}

static bool gfx_ps4_set_swap_interval(int interval) {
    // The swap interval can only be chosen before the EGL surface exists (see ps4GfxInit):
    // report success only for the one that is actually in effect, so the game falls back to its
    // own frame limiter otherwise.
    return (interval != 0) == (ps4GfxVsyncActive() != 0);
}

struct GfxWindowManagerAPI gfx_ps4 = {
    gfx_ps4_init,
    gfx_ps4_close,
    gfx_ps4_get_display_mode,
    gfx_ps4_get_current_display_mode,
    gfx_ps4_get_num_display_modes,
    gfx_ps4_get_fullscreen_state,
    gfx_ps4_set_fullscreen_changed_callback,
    gfx_ps4_set_fullscreen,
    gfx_ps4_set_fullscreen_exclusive,
    gfx_ps4_set_fullscreen_flag,
    gfx_ps4_get_fullscreen_flag_mode,
    gfx_ps4_get_maximized_state,
    gfx_ps4_set_maximize,
    gfx_ps4_get_active_window_refresh_rate,
    gfx_ps4_set_cursor_visibility,
    gfx_ps4_set_closest_resolution,
    gfx_ps4_set_dimensions,
    gfx_ps4_get_dimensions,
    gfx_ps4_get_centered_positions,
    gfx_ps4_handle_events,
    gfx_ps4_start_frame,
    gfx_ps4_swap_buffers_begin,
    gfx_ps4_swap_buffers_end,
    gfx_ps4_get_time,
    gfx_ps4_get_target_fps,
    gfx_ps4_set_target_fps,
    gfx_ps4_can_disable_vsync,
    gfx_ps4_get_window_handle,
    gfx_ps4_set_window_title,
    gfx_ps4_get_swap_interval,
    gfx_ps4_set_swap_interval,
};

#endif // PLATFORM_PS4
