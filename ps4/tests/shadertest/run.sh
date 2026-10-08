#!/usr/bin/env bash
# Validates every shader the GLES2 backend can generate (sampled) as GLSL ES 1.00 with glslang.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../../env.sh"
PD="$PD_GAME_DIR"
cd "$HERE"
mkdir -p inc/orbis
cp -r "$OO_PS4_TOOLCHAIN/include/GLES2" "$OO_PS4_TOOLCHAIN/include/KHR" inc/
printf '#pragma once\n#include <stdbool.h>\n#include <GLES2/gl2.h>\n#include <GLES2/gl2ext.h>\n' > inc/orbis/Pigletv2VSH.h
python3 gen_stubs.py "$OO_PS4_TOOLCHAIN"
rm -rf out
clang++ -std=c++20 -O1 -w -D_LANGUAGE_C=1 -DGL_GLEXT_PROTOTYPES -Iinc -I"$PD/include" -I"$PD/include/PR" \
    -I"$PD/src/include" -I"$PD/port/include" -I"$PD/port/fast3d" -include stdint.h \
    harness.cpp glstubs.cpp "$PD/port/fast3d/gfx_cc.cpp" -o harness
./harness "${1:-3000}"
fail=0
for f in out/*; do
    if ! glslangValidator "$f" > "$f.log" 2>&1; then
        fail=$((fail + 1))
        [ $fail -le 3 ] && { echo "== $f"; cat "$f.log"; }
    fi
done
total=$(ls out/*.vert out/*.frag | wc -l)
echo "glslang (GLSL ES 1.00): $((total - fail)) / $total shaders valid"
[ $fail -eq 0 ]
