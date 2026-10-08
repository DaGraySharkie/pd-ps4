// OpenGL ES 2.0 rendering backend, derived from gfx_opengl.cpp for Piglet (PS4), which only
// provides GLES 2.0 / GLSL ES 1.00. Differences from the desktop backend:
//  - shaders are GLSL ES 1.00; textureSize() is replaced by uTexSize0/1 uniforms, no bitwise
//    integer ops, no int-selector mix(), no "f" float suffixes
//  - no VAOs, no MSAA, unsized texture formats, 16- or 24-bit depth renderbuffers
//  - no glBlitFramebuffer: copies and the final present are textured quad draws
#include "platform.h"

#ifdef PLATFORM_PS4

#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include <map>
#include <unordered_map>
#include <vector>


#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include <orbis/Pigletv2VSH.h> // GLES2 + EGL headers with Piglet prototypes
#include "system.h"

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_pc.h"

using namespace std;

struct ShaderProgram {
    GLuint opengl_program_id;
    uint8_t num_inputs;
    bool used_textures[SHADER_MAX_TEXTURES];
    uint8_t num_floats;
    GLint attrib_locations[16];
    uint8_t attrib_sizes[16];
    uint8_t num_attribs;
    GLint frame_count_location;
    GLint noise_scale_location;
    GLint three_point_filter_locations[2];
    GLint tex_size_locations[2];
    float tex_size_set[2][2];
};

struct Framebuffer {
    uint32_t width, height;
    bool has_depth_buffer;
    uint32_t msaa_level;
    bool invert_y;

    GLuint fbo, clrbuf, rbo;
};

static std::map<pair<uint64_t, uint32_t>, struct ShaderProgram> shader_program_pool;
static GLuint opengl_vbo;
static GLuint opengl_vao;
static bool current_depth_mask;

static uint32_t frame_count;

static std::vector<Framebuffer> framebuffers;
static size_t current_framebuffer;
static float current_noise_scale;
static int current_anisotropy_level;
static FilteringMode current_filter_mode = FILTER_LINEAR;
static MipmapFilteringMode current_mipmap_filter_mode = MIPMAP_LINEAR;
static bool current_textures_linear_filter[2] = {false, false};

static GLenum gl_mirror_clamp = GL_MIRRORED_REPEAT;
static bool gl_npot_full = false;
static bool gl_has_aniso = false;
static bool gl_has_depth24 = false;
static bool gl_highp_fs = false;
static GLenum gl_depth_format = GL_DEPTH_COMPONENT16;

// state the blit helper has to put back
static struct ShaderProgram* current_prg;
static GLuint bound_tex[2];
static int active_tile = -1;
static int cur_viewport[4];
static int cur_scissor[4];
static std::unordered_map<GLuint, std::pair<uint32_t, uint32_t>> tex_sizes;

static int gfx_opengl_get_max_texture_size() {
    GLint max_texture_size;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    return max_texture_size;
}

static const char* gfx_opengl_get_name() {
    return "OpenGL";
}

static struct GfxClipParameters gfx_opengl_get_clip_parameters(void) {
    return { false, framebuffers[current_framebuffer].invert_y };
}

static void gfx_opengl_vertex_array_set_attribs(struct ShaderProgram* prg) {
    size_t num_floats = prg->num_floats;
    size_t pos = 0;

    for (int i = 0; i < prg->num_attribs; i++) {
        if (prg->attrib_locations[i] >= 0) {
            glEnableVertexAttribArray(prg->attrib_locations[i]);
            glVertexAttribPointer(prg->attrib_locations[i], prg->attrib_sizes[i], GL_FLOAT, GL_FALSE,
                                num_floats * sizeof(float), (void*)(pos * sizeof(float)));
        }
        pos += prg->attrib_sizes[i];
    }
}

static void gfx_opengl_set_uniforms(struct ShaderProgram* prg) {
    if (prg->frame_count_location >= 0) {
        glUniform1i(prg->frame_count_location, frame_count);
    }
    if (prg->noise_scale_location >= 0) {
        glUniform1f(prg->noise_scale_location, current_noise_scale);
    }
    if (prg->three_point_filter_locations[0] >= 0) {
        glUniform1i(prg->three_point_filter_locations[0], current_textures_linear_filter[0]);
    }
    if (prg->three_point_filter_locations[1] >= 0) {
        glUniform1i(prg->three_point_filter_locations[1], current_textures_linear_filter[1]);
    }
}

static void gfx_opengl_unload_shader(struct ShaderProgram* old_prg) {
    if (old_prg != NULL) {
        for (int i = 0; i < old_prg->num_attribs; i++) {
            if (old_prg->attrib_locations[i] >= 0) {
                glDisableVertexAttribArray(old_prg->attrib_locations[i]);
            }
        }
    }
}

static void gfx_opengl_load_shader(struct ShaderProgram* new_prg) {
    // if (!new_prg) return;
    glUseProgram(new_prg->opengl_program_id);
    current_prg = new_prg;
    gfx_opengl_vertex_array_set_attribs(new_prg);
    gfx_opengl_set_uniforms(new_prg);
}

static void append_str(char* buf, size_t* len, const char* str) {
    while (*str != '\0') {
        buf[(*len)++] = *str++;
    }
}

static void append_line(char* buf, size_t* len, const char* str) {
    while (*str != '\0') {
        buf[(*len)++] = *str++;
    }
    buf[(*len)++] = '\n';
}

#define RAND_NOISE "((random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + 1.0) / 2.0)"

static const char* shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha,
                                      bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "vec4(0.0, 0.0, 0.0, 0.0)" : "vec3(0.0, 0.0, 0.0)";
            case SHADER_1:
                return with_alpha ? "vec4(1.0, 1.0, 1.0, 1.0)" : "vec3(1.0, 1.0, 1.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "vInput1" : "vInput1.rgb";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "vInput2" : "vInput2.rgb";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "vInput3" : "vInput3.rgb";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "vInput4" : "vInput4.rgb";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.rgb";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.a"
                                           : (with_alpha ? "vec4(texVal0.a, texVal0.a, texVal0.a, texVal0.a)"
                                                         : "vec3(texVal0.a, texVal0.a, texVal0.a)");
            case SHADER_TEXEL1A:
                return hint_single_element ? "texVal1.a"
                                           : (with_alpha ? "vec4(texVal1.a, texVal1.a, texVal1.a, texVal1.a)"
                                                         : "vec3(texVal1.a, texVal1.a, texVal1.a)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.rgb";
            case SHADER_COMBINED:
                return with_alpha ? "texel" : "texel.rgb";
            case SHADER_NOISE:
                return with_alpha ? "vec4(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")"
                                  : "vec3(" RAND_NOISE ", " RAND_NOISE ", " RAND_NOISE ")";
        }
    } else {
        switch (item) {
            case SHADER_0:
                return "0.0";
            case SHADER_1:
                return "1.0";
            case SHADER_INPUT_1:
                return "vInput1.a";
            case SHADER_INPUT_2:
                return "vInput2.a";
            case SHADER_INPUT_3:
                return "vInput3.a";
            case SHADER_INPUT_4:
                return "vInput4.a";
            case SHADER_TEXEL0:
                return "texVal0.a";
            case SHADER_TEXEL0A:
                return "texVal0.a";
            case SHADER_TEXEL1A:
                return "texVal1.a";
            case SHADER_TEXEL1:
                return "texVal1.a";
            case SHADER_COMBINED:
                return "texel.a";
            case SHADER_NOISE:
                return RAND_NOISE;
        }
    }
    return "";
}

