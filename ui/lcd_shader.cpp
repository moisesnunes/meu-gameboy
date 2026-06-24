/*
 * lcd_shader.cpp — Shaders GLSL para efeitos de tela LCD/CRT do Game Boy
 *
 * Três presets:
 *   DMG  — ghosting verde característico do visor de cristal líquido passivo
 *   GBC  — grade de subpixel RGB simulando pixels reais de TFT
 *   CRT  — scanlines com queda de luminância estilo monitor de fósforo
 *
 * Pipeline: src_tex (160×144) → FBO (display_w × display_h) → ImGui::Image
 * O shader recebe tanto o frame atual quanto o anterior para o ghosting.
 */

#include "lcd_shader.h"

/* Inclui apenas o necessário para SDL_GL_GetProcAddress, sem puxar opengl.h */
#include <SDL3/SDL_video.h>
#include <stdio.h>
#include <string.h>

/* ── Mini-loader OpenGL 3.3 core ─────────────────────────────────────────── */
/* Carregamos via SDL_GL_GetProcAddress em vez de depender do loader parcial   */
/* do ImGui, que só expõe as funções que ele mesmo usa.                        */

#define GL_ARRAY_BUFFER           0x8892
#define GL_STATIC_DRAW            0x88B4
#define GL_FRAGMENT_SHADER        0x8B30
#define GL_VERTEX_SHADER          0x8B31
#define GL_COMPILE_STATUS         0x8B81
#define GL_LINK_STATUS            0x8B82
#define GL_FRAMEBUFFER            0x8D40
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_TEXTURE0               0x84C0
#define GL_TEXTURE1               0x84C1
#define GL_TEXTURE_2D             0x0DE1
#define GL_NEAREST                0x2600
#define GL_LINEAR                 0x2601
#define GL_TEXTURE_MIN_FILTER     0x2801
#define GL_TEXTURE_MAG_FILTER     0x2800
#define GL_TEXTURE_BINDING_2D     0x8069
#define GL_ACTIVE_TEXTURE         0x84E0
#define GL_RGBA                   0x1908
#define GL_RGBA8                  0x8058
#define GL_UNSIGNED_BYTE          0x1401
#define GL_FLOAT                  0x1406
#define GL_FALSE                  0
#define GL_TRIANGLES              0x0004
#define GL_FRAMEBUFFER_BINDING    0x8CA6
#define GL_CURRENT_PROGRAM        0x8B8D
#define GL_VERTEX_ARRAY_BINDING   0x85B5
#define GL_VIEWPORT               0x0BA2

typedef unsigned int  GLenum;
typedef unsigned int  GLuint;
typedef int           GLint;
typedef int           GLsizei;
typedef char          GLchar;
typedef float         GLfloat;
typedef signed long long GLsizeiptr;
typedef unsigned char GLubyte;

