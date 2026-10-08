// PS4 platform layer: system modules, Piglet (OpenGL ES 2.0 + EGL), DualShock 4.
//
// The approach (module pair from /data/self/system/common/lib, the Piglet configuration
// fallback list, opt-in vsync guarded by a watchdog, scePad exposed through an SDL virtual
// joystick) follows alechurri/shipofharkinian-ps4, which documents why each of these is
// needed on real hardware: https://github.com/alechurri/shipofharkinian-ps4/blob/main/docs/TECHNICAL.md

#include "platform.h"

#ifdef PLATFORM_PS4

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <SDL.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <stdbool.h> // Pigletv2VSH.h uses bool without including it
#include <orbis/Pigletv2VSH.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

#include "system.h"
#include "ps4platform.h"

// ---------------------------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------------------------

// Retail firmware ships Piglet without its runtime GLSL compiler, so (like every other GLES
// homebrew) a Piglet + libSceShaccVSH pair is loaded from here. The pair can also be bundled in
// the package under /app0/assets/misc/ for a self-contained install.
#define PS4_USER_MODULE_DIR "/data/self/system/common/lib"
#define PS4_BUNDLED_MODULE_DIR "/app0/assets/misc"

static int32_t pigletModule = -1;
static int32_t shaccModule = -1;

static int32_t ps4LoadModule(const char *path)
{
	int32_t startResult = 0;
	const int32_t handle = (int32_t)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &startResult);
	if (handle < 0) {
		sysLogPrintf(LOG_WARNING, "PS4: sceKernelLoadStartModule(%s) failed: 0x%08x", path, (uint32_t)handle);
	} else {
		sysLogPrintf(LOG_NOTE, "PS4: loaded %s (handle %d, start %d)", path, handle, startResult);
	}
	return handle;
}

static int ps4FileExists(const char *path)
{
	return access(path, F_OK) == 0;
}

static int ps4LoadGraphicsModules(void)
{
	if (pigletModule >= 0) {
		return 1;
	}

	char sysdir[128];
	const char *word = sceKernelGetFsSandboxRandomWord();
	snprintf(sysdir, sizeof(sysdir), "/%s/common/lib", word ? word : "system");

	static const char *const sysmods[] = {
		"libSceSysCore", "libSceMbus", "libSceIpmi", "libSceSystemService",
		"libSceUserService", "libSceAudioOut", "libScePad",
	};
	char path[256];
	for (size_t i = 0; i < sizeof(sysmods) / sizeof(*sysmods); ++i) {
		snprintf(path, sizeof(path), "%s/%s.sprx", sysdir, sysmods[i]);
		ps4LoadModule(path);
	}

	// Piglet and the shader compiler are a matched pair: both come from the same directory.
	const char *dirs[] = { PS4_BUNDLED_MODULE_DIR, PS4_USER_MODULE_DIR };
	const char *moddir = NULL;
	for (size_t i = 0; i < 2 && pigletModule < 0; ++i) {
		snprintf(path, sizeof(path), "%s/libScePigletv2VSH.sprx", dirs[i]);
		if (ps4FileExists(path)) {
			pigletModule = ps4LoadModule(path);
			if (pigletModule >= 0) {
				moddir = dirs[i];
			}
		}
	}

	if (pigletModule < 0) {
		sysLogPrintf(LOG_WARNING, "PS4: no Piglet in %s, trying the system one (shaders will most likely fail to compile)",
			PS4_USER_MODULE_DIR);
		snprintf(path, sizeof(path), "%s/libScePigletv2VSH.sprx", sysdir);
		pigletModule = ps4LoadModule(path);
		moddir = sysdir;
	}
	if (pigletModule < 0) {
		sysLogPrintf(LOG_ERROR, "PS4: could not load libScePigletv2VSH.sprx");
		return 0;
	}

	snprintf(path, sizeof(path), "%s/libSceShaccVSH.sprx", moddir);
	shaccModule = ps4LoadModule(path);
	if (shaccModule < 0) {
		snprintf(path, sizeof(path), "%s/libSceShaccVSH.sprx", sysdir);
		shaccModule = ps4LoadModule(path);
	}
	if (shaccModule < 0) {
		sysLogPrintf(LOG_ERROR, "PS4: could not load libSceShaccVSH.sprx (the GLSL compiler). "
			"Copy libScePigletv2VSH.sprx and libSceShaccVSH.sprx to " PS4_USER_MODULE_DIR "/");
	}

	return 1;
}