#undef RAND_NOISE

static void append_formula(char* buf, size_t* len, uint8_t c[2][4], bool do_single, bool do_multiply, bool do_mix,
                           bool with_alpha, bool only_alpha, bool opt_alpha) {
    if (do_single) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        append_str(buf, len, "mix(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ", ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, ")");
    } else {
        append_str(buf, len, "(");
        append_str(buf, len, shader_item_to_str(c[only_alpha][0], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, " - ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][1], with_alpha, only_alpha, opt_alpha, false));
        append_str(buf, len, ") * ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][2], with_alpha, only_alpha, opt_alpha, true));
        append_str(buf, len, " + ");
        append_str(buf, len, shader_item_to_str(c[only_alpha][3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static struct ShaderProgram* gfx_opengl_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1) {
    struct CCFeatures cc_features = { 0 };
    gfx_cc_get_features(shader_id0, shader_id1, &cc_features);

    char vs_buf[2048];
    char fs_buf[8192];
    size_t vs_len = 0;
    size_t fs_len = 0;
    size_t num_floats = 4;

    // Vertex shader

    append_line(vs_buf, &vs_len, "#version 100");

    append_line(vs_buf, &vs_len, "precision highp float;");

    if (false) { // GLSL ES 1.00
        append_line(vs_buf, &vs_len, "#define INPUT in");
        append_line(vs_buf, &vs_len, "#define OUTPUT out");
    } else {
        append_line(vs_buf, &vs_len, "#define INPUT attribute");
        append_line(vs_buf, &vs_len, "#define OUTPUT varying");
    }

    append_line(vs_buf, &vs_len, "INPUT vec4 aVtxPos;");

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            vs_len += sprintf(vs_buf + vs_len, "INPUT vec2 aTexCoord%d;\n", i);
            vs_len += sprintf(vs_buf + vs_len, "OUTPUT vec2 vTexCoord%d;\n", i);
            num_floats += 2;
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    vs_len += sprintf(vs_buf + vs_len, "INPUT float aTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
                    vs_len += sprintf(vs_buf + vs_len, "OUTPUT float vTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
                    num_floats += 1;
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "INPUT vec4 aFog;");
        append_line(vs_buf, &vs_len, "OUTPUT vec4 vFog;");
        num_floats += 4;
    }

    if (cc_features.opt_grayscale) {
        append_line(vs_buf, &vs_len, "INPUT vec4 aGrayscaleColor;");
        append_line(vs_buf, &vs_len, "OUTPUT vec4 vGrayscaleColor;");
        num_floats += 4;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "INPUT vec%d aInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        vs_len += sprintf(vs_buf + vs_len, "OUTPUT vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
        num_floats += cc_features.opt_alpha ? 4 : 3;
    }

    append_line(vs_buf, &vs_len, "void main() {");
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            vs_len += sprintf(vs_buf + vs_len, "    vTexCoord%d = aTexCoord%d;\n", i, i);
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    vs_len += sprintf(vs_buf + vs_len, "    vTexClamp%s%d = aTexClamp%s%d;\n", j == 0 ? "S" : "T", i,
                                      j == 0 ? "S" : "T", i);
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        append_line(vs_buf, &vs_len, "    vFog = aFog;");
    }
    if (cc_features.opt_grayscale) {
        append_line(vs_buf, &vs_len, "    vGrayscaleColor = aGrayscaleColor;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        vs_len += sprintf(vs_buf + vs_len, "    vInput%d = aInput%d;\n", i + 1, i + 1);
    }

    append_line(vs_buf, &vs_len, "    gl_Position = aVtxPos;");
    // HACK: workaround for no GL_DEPTH_CLAMP (never available in GLES2)
    append_line(vs_buf, &vs_len, "    gl_Position.z *= 0.3;");
    append_line(vs_buf, &vs_len, "}");

    // Fragment shader

    append_line(fs_buf, &fs_len, "#version 100");

    append_line(fs_buf, &fs_len, gl_highp_fs ? "precision highp float;" : "precision mediump float;");

    if (false) { // GLSL ES 1.00
        append_line(fs_buf, &fs_len, "#define INPUT in");
        append_line(fs_buf, &fs_len, "#define OUTPUT_COLOR outColor");
        append_line(fs_buf, &fs_len, "#define SAMPLE_TEX(tex, uv) texture(tex, uv)");
    } else {
        append_line(fs_buf, &fs_len, "#define INPUT varying");
        append_line(fs_buf, &fs_len, "#define OUTPUT_COLOR gl_FragColor");
        append_line(fs_buf, &fs_len, "#define SAMPLE_TEX(tex, uv) texture2D(tex, uv)");
    }

    // Reference approach to color wrapping as per GLideN64
    // Return wrapped value of x in interval [low, high)
    append_line(fs_buf, &fs_len, "#define WRAP(x, low, high) mod((x)-(low), (high)-(low)) + (low)");

    append_line(fs_buf, &fs_len, "#define TEX_OFFSET(tex, uv, texSize, off) SAMPLE_TEX(tex, uv - (off)/texSize)");

    // append_line(fs_buf, &fs_len, "precision mediump float;");
    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            fs_len += sprintf(fs_buf + fs_len, "INPUT vec2 vTexCoord%d;\n", i);
            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    fs_len += sprintf(fs_buf + fs_len, "INPUT float vTexClamp%s%d;\n", j == 0 ? "S" : "T", i);
                }
            }
        }
    }
    if (cc_features.opt_fog) {
        append_line(fs_buf, &fs_len, "INPUT vec4 vFog;");
    }
    if (cc_features.opt_grayscale) {
        append_line(fs_buf, &fs_len, "INPUT vec4 vGrayscaleColor;");
    }
    for (int i = 0; i < cc_features.num_inputs; i++) {
        fs_len += sprintf(fs_buf + fs_len, "INPUT vec%d vInput%d;\n", cc_features.opt_alpha ? 4 : 3, i + 1);
    }

    if (cc_features.used_textures[0]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex0;");
        append_line(fs_buf, &fs_len, "uniform vec2 uTexSize0;");
        if (current_filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "uniform int three_point_filter0;");
    }
    if (cc_features.used_textures[1]) {
        append_line(fs_buf, &fs_len, "uniform sampler2D uTex1;");
        append_line(fs_buf, &fs_len, "uniform vec2 uTexSize1;");
        if (current_filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "uniform int three_point_filter1;");
    }

    append_line(fs_buf, &fs_len, "uniform int frame_count;");
    append_line(fs_buf, &fs_len, "uniform float noise_scale;");

    append_line(fs_buf, &fs_len, "float random(in vec3 value) {");
    append_line(fs_buf, &fs_len, "    float random = dot(sin(value), vec3(12.9898, 78.233, 37.719));");
    append_line(fs_buf, &fs_len, "    return fract(sin(random) * 143758.5453);");
    append_line(fs_buf, &fs_len, "}");

    if (current_filter_mode == FILTER_THREE_POINT) {
        append_line(fs_buf, &fs_len, "vec4 filter3point(in sampler2D tex, in vec2 texCoord, in vec2 texSize) {");
        append_line(fs_buf, &fs_len, "    vec2 offset = fract(texCoord*texSize - vec2(0.5));");
        append_line(fs_buf, &fs_len, "    offset -= step(1.0, offset.x + offset.y);");
        append_line(fs_buf, &fs_len, "    vec4 c0 = TEX_OFFSET(tex, texCoord, texSize, offset);");
        append_line(fs_buf, &fs_len, "    vec4 c1 = TEX_OFFSET(tex, texCoord, texSize, vec2(offset.x - sign(offset.x), offset.y));");
        append_line(fs_buf, &fs_len, "    vec4 c2 = TEX_OFFSET(tex, texCoord, texSize, vec2(offset.x, offset.y - sign(offset.y)));");
        append_line(fs_buf, &fs_len, "    return c0 + abs(offset.x)*(c1-c0) + abs(offset.y)*(c2-c0);");
        append_line(fs_buf, &fs_len, "}");
    }

    if (cc_features.opt_blur) {
        // blur filter, used for menu backgrounds
        // used to be two for loops from 0 to 4, but apparently intel drivers crashed trying to unroll it
        // used to have a const weight array, but apparently drivers for the GT620 don't like const array initializers

        if (current_filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "lowp vec4 hookTexture2D(in sampler2D t, in vec2 uv, in vec2 texSize, in int three_point_filter) {");
        else
            append_line(fs_buf, &fs_len, "lowp vec4 hookTexture2D(in sampler2D t, in vec2 uv, in vec2 texSize) {");

        append_line(fs_buf, &fs_len, "    lowp vec4 cw = vec4(0.0);");
        append_line(fs_buf, &fs_len, "    for (int i = 0; i < 16; ++i) {");
        append_line(fs_buf, &fs_len, "        vec2 xy = vec2(mod(float(i), 4.0), floor(float(i) / 4.0));");
        append_line(fs_buf, &fs_len, "        lowp float w = 0.009947 - length(xy) * 0.001;");
        append_line(fs_buf, &fs_len, "        vec2 scaled_uv = uv + (vec2(-1.5) + xy) / texSize;");

        if (current_filter_mode == FILTER_THREE_POINT)
            append_line(fs_buf, &fs_len, "        lowp vec4 tex = mix(SAMPLE_TEX(t, scaled_uv), filter3point(t, scaled_uv, texSize), float(three_point_filter));");
        else
            append_line(fs_buf, &fs_len, "        lowp vec4 tex = SAMPLE_TEX(t, scaled_uv);");

        append_line(fs_buf, &fs_len, "        cw += vec4(tex.rgb * w, w);");
        append_line(fs_buf, &fs_len, "    }");
        append_line(fs_buf, &fs_len, "    return vec4(cw.rgb / cw.a, 1.0);");
        append_line(fs_buf, &fs_len, "}");
    } else {
        if (current_filter_mode == FILTER_THREE_POINT) {
            append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D tex, in vec2 uv, in vec2 texSize, in int three_point_filter) {");
            append_line(fs_buf, &fs_len, "    return mix(SAMPLE_TEX(tex, uv), filter3point(tex, uv, texSize), float(three_point_filter));");
            append_line(fs_buf, &fs_len, "}");
        } else {
            append_line(fs_buf, &fs_len, "vec4 hookTexture2D(in sampler2D tex, in vec2 uv, in vec2 texSize) {");
            append_line(fs_buf, &fs_len, "    return SAMPLE_TEX(tex, uv);");
            append_line(fs_buf, &fs_len, "}");
        }
    }

    if (false) { // GLSL ES 1.00
        append_line(fs_buf, &fs_len, "out vec4 outColor;");
    }

    append_line(fs_buf, &fs_len, "void main() {");

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            bool s = cc_features.clamp[i][0], t = cc_features.clamp[i][1];

            fs_len += sprintf(fs_buf + fs_len, "    vec2 texSize%d = uTexSize%d;\n", i, i);

            if (!s && !t) {
                fs_len += sprintf(fs_buf + fs_len, "    vec2 vTexCoordAdj%d = vTexCoord%d;\n", i, i);
            } else {
                if (s && t) {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "    vec2 vTexCoordAdj%d = clamp(vTexCoord%d, 0.5 / texSize%d, "
                                      "vec2(vTexClampS%d, vTexClampT%d));\n",
                                      i, i, i, i, i);
                } else if (s) {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "    vec2 vTexCoordAdj%d = vec2(clamp(vTexCoord%d.s, 0.5 / "
                                      "texSize%d.s, vTexClampS%d), vTexCoord%d.t);\n",
                                      i, i, i, i, i);
                } else {
                    fs_len += sprintf(fs_buf + fs_len,
                                      "    vec2 vTexCoordAdj%d = vec2(vTexCoord%d.s, clamp(vTexCoord%d.t, "
                                      "0.5 / texSize%d.t, vTexClampT%d));\n",
                                      i, i, i, i, i);
                }
            }

            if (current_filter_mode == FILTER_THREE_POINT)
                fs_len += sprintf(fs_buf + fs_len, "    vec4 texVal%d = hookTexture2D(uTex%d, vTexCoordAdj%d, texSize%d, three_point_filter%d);\n", i, i, i, i, i);
            else
                fs_len += sprintf(fs_buf + fs_len, "    vec4 texVal%d = hookTexture2D(uTex%d, vTexCoordAdj%d, texSize%d);\n", i, i, i, i);
        }
    }

    append_line(fs_buf, &fs_len, cc_features.opt_alpha ? "    vec4 texel;" : "    vec3 texel;");
    for (int c = 0; c < (cc_features.opt_2cyc ? 2 : 1); c++) {
        append_str(fs_buf, &fs_len, "    texel = ");
        if (!cc_features.color_alpha_same[c] && cc_features.opt_alpha) {
            append_str(fs_buf, &fs_len, "vec4(");
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][0],
                           cc_features.do_multiply[c][0], cc_features.do_mix[c][0], false, false, true);
            append_str(fs_buf, &fs_len, ", ");
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][1],
                           cc_features.do_multiply[c][1], cc_features.do_mix[c][1], true, true, true);
            append_str(fs_buf, &fs_len, ")");
        } else {
            append_formula(fs_buf, &fs_len, cc_features.c[c], cc_features.do_single[c][0],
                           cc_features.do_multiply[c][0], cc_features.do_mix[c][0], cc_features.opt_alpha, false,
                           cc_features.opt_alpha);
        }
        append_line(fs_buf, &fs_len, ";");

        if (c == 0) {
            append_line(fs_buf, &fs_len, "    texel = WRAP(texel, -1.01, 1.01);");
        }
    }

    append_line(fs_buf, &fs_len, "    texel = WRAP(texel, -0.51, 1.51);");
    append_line(fs_buf, &fs_len, "    texel = clamp(texel, 0.0, 1.0);");
    // TODO discard if alpha is 0?
    if (cc_features.opt_fog) {
        if (cc_features.opt_alpha) {
            append_line(fs_buf, &fs_len, "    texel = vec4(mix(texel.rgb, vFog.rgb, vFog.a), texel.a);");
        } else {
            append_line(fs_buf, &fs_len, "    texel = mix(texel, vFog.rgb, vFog.a);");
        }
    }

    if (cc_features.opt_texture_edge && cc_features.opt_alpha) {
        append_line(fs_buf, &fs_len, "    if (texel.a > 0.19) texel.a = 1.0; else discard;");
    }

    if (cc_features.opt_alpha && cc_features.opt_noise) {
        append_line(fs_buf, &fs_len,
                    "    texel.a *= floor(clamp(random(vec3(floor(gl_FragCoord.xy * noise_scale), float(frame_count))) + "
                    "texel.a, 0.0, 1.0));");
    }

    if (cc_features.opt_grayscale) {
        append_line(fs_buf, &fs_len, "    float intensity = (texel.r + texel.g + texel.b) / 3.0;");
        append_line(fs_buf, &fs_len, "    vec3 new_texel = vGrayscaleColor.rgb * intensity;");
        append_line(fs_buf, &fs_len, "    texel.rgb = mix(texel.rgb, new_texel, vGrayscaleColor.a);");
    }

    if (cc_features.opt_alpha) {
        if (cc_features.opt_alpha_threshold) {
            append_line(fs_buf, &fs_len, "    if (texel.a < 8.0 / 256.0) discard;");
        }
        if (cc_features.opt_invisible) {
            append_line(fs_buf, &fs_len, "    texel.a = 0.0;");
        }

        append_line(fs_buf, &fs_len, "    OUTPUT_COLOR = texel;");
    } else {
        append_line(fs_buf, &fs_len, "    OUTPUT_COLOR = vec4(texel, 1.0);");
    }

    append_line(fs_buf, &fs_len, "}");

    vs_buf[vs_len] = '\0';
    fs_buf[fs_len] = '\0';

    const GLchar* sources[2] = { vs_buf, fs_buf };
    const GLint lengths[2] = { (GLint)vs_len, (GLint)fs_len };
    GLint success;

    GLuint vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &sources[0], &lengths[0]);
    glCompileShader(vertex_shader);
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 1024;
        glGetShaderiv(vertex_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        glGetShaderInfoLog(vertex_shader, max_length, &max_length, &error_log[0]);
        sysLogPrintf(LOG_ERROR, "Failed to compile this vertex shader (ID %llx, %x):\n%s", shader_id0, shader_id1, vs_buf);
        sysFatalError("Vertex shader compilation failed:\n%s", error_log);
    }

    GLuint fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &sources[1], &lengths[1]);
    glCompileShader(fragment_shader);
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint max_length = 1024;
        glGetShaderiv(fragment_shader, GL_INFO_LOG_LENGTH, &max_length);
        char error_log[1024];
        glGetShaderInfoLog(fragment_shader, max_length, &max_length, &error_log[0]);
        sysLogPrintf(LOG_ERROR, "Failed to compile this fragment shader (ID %llx, %x):\n%s", shader_id0, shader_id1, fs_buf);
        sysFatalError("Fragment shader compilation failed:\n%s", error_log);
    }

    GLuint shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);
    glLinkProgram(shader_program);

    glDetachShader(shader_program, vertex_shader);
    glDetachShader(shader_program, fragment_shader);
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    size_t cnt = 0;

    struct ShaderProgram* prg = &shader_program_pool[make_pair(shader_id0, shader_id1)];
    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aVtxPos");
    prg->attrib_sizes[cnt] = 4;
    ++cnt;

    for (int i = 0; i < 2; i++) {
        if (cc_features.used_textures[i]) {
            char name[32];
            sprintf(name, "aTexCoord%d", i);
            prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
            prg->attrib_sizes[cnt] = 2;
            ++cnt;

            for (int j = 0; j < 2; j++) {
                if (cc_features.clamp[i][j]) {
                    sprintf(name, "aTexClamp%s%d", j == 0 ? "S" : "T", i);
                    prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
                    prg->attrib_sizes[cnt] = 1;
                    ++cnt;
                }
            }
        }
    }

    if (cc_features.opt_fog) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aFog");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    if (cc_features.opt_grayscale) {
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, "aGrayscaleColor");
        prg->attrib_sizes[cnt] = 4;
        ++cnt;
    }

    for (int i = 0; i < cc_features.num_inputs; i++) {
        char name[16];
        sprintf(name, "aInput%d", i + 1);
        prg->attrib_locations[cnt] = glGetAttribLocation(shader_program, name);
        prg->attrib_sizes[cnt] = cc_features.opt_alpha ? 4 : 3;
        ++cnt;
    }

    prg->opengl_program_id = shader_program;
    prg->num_inputs = cc_features.num_inputs;
    prg->used_textures[0] = cc_features.used_textures[0];
    prg->used_textures[1] = cc_features.used_textures[1];
    prg->num_floats = num_floats;
    prg->num_attribs = cnt;

    glUseProgram(shader_program);

    if (cc_features.used_textures[0]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex0");
        glUniform1i(sampler_location, 0);
    }
    if (cc_features.used_textures[1]) {
        GLint sampler_location = glGetUniformLocation(shader_program, "uTex1");
        glUniform1i(sampler_location, 1);
    }

    prg->frame_count_location = glGetUniformLocation(shader_program, "frame_count");
    prg->noise_scale_location = glGetUniformLocation(shader_program, "noise_scale");
    prg->three_point_filter_locations[0] = glGetUniformLocation(shader_program, "three_point_filter0");
    prg->three_point_filter_locations[1] = glGetUniformLocation(shader_program, "three_point_filter1");
    prg->tex_size_locations[0] = glGetUniformLocation(shader_program, "uTexSize0");
    prg->tex_size_locations[1] = glGetUniformLocation(shader_program, "uTexSize1");
    prg->tex_size_set[0][0] = prg->tex_size_set[0][1] = -1.f;
    prg->tex_size_set[1][0] = prg->tex_size_set[1][1] = -1.f;

    {
        GLint linked = 0;
        glGetProgramiv(shader_program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[1024] = { 0 };
            GLsizei len = 0;
            glGetProgramInfoLog(shader_program, sizeof(log) - 1, &len, log);
            sysLogPrintf(LOG_ERROR, "GLES2: link failed (ID %llx, %x):\n%s\n%s", shader_id0, shader_id1, vs_buf, fs_buf);
            sysFatalError("Shader program link failed:\n%s", log);
        }
    }

    gfx_opengl_load_shader(prg);

    return prg;
}