typedef GLuint (*PFNGLCREATESHADER)    (GLenum);
typedef void   (*PFNGLSHADERSOURCE)    (GLuint, GLsizei, const GLchar* const*, const GLint*);
typedef void   (*PFNGLCOMPILESHADER)   (GLuint);
typedef void   (*PFNGLGETSHADERIV)     (GLuint, GLenum, GLint*);
typedef void   (*PFNGLGETSHADERINFOLOG)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (*PFNGLDELETESHADER)    (GLuint);
typedef GLuint (*PFNGLCREATEPROGRAM)   (void);
typedef void   (*PFNGLATTACHSHADER)    (GLuint, GLuint);
typedef void   (*PFNGLLINKPROGRAM)     (GLuint);
typedef void   (*PFNGLGETPROGRAMIV)    (GLuint, GLenum, GLint*);
typedef void   (*PFNGLGETPROGRAMINFOLOG)(GLuint, GLsizei, GLsizei*, GLchar*);
typedef void   (*PFNGLDELETEPROGRAM)   (GLuint);
typedef void   (*PFNGLUSEPROGRAM)      (GLuint);
typedef GLint  (*PFNGLGETUNIFORMLOCATION)(GLuint, const GLchar*);
typedef void   (*PFNGLUNIFORM1I)       (GLint, GLint);
typedef void   (*PFNGLUNIFORM1F)       (GLint, GLfloat);
typedef void   (*PFNGLUNIFORM2F)       (GLint, GLfloat, GLfloat);
typedef void   (*PFNGLGENVERTEXARRAYS) (GLsizei, GLuint*);
typedef void   (*PFNGLBINDVERTEXARRAY) (GLuint);
typedef void   (*PFNGLDELETEVERTEXARRAYS)(GLsizei, const GLuint*);
typedef void   (*PFNGLGENBUFFERS)      (GLsizei, GLuint*);
typedef void   (*PFNGLBINDBUFFER)      (GLenum, GLuint);
typedef void   (*PFNGLBUFFERDATA)      (GLenum, GLsizeiptr, const void*, GLenum);
typedef void   (*PFNGLDELETEBUFFERS)   (GLsizei, const GLuint*);
typedef void   (*PFNGLENABLEVERTEXATTRIBARRAY)(GLuint);
typedef void   (*PFNGLVERTEXATTRIBPOINTER)(GLuint, GLint, GLenum, GLubyte, GLsizei, const void*);
typedef void   (*PFNGLDRAWARRAYS)      (GLenum, GLint, GLsizei);
typedef void   (*PFNGLGENTEXTURES)     (GLsizei, GLuint*);
typedef void   (*PFNGLBINDTEXTURE)     (GLenum, GLuint);
typedef void   (*PFNGLDELETETEXTURES)  (GLsizei, const GLuint*);
typedef void   (*PFNGLTEXIMAGE2D)      (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
typedef void   (*PFNGLTEXPARAMETERI)   (GLenum, GLenum, GLint);
typedef void   (*PFNGLACTIVETEXTURE)   (GLenum);
typedef void   (*PFNGLGENFRAMEBUFFERS) (GLsizei, GLuint*);
typedef void   (*PFNGLBINDFRAMEBUFFER) (GLenum, GLuint);
typedef void   (*PFNGLFRAMEBUFFERTEXTURE2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (*PFNGLCHECKFRAMEBUFFERSTATUS)(GLenum);
typedef void   (*PFNGLDELETEFRAMEBUFFERS)(GLsizei, const GLuint*);
typedef void   (*PFNGLVIEWPORT)        (GLint, GLint, GLsizei, GLsizei);
typedef void   (*PFNGLGETINTEGERV)     (GLenum, GLint*);

static PFNGLCREATESHADER          _glCreateShader           = nullptr;
static PFNGLSHADERSOURCE          _glShaderSource           = nullptr;
static PFNGLCOMPILESHADER         _glCompileShader          = nullptr;
static PFNGLGETSHADERIV           _glGetShaderiv            = nullptr;
static PFNGLGETSHADERINFOLOG      _glGetShaderInfoLog       = nullptr;
static PFNGLDELETESHADER          _glDeleteShader           = nullptr;
static PFNGLCREATEPROGRAM         _glCreateProgram          = nullptr;
static PFNGLATTACHSHADER          _glAttachShader           = nullptr;
static PFNGLLINKPROGRAM           _glLinkProgram            = nullptr;
static PFNGLGETPROGRAMIV          _glGetProgramiv           = nullptr;
static PFNGLGETPROGRAMINFOLOG     _glGetProgramInfoLog      = nullptr;
static PFNGLDELETEPROGRAM         _glDeleteProgram          = nullptr;
static PFNGLUSEPROGRAM            _glUseProgram             = nullptr;
static PFNGLGETUNIFORMLOCATION    _glGetUniformLocation     = nullptr;
static PFNGLUNIFORM1I             _glUniform1i              = nullptr;
static PFNGLUNIFORM1F             _glUniform1f              = nullptr;
static PFNGLUNIFORM2F             _glUniform2f              = nullptr;
static PFNGLGENVERTEXARRAYS       _glGenVertexArrays        = nullptr;
static PFNGLBINDVERTEXARRAY       _glBindVertexArray        = nullptr;
static PFNGLDELETEVERTEXARRAYS    _glDeleteVertexArrays     = nullptr;
static PFNGLGENBUFFERS            _glGenBuffers             = nullptr;
static PFNGLBINDBUFFER            _glBindBuffer             = nullptr;
static PFNGLBUFFERDATA            _glBufferData             = nullptr;
static PFNGLDELETEBUFFERS         _glDeleteBuffers          = nullptr;
static PFNGLENABLEVERTEXATTRIBARRAY _glEnableVertexAttribArray = nullptr;
static PFNGLVERTEXATTRIBPOINTER   _glVertexAttribPointer    = nullptr;
static PFNGLDRAWARRAYS            _glDrawArrays             = nullptr;
static PFNGLGENTEXTURES           _glGenTextures            = nullptr;
static PFNGLBINDTEXTURE           _glBindTexture            = nullptr;
static PFNGLDELETETEXTURES        _glDeleteTextures         = nullptr;
static PFNGLTEXIMAGE2D            _glTexImage2D             = nullptr;
static PFNGLTEXPARAMETERI         _glTexParameteri          = nullptr;
static PFNGLACTIVETEXTURE         _glActiveTexture          = nullptr;
static PFNGLGENFRAMEBUFFERS       _glGenFramebuffers        = nullptr;
static PFNGLBINDFRAMEBUFFER       _glBindFramebuffer        = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2D  _glFramebufferTexture2D   = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUS _glCheckFramebufferStatus = nullptr;
static PFNGLDELETEFRAMEBUFFERS    _glDeleteFramebuffers     = nullptr;
static PFNGLVIEWPORT              _glViewport               = nullptr;
static PFNGLGETINTEGERV           _glGetIntegerv            = nullptr;

#define LOAD(name) _##name = (PFN##name##_TYPE)SDL_GL_GetProcAddress(#name)

/* Macro auxiliar para carregar cada função */
#define LCD_LOAD(T, name) _##name = (T)SDL_GL_GetProcAddress(#name)

static bool load_gl_procs(void)
{
    LCD_LOAD(PFNGLCREATESHADER,           glCreateShader);
    LCD_LOAD(PFNGLSHADERSOURCE,           glShaderSource);
    LCD_LOAD(PFNGLCOMPILESHADER,          glCompileShader);
    LCD_LOAD(PFNGLGETSHADERIV,            glGetShaderiv);
    LCD_LOAD(PFNGLGETSHADERINFOLOG,       glGetShaderInfoLog);
    LCD_LOAD(PFNGLDELETESHADER,           glDeleteShader);
    LCD_LOAD(PFNGLCREATEPROGRAM,          glCreateProgram);
    LCD_LOAD(PFNGLATTACHSHADER,           glAttachShader);
    LCD_LOAD(PFNGLLINKPROGRAM,            glLinkProgram);
    LCD_LOAD(PFNGLGETPROGRAMIV,           glGetProgramiv);
    LCD_LOAD(PFNGLGETPROGRAMINFOLOG,      glGetProgramInfoLog);
    LCD_LOAD(PFNGLDELETEPROGRAM,          glDeleteProgram);
    LCD_LOAD(PFNGLUSEPROGRAM,             glUseProgram);
    LCD_LOAD(PFNGLGETUNIFORMLOCATION,     glGetUniformLocation);
    LCD_LOAD(PFNGLUNIFORM1I,              glUniform1i);
    LCD_LOAD(PFNGLUNIFORM1F,              glUniform1f);
    LCD_LOAD(PFNGLUNIFORM2F,              glUniform2f);
    LCD_LOAD(PFNGLGENVERTEXARRAYS,        glGenVertexArrays);
    LCD_LOAD(PFNGLBINDVERTEXARRAY,        glBindVertexArray);
    LCD_LOAD(PFNGLDELETEVERTEXARRAYS,     glDeleteVertexArrays);
    LCD_LOAD(PFNGLGENBUFFERS,             glGenBuffers);
    LCD_LOAD(PFNGLBINDBUFFER,             glBindBuffer);
    LCD_LOAD(PFNGLBUFFERDATA,             glBufferData);
    LCD_LOAD(PFNGLDELETEBUFFERS,          glDeleteBuffers);
    LCD_LOAD(PFNGLENABLEVERTEXATTRIBARRAY,glEnableVertexAttribArray);
    LCD_LOAD(PFNGLVERTEXATTRIBPOINTER,    glVertexAttribPointer);
    LCD_LOAD(PFNGLDRAWARRAYS,             glDrawArrays);
    LCD_LOAD(PFNGLGENTEXTURES,            glGenTextures);
    LCD_LOAD(PFNGLBINDTEXTURE,            glBindTexture);
    LCD_LOAD(PFNGLDELETETEXTURES,         glDeleteTextures);
    LCD_LOAD(PFNGLTEXIMAGE2D,             glTexImage2D);
    LCD_LOAD(PFNGLTEXPARAMETERI,          glTexParameteri);
    LCD_LOAD(PFNGLACTIVETEXTURE,          glActiveTexture);
    LCD_LOAD(PFNGLGENFRAMEBUFFERS,        glGenFramebuffers);
    LCD_LOAD(PFNGLBINDFRAMEBUFFER,        glBindFramebuffer);
    LCD_LOAD(PFNGLFRAMEBUFFERTEXTURE2D,   glFramebufferTexture2D);
    LCD_LOAD(PFNGLCHECKFRAMEBUFFERSTATUS, glCheckFramebufferStatus);
    LCD_LOAD(PFNGLDELETEFRAMEBUFFERS,     glDeleteFramebuffers);
    LCD_LOAD(PFNGLVIEWPORT,               glViewport);
    LCD_LOAD(PFNGLGETINTEGERV,            glGetIntegerv);

    if (!_glCreateShader || !_glShaderSource || !_glCompileShader ||
        !_glGetShaderiv || !_glGetShaderInfoLog || !_glDeleteShader ||
        !_glCreateProgram || !_glAttachShader || !_glLinkProgram ||
        !_glGetProgramiv || !_glGetProgramInfoLog || !_glDeleteProgram ||
        !_glUseProgram || !_glGetUniformLocation || !_glUniform1i ||
        !_glUniform1f || !_glUniform2f || !_glGenVertexArrays ||
        !_glBindVertexArray || !_glDeleteVertexArrays || !_glGenBuffers ||
        !_glBindBuffer || !_glBufferData || !_glDeleteBuffers ||
        !_glEnableVertexAttribArray || !_glVertexAttribPointer ||
        !_glDrawArrays || !_glGenTextures || !_glBindTexture ||
        !_glDeleteTextures || !_glTexImage2D || !_glTexParameteri ||
        !_glActiveTexture || !_glGenFramebuffers || !_glBindFramebuffer ||
        !_glFramebufferTexture2D || !_glCheckFramebufferStatus ||
        !_glDeleteFramebuffers || !_glViewport || !_glGetIntegerv)
    {
         fprintf(stderr, "lcd_shader: falha ao carregar funções OpenGL 3.3\n");
         return false;
    }
    return true;
}

/* Aliases para o código usar os nomes normais */
#define glCreateShader            _glCreateShader
#define glShaderSource            _glShaderSource
#define glCompileShader           _glCompileShader
#define glGetShaderiv             _glGetShaderiv
#define glGetShaderInfoLog        _glGetShaderInfoLog
#define glDeleteShader            _glDeleteShader
#define glCreateProgram           _glCreateProgram
#define glAttachShader            _glAttachShader
#define glLinkProgram             _glLinkProgram
#define glGetProgramiv            _glGetProgramiv
#define glGetProgramInfoLog       _glGetProgramInfoLog
#define glDeleteProgram           _glDeleteProgram
#define glUseProgram              _glUseProgram
#define glGetUniformLocation      _glGetUniformLocation
#define glUniform1i               _glUniform1i
#define glUniform1f               _glUniform1f
#define glUniform2f               _glUniform2f
#define glGenVertexArrays         _glGenVertexArrays
#define glBindVertexArray         _glBindVertexArray
#define glDeleteVertexArrays      _glDeleteVertexArrays
#define glGenBuffers              _glGenBuffers
#define glBindBuffer              _glBindBuffer
#define glBufferData              _glBufferData
#define glDeleteBuffers           _glDeleteBuffers
#define glEnableVertexAttribArray _glEnableVertexAttribArray
#define glVertexAttribPointer     _glVertexAttribPointer
#define glDrawArrays              _glDrawArrays
#define glGenTextures             _glGenTextures
#define glBindTexture             _glBindTexture
#define glDeleteTextures          _glDeleteTextures
#define glTexImage2D              _glTexImage2D
#define glTexParameteri           _glTexParameteri
#define glActiveTexture           _glActiveTexture
#define glGenFramebuffers         _glGenFramebuffers
#define glBindFramebuffer         _glBindFramebuffer
#define glFramebufferTexture2D    _glFramebufferTexture2D
#define glCheckFramebufferStatus  _glCheckFramebufferStatus
#define glDeleteFramebuffers      _glDeleteFramebuffers
#define glViewport                _glViewport
#define glGetIntegerv             _glGetIntegerv

/* ── GLSL sources ─────────────────────────────────────────────────────────── */

static const char *k_vert_src = R"GLSL(
#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
out vec2 v_uv;
void main() {
    v_uv = a_uv;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)GLSL";

/*
 * Uniforms:
 *   u_tex       — frame atual (GL_TEXTURE0)
 *   u_prev_tex  — frame anterior (GL_TEXTURE1)
 *   u_prev_valid — 1 se frame anterior está disponível
 *   u_preset    — 0=none, 1=DMG, 2=GBC, 3=CRT
 *   u_ghost     — intensidade do ghosting [0,1]
 *   u_subpixel  — intensidade da grade de subpixel [0,1]
 *   u_scanline  — intensidade das scanlines [0,1]
 *   u_brightness — multiplicador de brilho
 *   u_resolution — tamanho do FBO de saída (pixels)
 *   u_src_res    — tamanho da textura fonte (160×144)
 */
static const char *k_frag_src = R"GLSL(
#version 330 core
in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_tex;
uniform sampler2D u_prev_tex;
uniform int       u_prev_valid;
uniform int       u_preset;
uniform float     u_ghost;
uniform float     u_subpixel;
uniform float     u_scanline;
uniform float     u_brightness;
uniform vec2      u_resolution;
uniform vec2      u_src_res;

/* ── helpers ── */

/* Cor base: sample bilinear no frame atual */
vec3 sample_current(vec2 uv) {
    return texture(u_tex, uv).rgb;
}

/* Ghosting: mistura frame atual com frame anterior */
vec3 apply_ghost(vec3 cur, vec2 uv, float strength) {
    if (u_prev_valid == 0 || strength <= 0.0) return cur;
    vec3 prev = texture(u_prev_tex, uv).rgb;
    return mix(cur, prev, strength * 0.55);
}

/* Grade de subpixel RGB: escurece entre colunas de subpixel.
   Simula a disposição R·G·B de pixels reais de tela LCD. */
vec3 apply_subpixel(vec3 color, vec2 uv, float strength) {
    if (strength <= 0.0) return color;

    /* Coordenada de pixel dentro da textura fonte */
    vec2 src_coord = uv * u_src_res;

    /* Posição fracionária dentro do pixel destino ampliado */
    vec2 frac_pos = fract(src_coord * (u_resolution / u_src_res));

    /* Largura de cada sub-coluna (3 subpixels por pixel) */
    float sub_x = frac_pos.x * 3.0;
    float sub_col = floor(sub_x);  /* 0=R, 1=G, 2=B */

    /* Grade vertical entre pixels */
    float gap_v = smoothstep(0.0, 0.08, frac_pos.y) *
                  smoothstep(1.0, 0.92, frac_pos.y);
    /* Grade horizontal entre subpixels */
    float gap_h = smoothstep(0.0, 0.12, fract(sub_x)) *
                  smoothstep(1.0, 0.88, fract(sub_x));

    /* Máscara de subpixel: cada canal só ilumina na sua coluna */
    vec3 mask;
    mask.r = (sub_col == 0.0) ? 1.0 : 0.15;
    mask.g = (sub_col == 1.0) ? 1.0 : 0.15;
    mask.b = (sub_col == 2.0) ? 1.0 : 0.15;

    float grid = gap_v * gap_h;
    vec3 subpixel_color = color * mask * grid;

    return mix(color, subpixel_color, strength);
}

/* Scanlines: queda de luminância em linhas alternadas */
vec3 apply_scanlines(vec3 color, vec2 uv, float strength) {
    if (strength <= 0.0) return color;

    /* Linha de pixel dentro da textura fonte */
    float src_y = uv.y * u_src_res.y;
    float dst_y = uv.y * u_resolution.y;

    /* Queda senoidal entre as linhas (mais suave que retângulos) */
    float scale_y = u_resolution.y / u_src_res.y;
    float wave = 0.5 + 0.5 * cos(fract(src_y) * 3.14159265);
    /* Quanto maior o scale, mais pronunciado o efeito; normaliza acima de 3x */
    float depth = mix(0.25, 0.70, min(strength, 1.0));
    float scanline = 1.0 - depth * wave * min(scale_y / 3.0, 1.0);

    return color * scanline;
}

/* Ghosting com tonalidade esverdeada do LCD passivo DMG */
vec3 apply_dmg_ghost(vec3 cur, vec2 uv, float strength) {
    if (u_prev_valid == 0 || strength <= 0.0) return cur;
    vec3 prev = texture(u_prev_tex, uv).rgb;
    /* DMG tem persistência maior no verde */
    vec3 ghost_tint = vec3(0.75, 1.0, 0.60);
    vec3 ghost = prev * ghost_tint;
    return mix(cur, ghost, strength * 0.50);
}

/* ── main ── */
void main() {
    vec2 uv = v_uv;
    vec3 color = sample_current(uv);

    if (u_preset == 1) {
        /* DMG: ghosting verde + scanlines leves */
        color = apply_dmg_ghost(color, uv, u_ghost);
        color = apply_scanlines(color, uv, u_scanline * 0.6);

    } else if (u_preset == 2) {
        /* GBC: ghosting neutro + grade de subpixel */
        color = apply_ghost(color, uv, u_ghost);
        color = apply_subpixel(color, uv, u_subpixel);

    } else if (u_preset == 3) {
        /* CRT: ghosting neutro + scanlines pronunciadas */
        color = apply_ghost(color, uv, u_ghost * 0.4);
        color = apply_scanlines(color, uv, u_scanline);
    }

    color *= u_brightness;
    frag_color = vec4(clamp(color, 0.0, 1.0), 1.0);
}
)GLSL";

/* ── Estado interno ────────────────────────────────────────────────────────── */

static GLuint s_prog     = 0;
static GLuint s_vao      = 0;
static GLuint s_vbo      = 0;
static GLuint s_fbo      = 0;
static GLuint s_fbo_tex  = 0;
static int    s_fbo_w    = 0;
static int    s_fbo_h    = 0;
static int    s_src_w    = 0;
static int    s_src_h    = 0;

/* Locations dos uniforms */
static GLint s_u_tex        = -1;
static GLint s_u_prev_tex   = -1;
static GLint s_u_prev_valid = -1;
static GLint s_u_preset     = -1;
static GLint s_u_ghost      = -1;
static GLint s_u_subpixel   = -1;
static GLint s_u_scanline   = -1;
static GLint s_u_brightness = -1;
static GLint s_u_resolution = -1;
static GLint s_u_src_res    = -1;

/* ── Helpers de compilação ─────────────────────────────────────────────────── */

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint id = glCreateShader(type);
    glShaderSource(id, 1, &src, nullptr);
    glCompileShader(id);

    GLint ok = 0;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(id, sizeof(log), nullptr, log);
        fprintf(stderr, "lcd_shader: compile error:\n%s\n", log);
        glDeleteShader(id);
        return 0;
    }
    return id;
}