// ---------------------------------------------------------------------------------------------
// Memory statistics
// ---------------------------------------------------------------------------------------------

// The OpenOrbis prototypes for these take their out parameters by value; call them through the
// real signatures.
typedef int32_t (*Ps4AvailDirectFn)(off_t, off_t, size_t, off_t *, size_t *);
typedef int32_t (*Ps4AvailFlexFn)(size_t *);

extern int ps4HeapGetKind(void) __attribute__((weak));
extern void ps4HeapGetStats(size_t *arena, size_t *inuse, size_t *peak) __attribute__((weak));

static size_t ps4LargestFreeDirectBlock(void)
{
	off_t phys = 0;
	size_t size = 0;
	((Ps4AvailDirectFn)(void *)sceKernelAvailableDirectMemorySize)(0, (off_t)sceKernelGetDirectMemorySize(), 0, &phys, &size);
	return size;
}

void ps4LogMemory(const char *when)
{
	size_t flex = 0;
	((Ps4AvailFlexFn)(void *)sceKernelAvailableFlexibleMemorySize)(&flex);

	size_t arena = 0, inuse = 0, peak = 0;
	int kind = 0;
	if (ps4HeapGetStats && ps4HeapGetKind) {
		ps4HeapGetStats(&arena, &inuse, &peak);
		kind = ps4HeapGetKind();
	}

	sysLogPrintf(LOG_NOTE, "PS4: memory %s: heap %zu/%zu MiB (peak %zu, %s), direct total %zu MiB, "
		"largest free direct %zu MiB, free flexible %zu MiB",
		when, inuse >> 20, arena >> 20, peak >> 20,
		kind == 1 ? "system flexible" : (kind == 2 ? "direct" : "no arena"),
		(size_t)sceKernelGetDirectMemorySize() >> 20, ps4LargestFreeDirectBlock() >> 20, flex >> 20);
}

// ---------------------------------------------------------------------------------------------
// Piglet / EGL
// ---------------------------------------------------------------------------------------------

#define PS4_VSYNC_MARKER PS4_DATA_DIR "/ps4_vsync"
#define PS4_NOVSYNC_MARKER PS4_DATA_DIR "/ps4_novsync"

struct pigletsizes {
	uint64_t sysShared;
	uint64_t vidShared;
	uint64_t flexible;
	uint32_t cmdBuf;
	uint32_t lcueBuf;
	uint64_t neededDirect; // only tried if this much direct memory is free in one block
};

// Tried in order until eglGetDisplay() succeeds. A "gde" app gets ~768 MiB of direct memory and
// ~255 MiB of flexible memory. Perfect Dark needs far less than Ocarina of Time does (smaller
// framebuffers count, ~16 MiB game heap + a 32 MiB ROM image), so the heap arena is smaller and
// the list starts with the verified SoH configuration.
static const struct pigletsizes pigletCandidates[] = {
	{ 128ull << 20, 512ull << 20, 236ull << 20, 4u << 20, 4u << 20, 700ull << 20 },
	{ 128ull << 20, 512ull << 20, 208ull << 20, 4u << 20, 4u << 20, 700ull << 20 },
	{ 128ull << 20, 384ull << 20, 208ull << 20, 1u << 20, 1u << 20, 560ull << 20 },
	{  64ull << 20, 128ull << 20, 208ull << 20, 1u << 20, 1u << 20, 0 },
	{  64ull << 20, 128ull << 20, 176ull << 20, 1u << 20, 1u << 20, 0 },
	{  64ull << 20, 128ull << 20,  64ull << 20, 1u << 20, 1u << 20, 0 },
};

static EGLDisplay eglDisp = EGL_NO_DISPLAY;
static EGLSurface eglSurf = EGL_NO_SURFACE;
static EGLContext eglCtx = EGL_NO_CONTEXT;
static OrbisPglWindow pglWindow;
static int vsyncOn = 0;