static struct ShaderProgram* gfx_opengl_lookup_shader(uint64_t shader_id0, uint32_t shader_id1) {
    auto it = shader_program_pool.find(make_pair(shader_id0, shader_id1));
    return it == shader_program_pool.end() ? nullptr : &it->second;
}

static void gfx_opengl_shader_get_info(struct ShaderProgram* prg, uint8_t* num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static void gfx_opengl_clear_shaders(void) {
    glUseProgram(0);
    current_prg = nullptr;
    for (auto& pair : shader_program_pool) {
        glDeleteProgram(pair.second.opengl_program_id);
    }
    shader_program_pool.clear();
}


// ---------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------

static bool is_pot(uint32_t v) {
    return v && !(v & (v - 1));
}

static GLuint gfx_opengl_new_texture(void) {
    GLuint ret;
    glGenTextures(1, &ret);
    return ret;
}

static void gfx_opengl_delete_texture(uint32_t texID) {
    tex_sizes.erase(texID);
    glDeleteTextures(1, &texID);
}

static void gles2_set_active_tile(int tile) {
    if (active_tile != tile) {
        glActiveTexture(GL_TEXTURE0 + tile);
        active_tile = tile;
    }
}

static void gfx_opengl_select_texture(int tile, GLuint texture_id, bool linear_filter) {
    gles2_set_active_tile(tile);
    glBindTexture(GL_TEXTURE_2D, texture_id);
    bound_tex[tile] = texture_id;
    current_textures_linear_filter[tile] = linear_filter;
}

static void gfx_opengl_upload_texture(const uint8_t* rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps) {
    // GLES2: unsized internal format, must match the external format
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba32_buf);
    tex_sizes[bound_tex[active_tile]] = { width, height };
    const bool can_mip = gl_npot_full || (is_pot(width) && is_pot(height));
    if (can_mip && (gen_mipmaps || current_filter_mode == FILTER_THREE_POINT)) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
}

