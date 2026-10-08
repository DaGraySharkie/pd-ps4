#ifndef _IN_PS4PLATFORM_H
#define _IN_PS4PLATFORM_H

// PS4 platform layer (OpenOrbis): system modules, Piglet/EGL, DualShock 4, memory stats.
// Only compiled when PLATFORM_PS4 is defined.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Everything the game reads and writes (ROM, config, saves, logs) lives here.
#define PS4_DATA_DIR "/data/perfectdark"
// Piglet presents to a fixed 1080p surface, the system scales it to the actual output.
#define PS4_DISPLAY_WIDTH 1920
#define PS4_DISPLAY_HEIGHT 1080

// Loads the system modules and the Piglet + shader compiler pair, configures Piglet and creates
// a GLES2 context on a full screen window surface. Returns 0 on success.
int ps4GfxInit(int width, int height);
void ps4GfxSwapBuffers(void);
void ps4GfxShutdown(void);
// 1 if the context was created with a swap interval of 1 (see ps4GfxInit).
int ps4GfxVsyncActive(void);

// Initial (logged in) user, or -1.
int32_t ps4GetUserId(void);

// Opens the DualShock 4 of every logged-in user (initial user first, up to 4) and exposes each
// connected one to SDL as a virtual game controller, so the regular SDL input code handles them.
// Call after SDL_INIT_GAMECONTROLLER.
void ps4PadAttach(void);
// Picks up users logging in/out and controllers being switched on/off. Call every frame; it
// rate-limits itself.
void ps4PadPoll(void);

// The system shows the boot splash until the application hides it.
void ps4HideSplashScreen(void);

void ps4LogMemory(const char *when);

#ifdef __cplusplus
}
#endif

#endif
