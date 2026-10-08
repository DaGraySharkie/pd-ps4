#!/usr/bin/env bash
# One-time setup, run from anywhere: downloads OpenOrbis v0.5.4 (LLVM 18 build) to ps4/tools/
# and tells git to leave the PS4 folders alone. LLVM 18 (clang, clang++, ld.lld, llvm-ar) must be
# installed, or placed in ps4/tools/llvm/bin.
#
# Next: ps4/build-deps.sh && ps4/build.sh && ps4/package.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"

ps4_protect_local_dirs

if [ ! -d "$PS4_TOOLS_DIR/OpenOrbis/PS4Toolchain" ]; then
    echo "== downloading OpenOrbis v0.5.4"
    mkdir -p "$PS4_TOOLS_DIR"
    curl -fL -o "$PS4_TOOLS_DIR/oo.tar.gz" \
        https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz
    tar xzf "$PS4_TOOLS_DIR/oo.tar.gz" -C "$PS4_TOOLS_DIR"
    rm "$PS4_TOOLS_DIR/oo.tar.gz"
fi

command -v clang >/dev/null || [ -x "$PS4_TOOLS_DIR/llvm/bin/clang" ] || [ -x "$PS4_TOOLS_DIR/llvm/bin/clang.exe" ] \
    || echo "WARNING: no clang found (LLVM 18 needed)"
echo "setup done; next: ps4/build-deps.sh && ps4/build.sh && ps4/package.sh"