static uint32_t gfx_cm_to_opengl(uint32_t val) {
    switch (val) {
        case G_TX_NOMIRROR | G_TX_CLAMP:
            return GL_CLAMP_TO_EDGE;
        case G_TX_MIRROR | G_TX_WRAP:
            return GL_MIRRORED_REPEAT;
        case G_TX_MIRROR | G_TX_CLAMP:
            return gl_mirror_clamp;
        case G_TX_NOMIRROR | G_TX_WRAP:
            return GL_REPEAT;
    }
    return GL_REPEAT;
}

static void gfx_opengl_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps) {
    const GLint min_filters[3][3] = {
        // MIPMAP_DISABLED   MIPMAP_NEAREST               MIPMAP_LINEAR
        {  GL_NEAREST,       GL_NEAREST_MIPMAP_NEAREST,   GL_NEAREST_MIPMAP_LINEAR  }, // FILTER_NONE
        {  GL_LINEAR,        GL_LINEAR_MIPMAP_NEAREST,    GL_LINEAR_MIPMAP_LINEAR   }, // FILTER_BILINEAR
        {  GL_NEAREST,       GL_NEAREST,                  GL_NEAREST                }, // FILTER_THREE_POINT
    };

    // Without GL_OES_texture_npot, a non-power-of-two texture (framebuffer copies) is incomplete
    // with REPEAT wrapping or mipmapped filtering, and samples as black.
    bool npot_limited = false;
    if (!gl_npot_full) {
        auto it = tex_sizes.find(bound_tex[tile]);
        if (it != tex_sizes.end()) {
            npot_limited = !is_pot(it->second.first) || !is_pot(it->second.second);
        }
    }

    mipmaps = mipmaps && !npot_limited && (current_mipmap_filter_mode != MIPMAP_DISABLED);
    const int mip_idx = mipmaps ? current_mipmap_filter_mode : 0;
    const GLint min_filter = linear_filter ? min_filters[current_filter_mode][mip_idx] : GL_NEAREST;
    const GLint max_filter = linear_filter && (current_filter_mode == FILTER_LINEAR) ? GL_LINEAR : GL_NEAREST;

    gles2_set_active_tile(tile);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, max_filter);

    if (mipmaps && gl_has_aniso) {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, (GLfloat)(current_anisotropy_level > 0 ? current_anisotropy_level : 1));
    }

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, npot_limited ? GL_CLAMP_TO_EDGE : gfx_cm_to_opengl(cms));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, npot_limited ? GL_CLAMP_TO_EDGE : gfx_cm_to_opengl(cmt));
}

