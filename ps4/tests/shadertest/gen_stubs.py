#!/usr/bin/env python3
# Generates no-op definitions for every GLES2 entry point declared in the toolchain's gl2.h /
# gl2ext.h, so gfx_gles2.cpp can be compiled and driven on the host.
import re, sys
oo = sys.argv[1]
out = ['#include <GLES2/gl2.h>', '#include <GLES2/gl2ext.h>', '#include "glcapture.h"', 'extern "C" {']
seen = set()
for hdr in ('GLES2/gl2.h', 'GLES2/gl2ext.h'):
    src = open(f'{oo}/include/{hdr}').read()
    for m in re.finditer(r'GL_APICALL[ \t]+([^\n#]+?)[ \t]*GL_APIENTRY[ \t]+(gl\w+)[ \t]*\(([^;#]*?)\);', src):
        ret, name, args = m.group(1).strip(), m.group(2), ' '.join(m.group(3).split())
        if name in seen or name in ('glShaderSource', 'glGetShaderiv', 'glGetProgramiv', 'glGetString',
                                    'glCreateShader', 'glCreateProgram', 'glGetIntegerv', 'glGetShaderPrecisionFormat',
                                    'glGetUniformLocation', 'glGetAttribLocation', 'glCheckFramebufferStatus', 'glIsEnabled'):
            continue
        seen.add(name)
        body = '' if ret == 'void' else ' return 0;'
        out.append(f'{ret} GL_APIENTRY {name}({args}) {{{body}}}')
out.append('}')
open('glstubs.cpp', 'w').write('\n'.join(out) + '\n')