// SDL_GetTicks64() + 1 while eglSwapBuffers() runs, 0 otherwise
static volatile uint64_t swapStartedAt = 0;

static void *ps4SwapWatchdog(void *arg)
{
	(void)arg;
	for (;;) {
		SDL_Delay(250);
		const uint64_t started = swapStartedAt;
		if (started && SDL_GetTicks64() + 1 - started > 4000) {
			FILE *f = fopen(PS4_NOVSYNC_MARKER, "w");
			if (f) {
				fputs("eglSwapBuffers blocked with swap interval 1; delete this file to try vsync again\n", f);
				fclose(f);
			}
			sysLogPrintf(LOG_ERROR, "PS4: eglSwapBuffers blocked for 4 s; vsync disabled for the next start, quitting");
			sceSystemServiceLoadExec("exit", NULL);
			_exit(0);
		}
	}
	return NULL;
}

static int ps4ConfigurePiglet(int w, int h, const struct pigletsizes *sz)
{
	OrbisPglConfig cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.size = sizeof(cfg);
	cfg.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | ORBIS_PGL_FLAGS_USE_TILED_TEXTURE | 0x20;
	if (sz->flexible) {
		cfg.flags |= ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY;
	}
	cfg.processOrder = 1;
	cfg.systemSharedMemorySize = sz->sysShared;
	cfg.videoSharedMemorySize = sz->vidShared;
	cfg.maxMappedFlexibleMemory = sz->flexible;
	cfg.drawCommandBufferSize = sz->cmdBuf;
	cfg.lcueResourceBufferSize = sz->lcueBuf;
	cfg.dbgPosCmd_0x40 = (uint32_t)w;
	cfg.dbgPosCmd_0x44 = (uint32_t)h;
	cfg.dbgPosCmd_0x48 = 0;
	cfg.dbgPosCmd_0x4C = 0;
	cfg.unk_0x5C = 2;

	sysLogPrintf(LOG_NOTE, "PS4: Piglet config: system %llu MiB, video %llu MiB, flexible %llu MiB, cmdbuf %u MiB",
		(unsigned long long)(sz->sysShared >> 20), (unsigned long long)(sz->vidShared >> 20),
		(unsigned long long)(sz->flexible >> 20), sz->cmdBuf >> 20);

	if (!scePigletSetConfigurationVSH(&cfg)) {
		sysLogPrintf(LOG_WARNING, "PS4: scePigletSetConfigurationVSH rejected that configuration");
		return 0;
	}
	return 1;
}

static int ps4ChooseConfig(EGLConfig *config)
{
	static const EGLint ds[][2] = { { 24, 8 }, { 24, 0 }, { 16, 0 }, { 0, 0 } };
	for (size_t i = 0; i < sizeof(ds) / sizeof(*ds); ++i) {
		const EGLint attribs[] = {
			EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
			EGL_DEPTH_SIZE, ds[i][0], EGL_STENCIL_SIZE, ds[i][1],
			EGL_SAMPLE_BUFFERS, 0, EGL_SAMPLES, 0,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_NONE,
		};
		EGLint num = 0;
		if (eglChooseConfig(eglDisp, attribs, config, 1, &num) && num >= 1) {
			EGLint d = 0, s = 0;
			eglGetConfigAttrib(eglDisp, *config, EGL_DEPTH_SIZE, &d);
			eglGetConfigAttrib(eglDisp, *config, EGL_STENCIL_SIZE, &s);
			sysLogPrintf(LOG_NOTE, "PS4: EGL config: asked depth %d stencil %d, got %d/%d", ds[i][0], ds[i][1], d, s);
			return 1;
		}
		sysLogPrintf(LOG_WARNING, "PS4: eglChooseConfig(depth %d, stencil %d) failed: 0x%04x", ds[i][0], ds[i][1], eglGetError());
	}
	return 0;
}