static void gfx_opengl_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare, bool depth_source_prim, uint16_t zmode) {
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);
        glDepthMask(depth_update ? GL_TRUE : GL_FALSE);
        current_depth_mask = depth_update;

        if (depth_compare) {
            switch (zmode) {
                case ZMODE_INTER:
                    glDepthFunc(GL_LEQUAL);
                    glDisable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(0, 0);
                    break;

                case ZMODE_OPA:
                case ZMODE_XLU:
                    if (depth_source_prim) {
                        glDepthFunc(GL_LEQUAL);
                    } else {
                        glDepthFunc(GL_LESS);
                    }
                    glDisable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(0, 0);
                    break;

                case ZMODE_DEC:
                    glDepthFunc(GL_LEQUAL);
                    glEnable(GL_POLYGON_OFFSET_FILL);
                    glPolygonOffset(-2, -2);
                    break;
            }
        } else {
            glDepthFunc(GL_ALWAYS);
            glDisable(GL_POLYGON_OFFSET_FILL);
            glPolygonOffset(0, 0);
        }
    } else {
        glDisable(GL_DEPTH_TEST);
    }
}


static void gfx_opengl_set_depth_range(float znear, float zfar) {
    glDepthRangef(znear, zfar);
}

static void gfx_opengl_set_viewport(int x, int y, int width, int height) {
    cur_viewport[0] = x; cur_viewport[1] = y; cur_viewport[2] = width; cur_viewport[3] = height;
    glViewport(x, y, width, height);
}

