// Host harness: runs the GLES2 backend's real shader generator for a large set of combiner ids
// and options, writing every generated vertex/fragment shader to out/ for glslang validation.
#define PLATFORM_PS4 1
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
#include <set>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "glcapture.h"

std::vector<CapturedShader> g_captured;
static unsigned g_next_shader = 1;
static std::vector<unsigned> g_shader_types(1, 0);

extern "C" {
void sysLogPrintf(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void sysFatalError(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); fputc('\n', stderr); exit(2); }
GLuint GL_APIENTRY glCreateShader(GLenum type) { g_shader_types.push_back(type); return g_next_shader++; }
GLuint GL_APIENTRY glCreateProgram(void) { return 1000; }
void GL_APIENTRY glShaderSource(GLuint sh, GLsizei count, const GLchar *const *str, const GLint *len) {
    std::string s;
    for (GLsizei i = 0; i < count; i++) s.append(str[i], (len && len[i] >= 0) ? (size_t)len[i] : strlen(str[i]));
    g_captured.push_back({ g_shader_types[sh], s });
}
void GL_APIENTRY glGetShaderiv(GLuint, GLenum, GLint *p) { *p = 1; }
void GL_APIENTRY glGetProgramiv(GLuint, GLenum, GLint *p) { *p = 1; }
const GLubyte *GL_APIENTRY glGetString(GLenum) { return (const GLubyte *)""; }
void GL_APIENTRY glGetIntegerv(GLenum, GLint *p) { *p = 16; }
void GL_APIENTRY glGetShaderPrecisionFormat(GLenum, GLenum, GLint *r, GLint *p) { r[0] = r[1] = 127; *p = 23; }
GLint GL_APIENTRY glGetUniformLocation(GLuint, const GLchar *) { return 1; }
GLint GL_APIENTRY glGetAttribLocation(GLuint, const GLchar *) { return 1; }
GLenum GL_APIENTRY glCheckFramebufferStatus(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }
GLboolean GL_APIENTRY glIsEnabled(GLenum) { return 0; }
}

extern "C" { bool gfx_framebuffers_enabled = true; }

#include "../../../port/fast3d/gfx_gles2.cpp"

int main(int argc, char **argv) {
    const int count = argc > 1 ? atoi(argv[1]) : 3000;
    std::mt19937_64 rng(1234);
    gfx_gles2_api.init();
    std::set<std::pair<uint64_t, uint32_t>> done;
    int made = 0;
    for (int filt = 0; filt < 3; ++filt) {
        current_filter_mode = (FilteringMode)filt;
        for (int i = 0; i < count; ++i) {
            // only the items gfx_pc.cpp can encode and the generator knows: 0, INPUT_1..4,
            // TEXEL0/0A/1/1A, 1, COMBINED, NOISE
            static const uint8_t items[] = { 0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 13, 14 };
            uint64_t id0 = 0;
            for (int k = 0; k < 16; ++k) id0 |= (uint64_t)items[rng() % sizeof(items)] << (k * 4);
            // half of the time: plausible single-cycle modes (second cycle mirrors the first)
            if (i & 1) id0 = (id0 & 0xffffffffull) | ((id0 & 0xffffffffull) << 32);
            uint32_t id1 = (uint32_t)rng() & 0x1fff;
            if (!done.insert({ id0, id1 }).second) continue;
            gfx_opengl_clear_shaders();
            gfx_gles2_api.create_and_load_new_shader(id0, id1);
            ++made;
        }
    }
    // the blit program, too
    system("mkdir -p out");
    int n = 0;
    for (auto &c : g_captured) {
        char name[64];
        snprintf(name, sizeof(name), "out/s%05d.%s", n++, c.type == GL_VERTEX_SHADER ? "vert" : "frag");
        FILE *f = fopen(name, "w");
        fwrite(c.src.data(), 1, c.src.size(), f);
        fclose(f);
    }
    printf("%d programs, %zu shaders written\n", made, g_captured.size());
    return 0;
}
