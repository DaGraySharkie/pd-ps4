# Technical notes

## Overview

| Area | Approach |
| --- | --- |
| Toolchain | Stock LLVM 18 + OpenOrbis v0.5.4 (musl libc, libc++), through `cmake/ps4-toolchain.cmake` |
| Rendering | Piglet (OpenGL ES 2.0 + EGL 1.4). `port/fast3d/gfx_gles2.cpp` is a GLES2 variant of the port's GL backend |
| Window | `port/fast3d/gfx_ps4.cpp`: fixed 1920×1080 Piglet surface, timer-paced frames, SDL for events only |
| SDL2 | 2.30.9, dummy video driver; events, timing and the game controller API |
| Controllers | `scePad` per logged-in user (up to 4), each exposed to SDL as a virtual DualShock 4 while connected; logins, logouts and power-offs are polled twice a second, and `input.c`'s own hotplug handling assigns players |
| Audio | `sceAudioOut`; the game mixes at 22020 Hz, a feeder thread resamples to 48 kHz |
| Memory | `mmap`/`munmap` replaced so the C heap lives in its own arena, leaving flexible memory to Piglet |
| Files | Everything under `/data/perfectdark` with absolute paths (relative paths fail with `EINVAL`) |

PS4 sources added by the patch:

- `port/src/ps4/ps4main.c`: process entry, boot log, signal handler, 8 MiB game thread, `--basedir`/`--savedir`.
- `port/src/ps4/ps4platform.c`: module loading, Piglet configuration list, EGL, vsync watchdog, pad.
- `port/src/ps4/ps4heap.c`: heap arena (512 MiB system flexible memory, else 512→128 MiB direct memory).
- `port/fast3d/gfx_gles2.cpp`, `gfx_ps4.cpp`: renderer and window backend.

Hooks in existing files: `platform.h` (`PLATFORM_PS4`), `CMakeLists.txt` (target and source
selection), `video.c` (backend choice, 1080p/60 fps defaults), `audio.c` (sceAudioOut branch),
`input.c` (attach the pad), `system.c` (paths), `main.c` (`main` → `pdMain`), `gfx_pc.cpp` (always
render offscreen).

## Renderer: GLES2 differences

The upstream backend needs GL 3.0 / GLES 3.0. `gfx_gles2.cpp` keeps its combiner shader generator
and changes:

- **GLSL ES 1.00.** `textureSize()` becomes `uTexSize0/1` uniforms, fed in `draw_triangles` from
  sizes recorded at upload. The blur loop uses `mod`/`floor` instead of `&`/`>>`, `mix()` gets a
  float selector, and `0.3f` becomes `0.3`. Fragment shaders use `highp` when the driver reports it.
- **No `glBlitFramebuffer`.** Framebuffer copies and the final present draw a textured quad with a
  small dedicated program. The fast3d core caches program, attributes, viewport, scissor and texture
  bindings, so the blit restores all of them.
- **Always offscreen.** SoH found that reading the window surface (`glCopyTexImage2D`) takes
  seconds on Piglet. Perfect Dark copies the current frame for several effects (`bondview.c`,
  `menugfx.c`), so on PS4 `gfx_pc.cpp` always renders into its game framebuffer (as it already did
  for MSAA) and presents it at the end of the frame (`GFX_ALWAYS_OFFSCREEN`).
- **Formats and features.** Unsized `GL_RGBA`/`GL_RGB` textures, no VAOs, no MSAA, no depth clamp
  (the existing `z *= 0.3` fallback is used), and `GL_MIRRORED_REPEAT` instead of mirror-clamp.
  Depth is 24-bit if `GL_OES_depth24` is advertised and renders, otherwise 16-bit (SoH: a 32-bit
  depth renderbuffer gives an incomplete framebuffer). The N64 itself used 16-bit Z.
- **Non-power-of-two textures.** Without `GL_OES_texture_npot`, NPOT textures (framebuffer copies)
  are forced to clamp and no mipmaps, otherwise they would be incomplete and sample black.

`tests/shadertest` compiles the real generator on the host with stubbed GL entry points, produces
9000 programs over all filter modes and option bits, and validates every shader with glslang as
GLSL ES 1.00 (18002/18002 pass). A control shader with the desktop constructs fails as expected.

## Things verified off-console

- The OpenOrbis libc's `__mmap`/`mmap64` and `__munmap` are `jmp`s to the exported `mmap`/`munmap`,
  so musl's `malloc` really does use the arena; `mremap` returns `ENOSYS`, so realloc copies.
- All Piglet, EGL, pad, audio and system-service imports resolve to OpenOrbis stub libraries.
- A clean checkout runs `setup.sh` → `build-deps.sh` → `build.sh` → `package.sh` to a `.pkg`.

## Known risks (need a console)

- **Piglet memory configuration.** It is copied from the SoH configuration verified on a PS4 Pro,
  with fallbacks. Base PS4 and other firmwares are unknown territory.
- **Vertex attributes.** The most complex combiners use up to 12 attributes (position, 2 texcoords,
  4 clamp values, fog, grayscale, 4 inputs). GLES2 only guarantees 8. GCN hardware should offer 16;
  `pd.log` prints `GL_MAX_VERTEX_ATTRIBS`. If it is lower, the clamp floats can be packed into one vec4.
- **Shader compile hitches.** Each new combiner costs 0.1–0.3 s on Piglet's runtime compiler, and
  linked programs cannot be cached (SoH). SoH's fix, which is not ported yet, is to record the
  combiner ids seen and precompile them behind the splash screen on the next start.
- **Vsync and frame pacing.** The 60 fps timer pacing is what worked for SoH. `Uncap Tickrate` and
  framerates above 60 have not been considered.
