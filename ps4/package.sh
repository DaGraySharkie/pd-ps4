#!/usr/bin/env bash
# Turns build/ps4/pd.x86_64.elf into an installable PS4 package (ps4/out/*.pkg).
#
#   ps4/package.sh [path/to/pd.x86_64.elf]
#
# Optional, for a self-contained personal install (never share such a package):
#   BUNDLE_SPRX_DIR=<dir with libScePigletv2VSH.sprx + libSceShaccVSH.sprx>
#
# Images: ps4/custom/sce_sys/<name> if you put one there (kept when updating), otherwise the
# default from ps4/pkg-static/sce_sys/:
#   sce_sys/icon0.png   512x512    home screen icon (required)
#   sce_sys/pic1.png    1920x1080  launch splash, shown by the system until the first game frame
#   sce_sys/pic0.png    1920x1080  home screen background
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/env.sh"

TITLE="${PKG_TITLE:-Perfect Dark}"
VERSION="01.00"
TITLE_ID="${PKG_TITLE_ID:-PDRK00001}"
CONTENT_ID="IV0000-${TITLE_ID}_00-PERFECTDARKPORT0"
# System auth info + program id of the OpenOrbis Piglet sample: required to load Piglet.
AUTHINFO="000000000000000000000000001C004000FF000000000080000000000000000000000000000000000000008000400040000000000000008000000000000000080040FFFF000000F000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
PAID="0x3800000000000035"

OO="$OO_PS4_TOOLCHAIN"
BIN="$OO_HOST_BIN"
ELF="$(readlink -f "${1:-$PS4_BUILD_ROOT/ps4/pd.x86_64.elf}")"
STAGE="$HERE/pkg"
OUT="${OUT_DIR:-$HERE/out}"

[ -f "$ELF" ] || { echo "missing $ELF, build first"; exit 1; }

rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys/about" "$STAGE/sce_module" "$OUT"
# your own image if there is one, else the default; prints nothing if neither exists
image() {
    if [ -f "$HERE/custom/sce_sys/$1" ]; then echo "$HERE/custom/sce_sys/$1"
    elif [ -f "$HERE/pkg-static/sce_sys/$1" ]; then echo "$HERE/pkg-static/sce_sys/$1"; fi
}
ICON0="$(image icon0.png)"
cp "$ICON0" "$STAGE/sce_sys/icon0.png"
cp "$OO/samples/piglet/sce_sys/about/right.sprx" "$STAGE/sce_sys/about/right.sprx"
cp "$OO/samples/piglet/sce_module/libc.prx" "$OO/samples/piglet/sce_module/libSceFios2.prx" "$STAGE/sce_module/"

EXTRA_FILES=""

# Checks a PNG header: size and that it is RGB without alpha, which the system wants for its
# images. Only warns: the package still builds.
check_png() { # file width height
    python3 - "$1" "$2" "$3" <<'PYEOF'
import struct, sys
path, want_w, want_h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
data = open(path, 'rb').read(33)
if data[:8] != b'\x89PNG\r\n\x1a\n':
    print(f'WARNING: {path} is not a PNG file'); sys.exit()
w, h = struct.unpack('>II', data[16:24])
depth, ctype = data[24], data[25]
if (w, h) != (want_w, want_h):
    print(f'WARNING: {path} is {w}x{h}, the system expects {want_w}x{want_h}')
if ctype != 2 or depth != 8:
    print(f'WARNING: {path} should be a 24-bit RGB PNG without alpha (colour type {ctype}, depth {depth})')
PYEOF
}

SYS_IMAGES=""
for pic in pic0 pic1; do
    PIC="$(image $pic.png)"
    if [ -n "$PIC" ]; then
        echo "== $pic.png: $PIC"
        check_png "$PIC" 1920 1080
        cp "$PIC" "$STAGE/sce_sys/$pic.png"
        SYS_IMAGES="$SYS_IMAGES sce_sys/$pic.png"
    fi
done
echo "== icon0.png: $ICON0"
check_png "$ICON0" 512 512
if [ -n "$BUNDLE_SPRX_DIR" ]; then
    mkdir -p "$STAGE/assets/misc"
    cp "$BUNDLE_SPRX_DIR/libScePigletv2VSH.sprx" "$BUNDLE_SPRX_DIR/libSceShaccVSH.sprx" "$STAGE/assets/misc/"
    EXTRA_FILES=" assets/misc/libScePigletv2VSH.sprx assets/misc/libSceShaccVSH.sprx"
fi

cd "$STAGE"

"$BIN/create-fself$OO_EXE" -in="$(_W "$ELF")" -out="$(_W "$STAGE/../pd.oelf")" \
    --eboot "eboot.bin" --paid "$PAID" --authinfo "$AUTHINFO"

SFO=sce_sys/param.sfo
P="$BIN/PkgTool.Core$OO_EXE"
"$P" sfo_new $SFO
"$P" sfo_setentry $SFO APP_TYPE --type Integer --maxsize 4 --value 1
"$P" sfo_setentry $SFO APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
"$P" sfo_setentry $SFO ATTRIBUTE --type Integer --maxsize 4 --value 0
"$P" sfo_setentry $SFO CATEGORY --type Utf8 --maxsize 4 --value "gde"
"$P" sfo_setentry $SFO FORMAT --type Utf8 --maxsize 4 --value "obs"
"$P" sfo_setentry $SFO CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
"$P" sfo_setentry $SFO DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$P" sfo_setentry $SFO SYSTEM_VER --type Integer --maxsize 4 --value 1020
"$P" sfo_setentry $SFO TITLE --type Utf8 --maxsize 128 --value "$TITLE"
"$P" sfo_setentry $SFO TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
"$P" sfo_setentry $SFO VERSION --type Utf8 --maxsize 8 --value "$VERSION"

"$BIN/create-gp4$OO_EXE" -out pkg.gp4 --content-id="$CONTENT_ID" \
    --files "eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png$SYS_IMAGES sce_module/libc.prx sce_module/libSceFios2.prx$EXTRA_FILES"

"$P" pkg_build pkg.gp4 "$(_W "$OUT")"
ls -la "$OUT"