static void gfx_opengl_set_scissor(int x, int y, int width, int height) {
    cur_scissor[0] = x; cur_scissor[1] = y; cur_scissor[2] = width; cur_scissor[3] = height;
    glScissor(x, y, width, height);
}

static void gfx_opengl_set_use_alpha(bool use_alpha, bool modulate) {
    if (use_alpha) {
        glEnable(GL_BLEND);
    } else {
        glDisable(GL_BLEND);
    }
    if (modulate) {
        glBlendFunc(GL_DST_COLOR, GL_ZERO);
    } else {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
}

static void gles2_set_tex_size_uniforms(struct ShaderProgram* prg) {
    for (int i = 0; i < 2; i++) {
        if (prg->tex_size_locations[i] < 0) {
            continue;
        }
        float w = 1.0f, h = 1.0f;
        auto it = tex_sizes.find(bound_tex[i]);
        if (it != tex_sizes.end()) {
            w = (float)it->second.first;
            h = (float)it->second.second;
        }
        if (prg->tex_size_set[i][0] != w || prg->tex_size_set[i][1] != h) {
            glUniform2f(prg->tex_size_locations[i], w, h);
            prg->tex_size_set[i][0] = w;
            prg->tex_size_set[i][1] = h;
        }
    }
}

static void gfx_opengl_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (current_prg) {
        // replaces textureSize(), which GLSL ES 1.00 does not have
        gles2_set_tex_size_uniforms(current_prg);
    }
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * buf_vbo_len, buf_vbo, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 3 * buf_vbo_num_tris);
}

// ---------------------------------------------------------------------------------------------
// Blitting (GLES2 has no glBlitFramebuffer): draw a textured quad with a dedicated program and
// put back every bit of state the fast3d layer caches.
// ---------------------------------------------------------------------------------------------

static GLuint blit_program;
static GLint blit_tex_location;

static GLuint gles2_compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = { 0 };
        GLsizei len = 0;
        glGetShaderInfoLog(sh, sizeof(log) - 1, &len, log);
        sysFatalError("GLES2: blit shader compilation failed:\n%s", log);
    }
    return sh;
}

static void gles2_init_blit(void) {
    static const char* vs =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    vUV = aUV;\n"
        "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "}\n";
    static const char* fs =
        "precision mediump float;\n"
        "uniform sampler2D uTex;\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "    gl_FragColor = texture2D(uTex, vUV);\n"
        "}\n";
    GLuint v = gles2_compile(GL_VERTEX_SHADER, vs);
    GLuint f = gles2_compile(GL_FRAGMENT_SHADER, fs);
    blit_program = glCreateProgram();
    glAttachShader(blit_program, v);
    glAttachShader(blit_program, f);
    glBindAttribLocation(blit_program, 0, "aPos");
    glBindAttribLocation(blit_program, 1, "aUV");
    glLinkProgram(blit_program);
    GLint ok = 0;
    glGetProgramiv(blit_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        sysFatalError("GLES2: blit program link failed");
    }
    glDetachShader(blit_program, v);
    glDetachShader(blit_program, f);
    glDeleteShader(v);
    glDeleteShader(f);
    blit_tex_location = glGetUniformLocation(blit_program, "uTex");
}

// src rect is in texels of src_tex (GL convention, origin bottom left), dst rect in pixels of the
// destination framebuffer. dst_y0 > dst_y1 flips the image vertically.
static void gles2_blit(GLuint src_tex, uint32_t src_w, uint32_t src_h, int sx0, int sy0, int sx1, int sy1,
                       GLuint dst_fbo, uint32_t dst_w, uint32_t dst_h, int dx0, int dy0, int dx1, int dy1,
                       bool linear) {
    const GLboolean blend_on = glIsEnabled(GL_BLEND);
    const GLboolean depth_on = glIsEnabled(GL_DEPTH_TEST);
    const GLboolean scissor_on = glIsEnabled(GL_SCISSOR_TEST);

    if (current_prg) {
        gfx_opengl_unload_shader(current_prg);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
    glViewport(0, 0, dst_w, dst_h);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDepthMask(GL_FALSE);

    const float fw = (float)dst_w, fh = (float)dst_h;
    const float x0 = dx0 / fw * 2.f - 1.f, x1 = dx1 / fw * 2.f - 1.f;
    const float y0 = dy0 / fh * 2.f - 1.f, y1 = dy1 / fh * 2.f - 1.f;
    const float u0 = (float)sx0 / src_w, u1 = (float)sx1 / src_w;
    const float v0 = (float)sy0 / src_h, v1 = (float)sy1 / src_h;
    const float quad[16] = {
        x0, y0, u0, v0,
        x1, y0, u1, v0,
        x0, y1, u0, v1,
        x1, y1, u1, v1,
    };

    glUseProgram(blit_program);
    glUniform1i(blit_tex_location, 0);
    gles2_set_active_tile(0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);

    // restore
    glBindTexture(GL_TEXTURE_2D, bound_tex[0]);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[current_framebuffer].fbo);
    glViewport(cur_viewport[0], cur_viewport[1], cur_viewport[2], cur_viewport[3]);
    glScissor(cur_scissor[0], cur_scissor[1], cur_scissor[2], cur_scissor[3]);
    if (scissor_on) glEnable(GL_SCISSOR_TEST);
    if (depth_on) glEnable(GL_DEPTH_TEST);
    if (blend_on) glEnable(GL_BLEND);
    glDepthMask(current_depth_mask ? GL_TRUE : GL_FALSE);
    if (current_prg) {
        gfx_opengl_load_shader(current_prg);
    } else {
        glUseProgram(0);
    }
}