int ps4GfxInit(int width, int height)
{
	if (eglCtx != EGL_NO_CONTEXT) {
		return 0;
	}

	if (!ps4LoadGraphicsModules()) {
		return -1;
	}

	ps4LogMemory("before Piglet");

	for (size_t i = 0; i < sizeof(pigletCandidates) / sizeof(*pigletCandidates); ++i) {
		const struct pigletsizes *sz = &pigletCandidates[i];
		if (ps4LargestFreeDirectBlock() < sz->neededDirect) {
			continue;
		}
		if (!ps4ConfigurePiglet(width, height, sz)) {
			continue;
		}
		eglDisp = eglGetDisplay(EGL_DEFAULT_DISPLAY);
		if (eglDisp != EGL_NO_DISPLAY) {
			break;
		}
		sysLogPrintf(LOG_WARNING, "PS4: eglGetDisplay failed with that configuration");
	}
	if (eglDisp == EGL_NO_DISPLAY) {
		sysLogPrintf(LOG_ERROR, "PS4: eglGetDisplay failed with every Piglet configuration");
		return -1;
	}

	EGLint major = 0, minor = 0;
	if (!eglInitialize(eglDisp, &major, &minor)) {
		sysLogPrintf(LOG_ERROR, "PS4: eglInitialize failed: 0x%04x", eglGetError());
		return -1;
	}
	sysLogPrintf(LOG_NOTE, "PS4: EGL %d.%d", major, minor);

	if (!eglBindAPI(EGL_OPENGL_ES_API)) {
		sysLogPrintf(LOG_ERROR, "PS4: eglBindAPI failed: 0x%04x", eglGetError());
		return -1;
	}

	// eglSwapInterval(1) after eglMakeCurrent() has been seen to block eglSwapBuffers() forever;
	// it is only set here, before the surface exists, and only when asked for with a marker file.
	vsyncOn = ps4FileExists(PS4_VSYNC_MARKER) && !ps4FileExists(PS4_NOVSYNC_MARKER);
	sysLogPrintf(LOG_NOTE, "PS4: vsync %s", vsyncOn ? "on (ps4_vsync marker)" : "off, frame pacing by timer");
	if (!eglSwapInterval(eglDisp, vsyncOn ? 1 : 0)) {
		sysLogPrintf(LOG_WARNING, "PS4: eglSwapInterval failed: 0x%04x", eglGetError());
	}
	if (vsyncOn) {
		pthread_t thr;
		if (pthread_create(&thr, NULL, ps4SwapWatchdog, NULL) == 0) {
			pthread_detach(thr);
		}
	}

	EGLConfig config = NULL;
	if (!ps4ChooseConfig(&config)) {
		sysLogPrintf(LOG_ERROR, "PS4: no usable EGL config");
		return -1;
	}

	memset(&pglWindow, 0, sizeof(pglWindow));
	pglWindow.uID = 0;
	pglWindow.uWidth = (uint32_t)width;
	pglWindow.uHeight = (uint32_t)height;

	const EGLint winAttribs[] = { EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE };
	eglSurf = eglCreateWindowSurface(eglDisp, config, &pglWindow, winAttribs);
	if (eglSurf == EGL_NO_SURFACE) {
		sysLogPrintf(LOG_ERROR, "PS4: eglCreateWindowSurface failed: 0x%04x", eglGetError());
		return -1;
	}

	const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	eglCtx = eglCreateContext(eglDisp, config, EGL_NO_CONTEXT, ctxAttribs);
	if (eglCtx == EGL_NO_CONTEXT) {
		sysLogPrintf(LOG_ERROR, "PS4: eglCreateContext failed: 0x%04x", eglGetError());
		return -1;
	}

	if (!eglMakeCurrent(eglDisp, eglSurf, eglSurf, eglCtx)) {
		sysLogPrintf(LOG_ERROR, "PS4: eglMakeCurrent failed: 0x%04x", eglGetError());
		return -1;
	}

	sysLogPrintf(LOG_NOTE, "PS4: GLES context ready (%dx%d)", width, height);
	ps4LogMemory("after Piglet");

	return 0;
}

void ps4GfxSwapBuffers(void)
{
	if (eglDisp != EGL_NO_DISPLAY && eglSurf != EGL_NO_SURFACE) {
		swapStartedAt = SDL_GetTicks64() + 1;
		eglSwapBuffers(eglDisp, eglSurf);
		swapStartedAt = 0;
	}
}

int ps4GfxVsyncActive(void)
{
	return vsyncOn;
}