static GLuint link_program(GLuint vert, GLuint frag)
{
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "lcd_shader: link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

static bool ensure_fbo(int w, int h)
{
    if (s_fbo && s_fbo_w == w && s_fbo_h == h)
        return true;

    if (s_fbo_tex) glDeleteTextures(1, &s_fbo_tex);
    if (s_fbo)     glDeleteFramebuffers(1, &s_fbo);

    glGenTextures(1, &s_fbo_tex);
    glBindTexture(GL_TEXTURE_2D, s_fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glGenFramebuffers(1, &s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fbo_tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE)
    {
        fprintf(stderr, "lcd_shader: FBO incompleto (status=0x%x)\n", status);
        glDeleteTextures(1, &s_fbo_tex);
        glDeleteFramebuffers(1, &s_fbo);
        s_fbo_tex = 0;
        s_fbo = 0;
        s_fbo_w = 0;
        s_fbo_h = 0;
        return false;
    }

    s_fbo_w = w;
    s_fbo_h = h;
    return true;
}

/* ── API pública ───────────────────────────────────────────────────────────── */

bool lcd_shader_init(int src_w, int src_h)
{
    s_src_w = src_w;
    s_src_h = src_h;

    if (!load_gl_procs())
        return false;

    GLuint vert = compile_shader(GL_VERTEX_SHADER,   k_vert_src);
    GLuint frag = compile_shader(GL_FRAGMENT_SHADER, k_frag_src);
    if (!vert || !frag)
    {
        if (vert) glDeleteShader(vert);
        if (frag) glDeleteShader(frag);
        return false;
    }

    s_prog = link_program(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    if (!s_prog)
        return false;

    /* Cache uniform locations */
    s_u_tex        = glGetUniformLocation(s_prog, "u_tex");
    s_u_prev_tex   = glGetUniformLocation(s_prog, "u_prev_tex");
    s_u_prev_valid = glGetUniformLocation(s_prog, "u_prev_valid");
    s_u_preset     = glGetUniformLocation(s_prog, "u_preset");
    s_u_ghost      = glGetUniformLocation(s_prog, "u_ghost");
    s_u_subpixel   = glGetUniformLocation(s_prog, "u_subpixel");
    s_u_scanline   = glGetUniformLocation(s_prog, "u_scanline");
    s_u_brightness = glGetUniformLocation(s_prog, "u_brightness");
    s_u_resolution = glGetUniformLocation(s_prog, "u_resolution");
    s_u_src_res    = glGetUniformLocation(s_prog, "u_src_res");

    /* Quad full-screen: 2 triângulos cobrindo [-1,1]×[-1,1] */
    static const float k_quad[] = {
        /* pos xy        uv */
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f, -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f, -1.0f,  0.0f, 0.0f,
         1.0f,  1.0f,  1.0f, 1.0f,
        -1.0f,  1.0f,  0.0f, 1.0f,
    };

    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);

    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_quad), k_quad, GL_STATIC_DRAW);

    /* layout(location=0) = pos xy */
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    /* layout(location=1) = uv */
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    return true;
}