// ---------------------------------------------------------------------------------------------
// Init / frame
// ---------------------------------------------------------------------------------------------

static bool gles2_has_ext(const char* exts, const char* name) {
    if (!exts) {
        return false;
    }
    const size_t len = strlen(name);
    for (const char* p = strstr(exts, name); p; p = strstr(p + 1, name)) {
        if ((p == exts || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
            return true;
        }
    }
    return false;
}

static void gfx_opengl_init(void) {
    const char* version = (const char*)glGetString(GL_VERSION);
    const char* vendor = (const char*)glGetString(GL_VENDOR);
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* glsl = (const char*)glGetString(GL_SHADING_LANGUAGE_VERSION);
    const char* exts = (const char*)glGetString(GL_EXTENSIONS);
    sysLogPrintf(LOG_NOTE, "GLES2: version: %s", version ? version : "unknown");
    sysLogPrintf(LOG_NOTE, "GLES2: vendor: %s", vendor ? vendor : "unknown");
    sysLogPrintf(LOG_NOTE, "GLES2: renderer: %s", renderer ? renderer : "unknown");
    sysLogPrintf(LOG_NOTE, "GLES2: GLSL version: %s", glsl ? glsl : "unknown");
    sysLogPrintf(LOG_NOTE, "GLES2: extensions: %s", exts ? exts : "none");

    gl_npot_full = gles2_has_ext(exts, "GL_OES_texture_npot") || gles2_has_ext(exts, "GL_ARB_texture_non_power_of_two");
    gl_has_aniso = gles2_has_ext(exts, "GL_EXT_texture_filter_anisotropic");
    gl_has_depth24 = gles2_has_ext(exts, "GL_OES_depth24");
    gl_highp_fs = gles2_has_ext(exts, "GL_OES_fragment_precision_high");
    {
        GLint range[2] = { 0, 0 }, precision = 0;
        glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
        gl_highp_fs = gl_highp_fs || precision > 0;
    }

    GLint max_attribs = 0, max_varyings = 0, max_tex = 0;
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &max_attribs);
    glGetIntegerv(GL_MAX_VARYING_VECTORS, &max_varyings);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
    sysLogPrintf(LOG_NOTE, "GLES2: npot %d, aniso %d, depth24 %d, highp fs %d, attribs %d, varyings %d, max tex %d",
                 gl_npot_full, gl_has_aniso, gl_has_depth24, gl_highp_fs, max_attribs, max_varyings, max_tex);

    // 24-bit depth if advertised; update_framebuffer_parameters() drops to 16 bits if the
    // framebuffer turns out incomplete with it
    gl_depth_format = gl_has_depth24 ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16;

    // GL_MIRROR_CLAMP_TO_EDGE does not exist in GLES2
    gl_mirror_clamp = GL_MIRRORED_REPEAT;

    glGenBuffers(1, &opengl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, opengl_vbo);

    gles2_init_blit();

    glActiveTexture(GL_TEXTURE0);
    active_tile = 0;

    glDepthFunc(GL_LEQUAL);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    framebuffers.resize(1); // the window surface
    framebuffers[0].fbo = 0;
}

static void gfx_opengl_on_resize(void) {
}

static void gfx_opengl_start_frame(void) {
    frame_count++;
}

static void gfx_opengl_end_frame(void) {
    glFlush();
}

static void gfx_opengl_finish_render(void) {
}

// ---------------------------------------------------------------------------------------------
// Framebuffers
// ---------------------------------------------------------------------------------------------

static int gfx_opengl_create_framebuffer() {
    size_t i = framebuffers.size();
    framebuffers.resize(i + 1);

    GLuint clrbuf;
    glGenTextures(1, &clrbuf);
    glBindTexture(GL_TEXTURE_2D, clrbuf);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, bound_tex[active_tile]);
    framebuffers[i].clrbuf = clrbuf;
    tex_sizes[clrbuf] = { 1, 1 };

    if (!gfx_framebuffers_enabled) {
        return i;
    }

    glGenRenderbuffers(1, &framebuffers[i].rbo);
    glGenFramebuffers(1, &framebuffers[i].fbo);

    return i;
}

static void gles2_alloc_depth(GLuint rbo, uint32_t width, uint32_t height) {
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, gl_depth_format, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
}

static void gfx_opengl_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height, uint32_t msaa_level,
                                                     bool opengl_invert_y, bool render_target, bool has_depth_buffer,
                                                     bool can_extract_depth) {
    Framebuffer& fb = framebuffers[fb_id];

    width = max(width, 1U);
    height = max(height, 1U);
    msaa_level = 1; // no multisampled renderbuffers in GLES2

    if (fb_id != 0 && gfx_framebuffers_enabled) {
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);

        const bool resized = fb.width != width || fb.height != height;

        if (resized) {
            glBindTexture(GL_TEXTURE_2D, fb.clrbuf);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
            glBindTexture(GL_TEXTURE_2D, bound_tex[active_tile]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fb.clrbuf, 0);
            tex_sizes[fb.clrbuf] = { width, height };
        }

        if (has_depth_buffer && (resized || !fb.has_depth_buffer)) {
            gles2_alloc_depth(fb.rbo, width, height);
        }

        if (!fb.has_depth_buffer && has_depth_buffer) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fb.rbo);
        } else if (fb.has_depth_buffer && !has_depth_buffer) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
        }

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE && has_depth_buffer && gl_depth_format != GL_DEPTH_COMPONENT16) {
            // a deeper format is advertised but not renderable (seen with depth32 on PS4)
            sysLogPrintf(LOG_WARNING, "GLES2: framebuffer incomplete (0x%04x) with depth format 0x%04x, using 16 bits",
                         status, gl_depth_format);
            gl_depth_format = GL_DEPTH_COMPONENT16;
            gles2_alloc_depth(fb.rbo, width, height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fb.rbo);
            status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        }
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            sysLogPrintf(LOG_ERROR, "GLES2: framebuffer %d (%ux%u) incomplete: 0x%04x", fb_id, width, height, status);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, framebuffers[current_framebuffer].fbo);
    } else if (fb_id != 0) {
        has_depth_buffer = false;
    }

    fb.width = width;
    fb.height = height;
    fb.has_depth_buffer = has_depth_buffer;
    fb.msaa_level = msaa_level;
    fb.invert_y = opengl_invert_y;
}