void ps4GfxShutdown(void)
{
	if (eglDisp == EGL_NO_DISPLAY) {
		return;
	}
	eglMakeCurrent(eglDisp, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (eglSurf != EGL_NO_SURFACE) {
		eglDestroySurface(eglDisp, eglSurf);
		eglSurf = EGL_NO_SURFACE;
	}
	if (eglCtx != EGL_NO_CONTEXT) {
		eglDestroyContext(eglDisp, eglCtx);
		eglCtx = EGL_NO_CONTEXT;
	}
	eglTerminate(eglDisp);
	eglDisp = EGL_NO_DISPLAY;
}

void ps4HideSplashScreen(void)
{
	sceSystemServiceHideSplashScreen();
}

// ---------------------------------------------------------------------------------------------
// User / pad
// ---------------------------------------------------------------------------------------------

static int userReady = 0;
static int32_t userId = -1;

int32_t ps4GetUserId(void)
{
	if (!userReady) {
		struct { int32_t priority; } params = { 700 }; // SCE_KERNEL_PRIO_FIFO_DEFAULT
		sceUserServiceInitialize(&params); // harmless if already initialized
		if (sceUserServiceGetInitialUser(&userId) < 0) {
			sysLogPrintf(LOG_ERROR, "PS4: sceUserServiceGetInitialUser failed");
			userId = -1;
		}
		userReady = 1;
	}
	return userId;
}

// One slot per logged-in PS4 user (up to 4, the game's maximum). Each user's DualShock 4 is
// exposed to SDL as its own virtual game controller while it is connected; the game's input code
// already assigns controllers to players as they appear and disappear. Users who log in later
// (PS button on a second controller, pick a profile) are picked up by ps4PadPoll().

#define PS4_MAX_PADS 4
#define PS4_PAD_POLL_MS 500

struct ps4pad {
	int32_t userId;
	int32_t handle;
	SDL_JoystickID instance;
};

static struct ps4pad pads[PS4_MAX_PADS] = {
	{ -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 }, { -1, -1, -1 },
};
static int padInitDone = 0;
static uint64_t padLastPoll = 0;

static Sint16 ps4StickToAxis(uint8_t v)
{
	int s = ((int)v - 128) * 258;
	if (s > 32767) s = 32767;
	if (s < -32768) s = -32768;
	return (Sint16)s;
}

static Sint16 ps4TriggerToAxis(uint8_t v)
{
	// The generated mapping for a virtual controller ("lefttrigger:a4") takes the full
	// -32768..32767 range as input and rescales it to the 0..32767 trigger range.
	return (Sint16)((int)v * 257 - 32768);
}

static void SDLCALL ps4PadUpdate(void *userdata)
{
	struct ps4pad *pad = (struct ps4pad *)userdata;
	if (pad->handle < 0 || pad->instance < 0) {
		return;
	}

	SDL_Joystick *joy = SDL_JoystickFromInstanceID(pad->instance);
	if (!joy) {
		return;
	}

	OrbisPadData data;
	memset(&data, 0, sizeof(data));
	if (scePadReadState(pad->handle, &data) < 0 || !data.connected) {
		return;
	}

	static const struct { SDL_GameControllerButton sdl; uint32_t orbis; } map[] = {
		{ SDL_CONTROLLER_BUTTON_A,             ORBIS_PAD_BUTTON_CROSS },
		{ SDL_CONTROLLER_BUTTON_B,             ORBIS_PAD_BUTTON_CIRCLE },
		{ SDL_CONTROLLER_BUTTON_X,             ORBIS_PAD_BUTTON_SQUARE },
		{ SDL_CONTROLLER_BUTTON_Y,             ORBIS_PAD_BUTTON_TRIANGLE },
		// SHARE belongs to the system; the touch pad click stands in for BACK.
		{ SDL_CONTROLLER_BUTTON_BACK,          ORBIS_PAD_BUTTON_TOUCH_PAD },
		{ SDL_CONTROLLER_BUTTON_START,         ORBIS_PAD_BUTTON_OPTIONS },
		{ SDL_CONTROLLER_BUTTON_LEFTSTICK,     ORBIS_PAD_BUTTON_L3 },
		{ SDL_CONTROLLER_BUTTON_RIGHTSTICK,    ORBIS_PAD_BUTTON_R3 },
		{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  ORBIS_PAD_BUTTON_L1 },
		{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ORBIS_PAD_BUTTON_R1 },
		{ SDL_CONTROLLER_BUTTON_DPAD_UP,       ORBIS_PAD_BUTTON_UP },
		{ SDL_CONTROLLER_BUTTON_DPAD_DOWN,     ORBIS_PAD_BUTTON_DOWN },
		{ SDL_CONTROLLER_BUTTON_DPAD_LEFT,     ORBIS_PAD_BUTTON_LEFT },
		{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    ORBIS_PAD_BUTTON_RIGHT },
	};
	for (size_t i = 0; i < sizeof(map) / sizeof(*map); ++i) {
		SDL_JoystickSetVirtualButton(joy, map[i].sdl, (data.buttons & map[i].orbis) ? SDL_PRESSED : SDL_RELEASED);
	}

	SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_LEFTX, ps4StickToAxis(data.leftStick.x));
	SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_LEFTY, ps4StickToAxis(data.leftStick.y));
	SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_RIGHTX, ps4StickToAxis(data.rightStick.x));
	SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_RIGHTY, ps4StickToAxis(data.rightStick.y));
	SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERLEFT, ps4TriggerToAxis(data.analogButtons.l2));
	SDL_JoystickSetVirtualAxis(joy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, ps4TriggerToAxis(data.analogButtons.r2));
}