void lcd_shader_destroy(void)
{
    if (s_fbo_tex) { glDeleteTextures(1, &s_fbo_tex);     s_fbo_tex = 0; }
    if (s_fbo)     { glDeleteFramebuffers(1, &s_fbo);     s_fbo = 0; }
    if (s_vbo)     { glDeleteBuffers(1, &s_vbo);           s_vbo = 0; }
    if (s_vao)     { glDeleteVertexArrays(1, &s_vao);     s_vao = 0; }
    if (s_prog)    { glDeleteProgram(s_prog);              s_prog = 0; }
    s_fbo_w = s_fbo_h = 0;
}

unsigned int lcd_shader_process(unsigned int src_tex, unsigned int prev_tex, bool prev_valid,
                                const lcd_shader_params *params,
                                int display_w, int display_h)
{
    if (!s_prog || !src_tex || !params || display_w <= 0 || display_h <= 0)
        return src_tex;

    /* Pass-through: sem overhead de FBO quando não há efeito */
    if (params->preset == LCD_SHADER_NONE)
        return src_tex;

    /* Salva estado OpenGL relevante antes de qualquer resize/recriação do FBO. */
    GLint prev_fbo = 0, prev_prog = 0, prev_vao = 0, prev_active_tex = 0;
    GLint prev_tex0 = 0, prev_tex1 = 0;
    GLint prev_vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,    &prev_fbo);
    glGetIntegerv(GL_CURRENT_PROGRAM,        &prev_prog);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,   &prev_vao);
    glGetIntegerv(GL_VIEWPORT,               prev_vp);
    glGetIntegerv(GL_ACTIVE_TEXTURE,          &prev_active_tex);

    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex1);
    glActiveTexture((GLenum)prev_active_tex);

    if (!ensure_fbo(display_w, display_h))
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex1);
        glActiveTexture((GLenum)prev_active_tex);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        return src_tex;
    }

    /* Renderiza para o FBO */
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, display_w, display_h);

    glUseProgram(s_prog);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, src_tex);
    glUniform1i(s_u_tex, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, prev_valid ? prev_tex : src_tex);
    glUniform1i(s_u_prev_tex, 1);

    glUniform1i(s_u_prev_valid, prev_valid ? 1 : 0);
    glUniform1i(s_u_preset,     (int)params->preset);
    glUniform1f(s_u_ghost,      params->ghost_strength);
    glUniform1f(s_u_subpixel,   params->subpixel_str);
    glUniform1f(s_u_scanline,   params->scanline_str);
    glUniform1f(s_u_brightness, params->brightness);
    glUniform2f(s_u_resolution, (float)display_w, (float)display_h);
    glUniform2f(s_u_src_res,    (float)s_src_w,   (float)s_src_h);

    glBindVertexArray(s_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    /* Restaura estado */
    glBindVertexArray((GLuint)prev_vao);
    glUseProgram((GLuint)prev_prog);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex1);
    glActiveTexture((GLenum)prev_active_tex);

    return s_fbo_tex;
}

lcd_shader_params lcd_shader_defaults(lcd_shader_preset preset)
{
    lcd_shader_params p = {};
    p.preset     = preset;
    p.brightness = 1.0f;

    switch (preset)
    {
    case LCD_SHADER_DMG:
        p.ghost_strength = 0.55f;
        p.subpixel_str   = 0.0f;
        p.scanline_str   = 0.40f;
        p.brightness     = 0.95f;
        break;
    case LCD_SHADER_GBC:
        p.ghost_strength = 0.20f;
        p.subpixel_str   = 0.70f;
        p.scanline_str   = 0.0f;
        p.brightness     = 1.10f;
        break;
    case LCD_SHADER_CRT:
        p.ghost_strength = 0.25f;
        p.subpixel_str   = 0.0f;
        p.scanline_str   = 0.65f;
        p.brightness     = 1.05f;
        break;
    default:
        break;
    }
    return p;
}

const char *lcd_shader_preset_name(lcd_shader_preset preset)
{
    switch (preset)
    {
    case LCD_SHADER_DMG: return "DMG (ghosting verde)";
    case LCD_SHADER_GBC: return "GBC (subpixel RGB)";
    case LCD_SHADER_CRT: return "CRT (scanlines)";
    default:             return "Desligado";
    }
}