bool gfx_opengl_start_draw_to_framebuffer(int fb_id, float noise_scale) {
    if (fb_id < (int)framebuffers.size() && (fb_id == 0 || gfx_framebuffers_enabled)) {
        Framebuffer& fb = framebuffers[fb_id];
        if (noise_scale != 0.0f) {
            current_noise_scale = 1.0f / noise_scale;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fb.fbo);
        current_framebuffer = fb_id;
        return true;
    }
    return false;
}

void gfx_opengl_clear_framebuffer(bool clear_color, bool clear_depth) {
    glDisable(GL_SCISSOR_TEST);

    GLbitfield mask = 0;
    if (clear_color) {
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (clear_depth) {
        glDepthMask(GL_TRUE);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    glClear(mask);
    if (clear_depth) {
        glDepthMask(current_depth_mask ? GL_TRUE : GL_FALSE);
    }

    glEnable(GL_SCISSOR_TEST);
}

// With no MSAA this is "present the offscreen game framebuffer": a straight 1:1 copy.
void gfx_opengl_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source) {
    if (!gfx_framebuffers_enabled || fb_id_source == 0) {
        return;
    }
    const Framebuffer& src = framebuffers[fb_id_source];
    const Framebuffer& dst = framebuffers[fb_id_target];
    gles2_blit(src.clrbuf, src.width, src.height, 0, 0, src.width, src.height,
               dst.fbo, dst.width, dst.height, 0, 0, dst.width, dst.height, false);
}

void* gfx_opengl_get_framebuffer_texture_id(int fb_id) {
    return (void*)(uintptr_t)framebuffers[fb_id].clrbuf;
}

void gfx_opengl_select_texture_fb(int fb_id) {
    gles2_set_active_tile(0);
    glBindTexture(GL_TEXTURE_2D, framebuffers[fb_id].clrbuf);
    bound_tex[0] = framebuffers[fb_id].clrbuf;
    current_textures_linear_filter[0] = true;
}

void gfx_opengl_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back) {
    if (!gfx_framebuffers_enabled || fb_dst >= (int)framebuffers.size() || fb_src >= (int)framebuffers.size()) {
        return;
    }

    const Framebuffer& src = framebuffers[fb_src];
    const Framebuffer& dst = framebuffers[fb_dst];

    int srcX0, srcY0, srcX1, srcY1;
    int dstX0 = 0, dstY0 = 0, dstX1 = dst.width, dstY1 = dst.height;

    if (left >= 0 && top >= 0) {
        // unscaled rect copy
        srcX0 = left;
        srcY0 = top;
        srcX1 = left + dst.width;
        srcY1 = top + dst.height;
    } else {
        // scaled full copy
        srcX0 = 0;
        srcY0 = 0;
        srcX1 = src.width;
        srcY1 = src.height;
    }

    if (flip_y) {
        std::swap(dstY0, dstY1);
    }

    if (fb_src == 0) {
        // The window surface cannot be sampled, and glCopyTexSubImage2D from it stalls Piglet for
        // seconds. gfx_pc.cpp redirects these copies to the offscreen game framebuffer on PS4, so
        // this can only happen before the first frame: skip the copy (the target keeps its old
        // contents, at worst a stale blur) rather than freeze.
        static bool warned = false;
        if (!warned) {
            sysLogPrintf(LOG_WARNING, "GLES2: skipped a copy from the window surface (fb %d)", fb_dst);
            warned = true;
        }
        return;
    }

    gles2_blit(src.clrbuf, src.width, src.height, srcX0, srcY0, srcX1, srcY1,
               dst.fbo, dst.width, dst.height, dstX0, dstY0, dstX1, dstY1, false);
}

void gfx_opengl_set_texture_filter(FilteringMode mode) {
    current_filter_mode = mode;
}

FilteringMode gfx_opengl_get_texture_filter(void) {
    return current_filter_mode;
}

void gfx_opengl_set_mipmap_filter(MipmapFilteringMode mode) {
    current_mipmap_filter_mode = mode;
}

static int gfx_opengl_get_max_anisotropy_level() {
    if (!gl_has_aniso) {
        return 1;
    }
    GLfloat max_aniso_level = 1.f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &max_aniso_level);
    return (int)max_aniso_level;
}

static void gfx_opengl_set_anisotropy_level(int level) {
    current_anisotropy_level = level;
}

static const char* gfx_gles2_get_name() {
    return "OpenGL ES 2.0";
}

struct GfxRenderingAPI gfx_gles2_api = {
    gfx_gles2_get_name,
    gfx_opengl_get_max_texture_size,
    gfx_opengl_get_clip_parameters,
    gfx_opengl_unload_shader,
    gfx_opengl_load_shader,
    gfx_opengl_create_and_load_new_shader,
    gfx_opengl_lookup_shader,
    gfx_opengl_shader_get_info,
    gfx_opengl_clear_shaders,
    gfx_opengl_new_texture,
    gfx_opengl_select_texture,
    gfx_opengl_upload_texture,
    gfx_opengl_set_sampler_parameters,
    gfx_opengl_set_depth_mode,
    gfx_opengl_set_depth_range,
    gfx_opengl_set_viewport,
    gfx_opengl_set_scissor,
    gfx_opengl_set_use_alpha,
    gfx_opengl_draw_triangles,
    gfx_opengl_init,
    gfx_opengl_on_resize,
    gfx_opengl_start_frame,
    gfx_opengl_end_frame,
    gfx_opengl_finish_render,
    gfx_opengl_create_framebuffer,
    gfx_opengl_update_framebuffer_parameters,
    gfx_opengl_start_draw_to_framebuffer,
    gfx_opengl_copy_framebuffer,
    gfx_opengl_clear_framebuffer,
    gfx_opengl_resolve_msaa_color_buffer,
    gfx_opengl_get_framebuffer_texture_id,
    gfx_opengl_select_texture_fb,
    gfx_opengl_delete_texture,
    gfx_opengl_set_texture_filter,
    gfx_opengl_get_texture_filter,
    gfx_opengl_set_mipmap_filter,
    gfx_opengl_set_anisotropy_level,
    gfx_opengl_get_max_anisotropy_level
};

#endif // PLATFORM_PS4
