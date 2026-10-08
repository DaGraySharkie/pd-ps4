#!/usr/bin/env bash
# Configures and builds build/ps4/pd.x86_64.elf.  ROMID=ntsc-final (default), pal-final, jpn-final
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
ROMID="${ROMID:-ntsc-final}"
BUILD="$PS4_BUILD_ROOT/ps4"
[ "$ROMID" = ntsc-final ] || BUILD="$PS4_BUILD_ROOT/ps4-$ROMID"
cmake -G Ninja -S "$(_W "$PD_GAME_DIR")" -B "$(_W "$BUILD")" \
    -DCMAKE_TOOLCHAIN_FILE="$(_W "$HERE/cmake/ps4-toolchain.cmake")" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}" -DROMID="$ROMID"
cmake --build "$(_W "$BUILD")"
ls -la "$BUILD"/*.elf