static int SDLCALL ps4PadRumble(void *userdata, Uint16 lo, Uint16 hi)
{
	struct ps4pad *pad = (struct ps4pad *)userdata;
	if (pad->handle < 0) {
		return -1;
	}
	OrbisPadVibeParam vib;
	vib.lgMotor = (uint8_t)(lo >> 8);
	vib.smMotor = (uint8_t)(hi >> 8);
	return scePadSetVibration(pad->handle, &vib) < 0 ? -1 : 0;
}

static int SDLCALL ps4PadSetLED(void *userdata, Uint8 r, Uint8 g, Uint8 b)
{
	struct ps4pad *pad = (struct ps4pad *)userdata;
	if (pad->handle < 0) {
		return -1;
	}
	OrbisPadColor c = { r, g, b, 255 };
	return scePadSetLightBar(pad->handle, &c) < 0 ? -1 : 0;
}

static void ps4PadAttachVirtual(struct ps4pad *pad)
{
	SDL_VirtualJoystickDesc desc;
	SDL_zero(desc);
	desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	desc.naxes = SDL_CONTROLLER_AXIS_MAX;
	desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
	desc.nhats = 0;
	desc.vendor_id = 0x054C;  // Sony
	desc.product_id = 0x09CC; // DualShock 4 (v2): lets SDL report it as a PS4 controller
	desc.name = "PS4 Controller";
	desc.userdata = pad;
	desc.Update = ps4PadUpdate;
	desc.Rumble = ps4PadRumble;
	desc.SetLED = ps4PadSetLED;

	const int idx = SDL_JoystickAttachVirtualEx(&desc);
	if (idx < 0) {
		sysLogPrintf(LOG_ERROR, "PS4: SDL_JoystickAttachVirtualEx failed: %s", SDL_GetError());
		return;
	}
	pad->instance = SDL_JoystickGetDeviceInstanceID(idx);
	sysLogPrintf(LOG_NOTE, "PS4: controller of user 0x%08x connected (pad %d, SDL instance %d)",
		(uint32_t)pad->userId, (int)(pad - pads), (int)pad->instance);
}

static void ps4PadDetachVirtual(struct ps4pad *pad)
{
	if (pad->instance < 0) {
		return;
	}
	const int n = SDL_NumJoysticks();
	for (int i = 0; i < n; ++i) {
		if (SDL_JoystickGetDeviceInstanceID(i) == pad->instance) {
			SDL_JoystickDetachVirtual(i);
			break;
		}
	}
	sysLogPrintf(LOG_NOTE, "PS4: controller of user 0x%08x disconnected (pad %d)", (uint32_t)pad->userId, (int)(pad - pads));
	pad->instance = -1;
}

