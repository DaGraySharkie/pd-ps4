# Source this file before building:  source ps4/env.sh
#
# Layout (everything lives in the perfect_dark repository):
#   perfect_dark/                       the game
#   perfect_dark/ps4/                   these scripts
#   perfect_dark/ps4/tools/OpenOrbis/PS4Toolchain   OpenOrbis v0.5.4 (setup.sh downloads it)
#   perfect_dark/ps4/tools/llvm/bin     optional: LLVM 18 (otherwise clang from PATH or /usr/bin)
#   perfect_dark/ps4/tools/cmake-*/bin, ps4/tools/ninja   optional portable CMake / Ninja
#   perfect_dark/ps4/prefix, ps4/deps   zlib + SDL2 built by build-deps.sh
#   perfect_dark/ps4/custom/sce_sys/    your own icon0.png / pic0.png / pic1.png (kept on updates)
#   perfect_dark/build/ps4              build output (build.sh)
#   perfect_dark/ps4/out                finished .pkg (package.sh)
#
# Works in a Linux/WSL shell and in Git Bash / MSYS2 on Windows.
_PDPS4_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PDPS4_DIR="$_PDPS4_DIR"
export PD_GAME_DIR="$(cd "$_PDPS4_DIR/.." && pwd)"
export PS4_TOOLS_DIR="$_PDPS4_DIR/tools"
export PS4_BUILD_ROOT="$PD_GAME_DIR/build"

# Paths handed to native Windows programs (cmake.exe, clang.exe, ...) must be C:/style.
_W() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else echo "$1"; fi; }

_CMAKE_BIN="$(ls -d "$PS4_TOOLS_DIR"/cmake-*/bin 2>/dev/null | head -1)"
[ -n "$_CMAKE_BIN" ] && export PATH="$_CMAKE_BIN:$PATH"
[ -d "$PS4_TOOLS_DIR/ninja" ] && export PATH="$PS4_TOOLS_DIR/ninja:$PATH"
[ -d "$PS4_TOOLS_DIR/llvm/bin" ] && export PATH="$PS4_TOOLS_DIR/llvm/bin:$PATH"

export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$(_W "$PS4_TOOLS_DIR/OpenOrbis/PS4Toolchain")}"
export PS4_PREFIX="${PS4_PREFIX:-$(_W "$_PDPS4_DIR/prefix")}"
if [ -z "$PS4_LLVM_BIN" ]; then
    if [ -d "$PS4_TOOLS_DIR/llvm/bin" ]; then
        export PS4_LLVM_BIN="$(_W "$PS4_TOOLS_DIR/llvm/bin")"
    else
        _CLANG="$(command -v clang || true)"
        [ -n "$_CLANG" ] && export PS4_LLVM_BIN="$(_W "$(dirname "$(readlink -f "$_CLANG")")")"
    fi
fi

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) export OO_HOST_BIN="$OO_PS4_TOOLCHAIN/bin/windows"; export OO_EXE=".exe" ;;
    Darwin*)              export OO_HOST_BIN="$OO_PS4_TOOLCHAIN/bin/macos";   export OO_EXE="" ;;
    *)                    export OO_HOST_BIN="$OO_PS4_TOOLCHAIN/bin/linux";   export OO_EXE="" ;;
esac
# PkgTool.Core targets .NET Core 3.0; let it run on whatever newer runtime is installed.
export DOTNET_ROLL_FORWARD=LatestMajor
# PkgTool.Core aborts without ICU unless told to use invariant globalization.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

# Local, untracked folders that git must leave alone (also when update.sh resets the checkout).
# Written to .git/info/exclude, which no reset or patch touches.
ps4_protect_local_dirs() {
    local excl
    excl="$(git -C "$PD_GAME_DIR" rev-parse --git-path info/exclude 2>/dev/null)" || return 0
    case "$excl" in /*) ;; *) excl="$PD_GAME_DIR/$excl" ;; esac
    mkdir -p "$(dirname "$excl")"
    touch "$excl"
    grep -q "^# perfect dark ps4 local folders" "$excl" && return 0
    cat >> "$excl" <<'EXCL'
# perfect dark ps4 local folders
/ps4/tools/
/ps4/prefix/
/ps4/deps/
/ps4/custom/
/ps4/pkg/
/ps4/out/
/ps4/pd.oelf
/ps4/tests/shadertest/out/
/ps4/tests/shadertest/inc/
/ps4/tests/shadertest/harness
/ps4/tests/shadertest/glstubs.cpp
EXCL
}
