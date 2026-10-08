#!/usr/bin/env bash
# Updates this checkout to a new version of the PS4 patch:
#
#   ps4/update.sh path/to/perfect_dark-ps4.patch
#
# Puts the game code back to the upstream commit the patch is made against, then applies the
# patch. Kept: ps4/tools, ps4/prefix, ps4/deps, ps4/custom (your images), ps4/out, build/.
# Lost: any changes of your own to the game code or to the ps4/ scripts.
set -e

# The reset below replaces this very file; run from a copy so bash never reads a changed script.
if [ -z "$PD_PS4_UPDATE_COPY" ]; then
    tmp="$(mktemp /tmp/pd-ps4-update.XXXXXX)"
    cp "$0" "$tmp"
    PD_PS4_UPDATE_COPY="$tmp" PD_PS4_DIR="$(cd "$(dirname "$0")" && pwd)" exec bash "$tmp" "$@"
fi
trap 'rm -f "$PD_PS4_UPDATE_COPY"' EXIT

PD_COMMIT=32a1cb9f268dd3ac73016801025c6bbbfa20130f   # upstream commit the patch is made against
HERE="$PD_PS4_DIR"
GAME="$(cd "$HERE/.." && pwd)"

if [ -z "$1" ] || [ ! -f "$1" ]; then
    echo "usage: ps4/update.sh path/to/perfect_dark-ps4.patch"
    exit 1
fi

# read the patch before anything else: it may be sitting inside this checkout
PATCH="$(mktemp /tmp/pd-ps4-patch.XXXXXX)"
trap 'rm -f "$PD_PS4_UPDATE_COPY" "$PATCH"' EXIT
tr -d '\r' < "$1" > "$PATCH"   # in case it was saved through Windows
if ! grep -q '^diff --git a/ps4/update.sh' "$PATCH"; then
    echo "ERROR: $1 does not look like a perfect_dark-ps4.patch (no ps4/ folder in it)"
    exit 1
fi

source "$HERE/env.sh"
ps4_protect_local_dirs

cd "$GAME"
git rev-parse --git-dir >/dev/null 2>&1 || { echo "ERROR: $GAME is not a git checkout"; exit 1; }
if ! git cat-file -e "$PD_COMMIT^{commit}" 2>/dev/null; then
    echo "== fetching upstream commit ${PD_COMMIT:0:9}"
    git fetch -q origin "$PD_COMMIT" || git fetch -q --unshallow origin || true
    git cat-file -e "$PD_COMMIT^{commit}" || { echo "ERROR: commit $PD_COMMIT not available"; exit 1; }
fi

echo "== resetting the game code to upstream ${PD_COMMIT:0:9}"
git config core.autocrlf false
git rm -rq --cached .
git reset -q --hard "$PD_COMMIT"
git clean -fdq

echo "== applying the patch"
if ! git apply "$PATCH"; then
    echo "ERROR: the patch did not apply; the folder is now plain upstream code"
    exit 1
fi
chmod +x ps4/*.sh ps4/tests/shadertest/*.sh 2>/dev/null || true

echo "== done; next: ps4/build.sh && ps4/package.sh"
