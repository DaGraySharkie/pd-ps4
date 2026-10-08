#!/usr/bin/env bash
# Cross-builds the two third-party dependencies of the Perfect Dark port (zlib, SDL2) into
# ps4/prefix. SDL2 is only used for events, timing, threads and the game controller
# API (with a virtual joystick fed from scePad); video goes through Piglet, audio through
# sceAudioOut.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"
SRC="$HERE/deps/src"; BLD="$HERE/deps/build"; PREFIX="$PS4_PREFIX"
TC="$HERE/cmake/ps4-toolchain.cmake"
mkdir -p "$SRC" "$BLD" "$PREFIX"

fetch() { # name url tag
    if [ ! -d "$SRC/$1" ]; then
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$2" "$SRC/$1"
    fi
}
build() { # name srcdir [cmake args...]
    local name="$1" src="$2"; shift 2
    if [ -f "$BLD/$name/.done" ]; then echo "== $name (cached)"; return; fi
    echo "== $name"
    cmake -G Ninja -S "$(_W "$src")" -B "$(_W "$BLD/$name")" -DCMAKE_TOOLCHAIN_FILE="$(_W "$TC")" \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$(_W "$PREFIX")" -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@" > "$BLD/$name.configure.log" 2>&1 \
        || { tail -40 "$BLD/$name.configure.log"; exit 1; }
    cmake --build "$(_W "$BLD/$name")" > "$BLD/$name.build.log" 2>&1 || { tail -60 "$BLD/$name.build.log"; exit 1; }
    cmake --install "$(_W "$BLD/$name")" > "$BLD/$name.install.log" 2>&1 || { tail -40 "$BLD/$name.install.log"; exit 1; }
    touch "$BLD/$name/.done"
}

fetch zlib https://github.com/madler/zlib.git v1.3.1
build zlib "$HERE/zlib-cmake" -DZLIB_SRC="$(_W "$SRC/zlib")"

fetch SDL https://github.com/libsdl-org/SDL.git release-2.30.9
build SDL "$SRC/SDL" -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL2_DISABLE_SDL2MAIN=ON \
    -DSDL2_DISABLE_INSTALL=OFF -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF \
    -DSDL_VULKAN=OFF -DSDL_KMSDRM=OFF -DSDL_RPI=OFF -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF \
    -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=OFF -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF \
    -DSDL_FUSIONSOUND=OFF -DSDL_LIBSAMPLERATE=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_FCITX=OFF \
    -DSDL_LIBUDEV=OFF -DSDL_HIDAPI_LIBUSB=OFF -DSDL_RPATH=OFF -DSDL_DUMMYVIDEO=ON -DSDL_DUMMYAUDIO=ON \
    -DSDL_DISKAUDIO=OFF -DSDL_OFFSCREEN=OFF -DSDL_LOADSO=OFF -DSDL_DIRECTFB=OFF -DSDL_SYSTEM_ICONV=OFF \
    -DSDL_LIBICONV=OFF -DSDL_CCACHE=OFF -DSDL_FILESYSTEM=OFF -DSDL_VIRTUAL_JOYSTICK=ON -DSDL_HIDAPI=ON \
    -DSDL_HIDAPI_JOYSTICK=OFF -DSDL_WERROR=OFF

echo "ALL DEPS OK"
