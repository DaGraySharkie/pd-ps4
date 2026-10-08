# Perfect Dark for PS4

An experimental native port of the [Perfect Dark PC port](https://github.com/perfect-dark-pc-port/perfect_dark)
(itself built on the [n64decomp decompilation](https://github.com/n64decomp/perfect_dark)) to jailbroken
PS4 consoles, built with the [OpenOrbis toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain).
It renders through Piglet, the PS4's OpenGL ES 2.0 implementation. It is not an emulator.

The structure and most of the platform knowledge come from
[alechurri/shipofharkinian-ps4](https://github.com/alechurri/shipofharkinian-ps4); see
[docs/TECHNICAL.md](docs/TECHNICAL.md).

**No game data and no Sony binaries are included.** You need your own Perfect Dark ROM and the two
Piglet modules dumped from your own console.

Everything lives in one folder: the `perfect_dark` repository, with this `ps4/` folder in it. The
whole thing is distributed as a single patch file, `perfect_dark-ps4.patch`.

## Building

Linux (tested on Ubuntu 24.04) or WSL; Git Bash on Windows also works. Requirements: LLVM 18
(`clang`, `clang++`, `ld.lld`, `llvm-ar`), CMake 3.16+, Ninja, Git, Python 3, curl, and for
`package.sh` OpenSSL 1.1 (`PkgTool.Core` is a .NET Core 3 program; on Ubuntu 24 install
`libssl1.1` from the focal archive).

First time:

```sh
git clone https://github.com/perfect-dark-pc-port/perfect_dark
cd perfect_dark
git config core.autocrlf false
git checkout 32a1cb9f268dd3ac73016801025c6bbbfa20130f
tr -d '\r' < /path/to/perfect_dark-ps4.patch > /tmp/pd-ps4.patch && git apply /tmp/pd-ps4.patch

ps4/setup.sh        # downloads OpenOrbis v0.5.4 to ps4/tools/
ps4/build-deps.sh   # cross-builds zlib and SDL2 into ps4/prefix/
ps4/build.sh        # -> build/ps4/pd.x86_64.elf   (ROMID=pal-final / jpn-final for other regions)
ps4/package.sh      # -> ps4/out/IV0000-PDRK00001_00-PERFECTDARKPORT0.pkg
```

## Updating

A new version always comes as a complete `perfect_dark-ps4.patch`. From the `perfect_dark` folder:

```sh
ps4/update.sh /path/to/perfect_dark-ps4.patch
ps4/build.sh && ps4/package.sh
```

`update.sh` resets the game code to the upstream commit and applies the new patch. The toolchain
(`ps4/tools`), the dependencies (`ps4/prefix`, `ps4/deps`), your images (`ps4/custom`), finished
packages (`ps4/out`) and the build folder (`build/`) are kept. Changes of your own to the game
code or to the scripts are not.

Then uninstall the game on the PS4 before installing the new package: an install over the top
with the same version number may silently keep the old one.

## Installing

1. Dump `libScePigletv2VSH.sprx` and `libSceShaccVSH.sprx` (Piglet with its runtime shader
   compiler) and copy them over FTP to `/data/self/system/common/lib/`. Retail Piglet has no shader
   compiler; the same files work for the SoH and Super Mario 64 PS4 ports.
2. Copy your ROM to `/data/perfectdark/pd.ntsc-final.z64`
   (md5 `e03b088b6ac9e0080440efed07c1e40f`). Optionally `pd.gbc` next to it for the Transfer Pak
   cheats. A ROM in the older location `/data/perfectdark/data/` is still found.
3. Install the `.pkg` with GoldHEN's package installer and start it.

Config (`pd.ini`), saves and logs are written to `/data/perfectdark/` as well.

## Package images

The images the PS4 shows for the game. Put your own in `ps4/custom/sce_sys/` (create the folder);
anything not there comes from the defaults in `ps4/pkg-static/sce_sys/`. Run `ps4/package.sh` again
afterwards.

| File | Size | Shown |
| --- | --- | --- |
| `icon0.png` | 512×512 | Home screen icon |
| `pic1.png` | 1920×1080 | Launch splash: from the moment the game is started until its first frame |
| `pic0.png` | 1920×1080 | Home screen background when the game is selected (optional) |

All must be 24-bit RGB PNGs without transparency; `package.sh` says which file it uses and warns
if the size or format is wrong. The PS4 caches these images: uninstall the game before installing
a package with changed images (saves in `/data/perfectdark` are kept).

## Multiplayer

Up to four players, one DualShock 4 each. Every controller has to belong to a logged-in PS4
user: switch on the second controller, press its PS button and pick a profile (a guest user is
fine). The game picks it up within half a second, also in the middle of a session, and assigns
it to the next free player slot. Controllers that are switched off or whose user logs out are
released again. The first player is always the user who started the game.

## Controls

The DualShock 4 appears to the game as a standard controller, so the port's default Xbox-style
layout applies and everything can be rebound in `pd.ini`.

| DualShock 4 | Action |
| --- | --- |
| R2 | Fire / accept |
| L2 | Aim mode |
| Cross | Use / accept |
| Circle | Previous weapon |
| Square | Reload |
| Triangle | Next weapon |
| L1 | Radial menu |
| R1 | Alt-fire mode |
| L3 | Crouch cycle |
| OPTIONS | Start / pause |
| Touchpad click | Back |

## Debugging

- `/data/perfectdark/ps4_boot.log`: stdout/stderr from the very first instruction, including
  Piglet's own `[PIG]` messages and a line if the game crashes with a signal.
- `/data/perfectdark/pd.log`: the game's log, including the GLES capabilities found at startup
  (extensions, attribute/varying limits) and the memory layout.
- Vsync is off by default (frames are paced by a timer at 60 fps). An empty file
  `/data/perfectdark/ps4_vsync` turns it on; if `eglSwapBuffers` hangs with it, a watchdog quits and
  writes `ps4_novsync` so the next start goes back to the timer.

## Layout

| Path | Contents |
| --- | --- |
| `ps4/setup.sh`, `build-deps.sh`, `build.sh`, `package.sh`, `update.sh`, `env.sh` | Setup, build, packaging, updating |
| `ps4/cmake/` | CMake toolchain and platform files for OpenOrbis + LLVM 18 |
| `ps4/compat/` | Header fix-ups force-included into every file (the `struct stat` layout bug) |
| `ps4/zlib-cmake/` | Static zlib build for `build-deps.sh` |
| `ps4/pkg-static/` | Default package images |
| `ps4/tests/shadertest/` | Host-side GLSL ES 1.00 validation of the generated shaders |
| `port/src/ps4/`, `port/fast3d/gfx_ps4.cpp`, `gfx_gles2.cpp` | The PS4 code in the game |
| `ps4/tools/`, `prefix/`, `deps/`, `custom/`, `out/` and `build/` | Local, never touched by `update.sh` |

## Credits

fgsfds and contributors for the Perfect Dark port, the n64decomp team for the decompilation,
alechurri for the Ship of Harkinian PS4 port and its write-up, OsirizX for the Super Mario 64 PS4
port, flat_z and the orbisdev contributors for the Piglet research, and the OpenOrbis team.

## License

The scripts, CMake files, tests and documentation in this repository are MIT licensed (the toolchain
file, `compat/` and `zlib-cmake/` are adapted from shipofharkinian-ps4, also MIT). The patch modifies
the Perfect Dark port and is subject to its license. Not affiliated with or endorsed by Nintendo, Rare,
Microsoft or Sony.