static void ps4PadClose(struct ps4pad *pad)
{
	ps4PadDetachVirtual(pad);
	if (pad->handle >= 0) {
		scePadClose(pad->handle);
	}
	sysLogPrintf(LOG_NOTE, "PS4: user 0x%08x logged out (pad %d)", (uint32_t)pad->userId, (int)(pad - pads));
	pad->handle = -1;
	pad->userId = -1;
}

static void ps4PadOpen(struct ps4pad *pad, int32_t uid)
{
	int32_t handle = scePadOpen(uid, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);
	if (handle < 0) {
		handle = scePadGetHandle(uid, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
	}
	if (handle < 0) {
		sysLogPrintf(LOG_WARNING, "PS4: scePadOpen for user 0x%08x failed: 0x%08x", (uint32_t)uid, (uint32_t)handle);
		return;
	}
	pad->userId = uid;
	pad->handle = handle;
	pad->instance = -1;
	sysLogPrintf(LOG_NOTE, "PS4: opened the controller of user 0x%08x (pad %d, handle %d)", (uint32_t)uid, (int)(pad - pads), handle);
}

static void ps4PadRefresh(void)
{
	// logged-in users, the initial user first so they stay player 1
	int32_t users[PS4_MAX_PADS];
	int nusers = 0;
	const int32_t initial = ps4GetUserId();
	if (initial >= 0) {
		users[nusers++] = initial;
	}

	OrbisUserServiceLoginUserIdList list;
	memset(&list, 0xff, sizeof(list));
	if (sceUserServiceGetLoginUserIdList(&list) >= 0) {
		for (int i = 0; i < ORBIS_USER_SERVICE_MAX_LOGIN_USERS && nusers < PS4_MAX_PADS; ++i) {
			const int32_t uid = list.userId[i];
			if (uid == ORBIS_USER_SERVICE_USER_ID_INVALID || uid == initial) {
				continue;
			}
			users[nusers++] = uid;
		}
	}

	// users who logged out
	for (int p = 0; p < PS4_MAX_PADS; ++p) {
		if (pads[p].userId < 0) {
			continue;
		}
		int found = 0;
		for (int u = 0; u < nusers; ++u) {
			found |= users[u] == pads[p].userId;
		}
		if (!found) {
			ps4PadClose(&pads[p]);
		}
	}

	// users who logged in
	for (int u = 0; u < nusers; ++u) {
		int have = 0;
		for (int p = 0; p < PS4_MAX_PADS; ++p) {
			have |= pads[p].userId == users[u];
		}
		for (int p = 0; p < PS4_MAX_PADS && !have; ++p) {
			if (pads[p].userId < 0) {
				ps4PadOpen(&pads[p], users[u]);
				have = 1;
			}
		}
	}

	// controllers switched on or off (a logged-in user can be without a controller)
	for (int p = 0; p < PS4_MAX_PADS; ++p) {
		if (pads[p].handle < 0) {
			continue;
		}
		OrbisPadData data;
		memset(&data, 0, sizeof(data));
		const int connected = scePadReadState(pads[p].handle, &data) >= 0 && data.connected;
		if (connected && pads[p].instance < 0) {
			ps4PadAttachVirtual(&pads[p]);
		} else if (!connected && pads[p].instance >= 0) {
			ps4PadDetachVirtual(&pads[p]);
		}
	}
}

void ps4PadAttach(void)
{
	if (padInitDone) {
		return;
	}
	padInitDone = 1;

	// The dummy video driver never gives the window focus, and SDL drops joystick input of
	// unfocused applications unless told otherwise.
	SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_OVERRIDE);

	const int32_t ret = scePadInit();
	if (ret < 0) {
		sysLogPrintf(LOG_WARNING, "PS4: scePadInit returned 0x%08x", (uint32_t)ret);
	}

	ps4PadRefresh();
	padLastPoll = SDL_GetTicks64();
}

void ps4PadPoll(void)
{
	if (!padInitDone) {
		return;
	}
	const uint64_t now = SDL_GetTicks64();
	if (now - padLastPoll >= PS4_PAD_POLL_MS) {
		padLastPoll = now;
		ps4PadRefresh();
	}
}

#endif // PLATFORM_PS4
