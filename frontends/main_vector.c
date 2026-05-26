/*
 * main_vector.c — Frontend "Game Boy vetorial": renderiza o hardware DMG ou GBC
 * usando primitivas SDL3 (retângulos, arredondamentos, círculos via triangles).
 * Detecta automaticamente se a ROM é GBC e desenha o hardware correspondente.
 * Botões acendem quando pressionados. Sem foto — 100% desenhado em código.
 *
 * Uso: gameboy-vector <rom.gb>
 *
 * Controles:
 *   Setas         D-Pad
 *   LCtrl         A
 *   LShift        B
 *   Enter         Start
 *   RShift        Select
 *   Tab (segurar) Fast-forward 2x
 *   F11           Fullscreen
 *   Q / Escape    Sair
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <semaphore.h>
#include <SDL3/SDL.h>

#include "gb.h"

/* ─── Paleta DMG ─── */
static const uint32_t DMG_PALETTE[4] = {
    0xFF9BBC0F, 0xFF8BAC0F, 0xFF306230, 0xFF0F380F,
};

/* ══════════════════════════════════════════════════════════════════════
 *  CORES DMG (Game Boy original — cinza)
 * ══════════════════════════════════════════════════════════════════════ */
#define DMG_BODY_LIGHT   0xFFCDC7BB   /* bege acinzentado — face frontal     */
#define DMG_BODY_DARK    0xFFB0AA9E   /* bege médio — borda/lateral          */
#define DMG_BODY_EDGE    0xFF9A9488   /* bege escuro — sombra                */
#define DMG_BEZEL        0xFF4A4A54   /* cinza-chumbo azulado — moldura LCD  */
#define DMG_SCREEN_BG    0xFF8B956D   /* oliva/musgo — LCD apagado           */
#define DMG_BTN_AB       0xFFA02858   /* vinho-magenta — A e B               */
#define DMG_BTN_AB_PRESS 0xFFD060A0   /* magenta claro — pressionado         */
#define DMG_BTN_AB_RING  0xFF601040   /* anel ao redor do botão              */
#define DMG_DPAD         0xFF1A1A1A   /* cruzeta quase-preta                 */
#define DMG_DPAD_PRESS   0xFF555555   /* cruzeta pressionada                 */
#define DMG_BTN_SMALL    0xFF7878A0   /* Select/Start — azul acinzentado     */
#define DMG_BTN_SMALL_P  0xFFAAAAFF   /* Select/Start pressionado            */
#define DMG_LABEL        0xFF3A3A5C   /* labels: azul escuro                 */
#define DMG_BRAND        0xFF3A3A6A   /* "Nintendo GAME BOY"                 */
#define DMG_STRIPE_PURP  0xFF663399   /* faixa roxa decorativa               */
#define DMG_STRIPE_BLU   0xFF3355AA   /* faixa azul decorativa               */
#define DMG_SPEAKER      0xFF9A9990   /* grelha alto-falante                 */
#define DMG_LED_RED      0xFFDD2222   /* LED de bateria (vermelho)           */
#define DMG_DOT_MATRIX   0xFF4A4A6A   /* texto "DOT MATRIX..."               */

/* ══════════════════════════════════════════════════════════════════════
 *  CORES GBC (Game Boy Color — verde kiwi)
 * ══════════════════════════════════════════════════════════════════════ */
#define GBC_BODY         0xFF82C341   /* verde kiwi pastel — corpo           */
#define GBC_BODY_DARK    0xFF5E9430   /* verde escuro — borda/sombra         */
#define GBC_BODY_EDGE    0xFF447022   /* verde mais escuro — borda externa   */
#define GBC_BEZEL        0xFF111111   /* bezel preto                         */
#define GBC_SCREEN_BG    0xFF2A2A2A   /* LCD apagado (cinza escuro)          */
#define GBC_BTN_AB       0xFF1A1A1A   /* botões A/B — quase preto            */
#define GBC_BTN_AB_PRESS 0xFF444444   /* botões A/B pressionados             */
#define GBC_BTN_AB_RING  0xFF333333   /* anel do botão                       */
#define GBC_DPAD         0xFF1A1A1A   /* cruzeta preta                       */
#define GBC_DPAD_PRESS   0xFF444444   /* cruzeta pressionada                 */
#define GBC_BTN_SMALL    0xFF111111   /* Select/Start — preto embutido       */
#define GBC_BTN_SMALL_P  0xFF333333   /* Select/Start pressionado            */
#define GBC_LABEL        0xFF222222   /* labels escuros                      */
#define GBC_SPEAKER      0xFF267700   /* pontos do alto-falante              */
#define GBC_NINTENDO_OV  0xFFDDDDDD   /* oval do logo Nintendo               */
#define GBC_NINTENDO_TXT 0xFF111111   /* texto "Nintendo" dentro do oval     */
/* Cores multicoloridas do logo "GAME BOY COLOR" */
#define GBC_LOGO_G       0xFFEE1111   /* G — vermelho                        */
#define GBC_LOGO_A       0xFFDD1111
#define GBC_LOGO_M       0xFFDD8800   /* M — laranja                         */
#define GBC_LOGO_E       0xFFDDDD00   /* E — amarelo                         */
#define GBC_LOGO_B       0xFF11AA11   /* B — verde                           */
#define GBC_LOGO_O       0xFF1111EE   /* O — azul                            */
#define GBC_LOGO_Y       0xFF8811EE   /* Y — roxo                            */
#define GBC_LOGO_C       0xFFEE1111   /* C — vermelho                        */
#define GBC_LOGO_OL      0xFFDD8800   /* o — laranja                         */
#define GBC_LOGO_LO      0xFF11AA11   /* l — verde                           */
#define GBC_LOGO_OR      0xFF1111EE   /* o2 — azul                           */
#define GBC_LOGO_R       0xFF8811EE   /* R — roxo                            */

/* ─── Proporções ─── */
/* DMG: ~90mm × 148mm → 1 : 1.644 */
#define DMG_ASPECT_W 1.0f
#define DMG_ASPECT_H 1.644f
/* GBC: ~69mm × 133mm → 1 : 1.928 */
#define GBC_ASPECT_W 1.0f
#define GBC_ASPECT_H 1.928f

/* ─── Enum botões ─── */
enum {
    VEC_BTN_A = 0,
    VEC_BTN_B,
    VEC_BTN_START,
    VEC_BTN_SELECT,
    VEC_BTN_UP,
    VEC_BTN_DOWN,
    VEC_BTN_LEFT,
    VEC_BTN_RIGHT,
    VEC_BTN_COUNT
};

struct vec_ctx {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *game_tex;

    uint32_t pixels[GB_LCD_WIDTH * GB_LCD_HEIGHT];

    SDL_AudioStream *audio_stream;
    unsigned  audio_buf_index;
    size_t    audio_buf_offset;

    bool buttons[VEC_BTN_COUNT];
    bool fullscreen;
    bool fast_forward;
    bool is_gbc;        /* true quando a ROM carregada é GBC */
};

/* ─── Geometria DMG ─── */
struct dmg_layout {
    float W, H;
    float ox, oy;

    SDL_FRect body;
    float corner_r;

    SDL_FRect bezel;
    SDL_FRect lcd;

    /* LED de bateria */
    float led_cx, led_cy, led_r;

    float dpad_cx, dpad_cy;
    float dpad_arm_w, dpad_arm_h;

    float btn_a_cx, btn_a_cy, btn_r;
    float btn_b_cx, btn_b_cy;

    SDL_FRect sel_rect, sta_rect;

    /* faixas acima do LCD */
    SDL_FRect stripe_purp, stripe_blue;

    /* alto-falante (grades diagonais) */
    float spk_x, spk_y;
    float spk_slot_w, spk_slot_h, spk_gap;
    int   spk_slots;

    /* interruptor ON/OFF */
    SDL_FRect sw_track, sw_knob;
};

/* ─── Geometria GBC ─── */
struct gbc_layout {
    float W, H;
    float ox, oy;

    SDL_FRect body;
    float corner_r;

    SDL_FRect bezel;
    SDL_FRect lcd;

    float dpad_cx, dpad_cy;
    float dpad_arm_w, dpad_arm_h;

    float btn_a_cx, btn_a_cy, btn_r;
    float btn_b_cx, btn_b_cy;

    /* Select/Start — retângulos embutidos */
    SDL_FRect sel_rect, sta_rect;

    /* oval Nintendo */
    float oval_cx, oval_cy, oval_rx, oval_ry;

    /* alto-falante (grade de pontos) */
    float spk_x, spk_y, spk_dot, spk_gap;
    int   spk_cols, spk_rows;
};

/* ─── Helpers de desenho ─── */

static void set_color(SDL_Renderer *r, uint32_t argb)
{
    SDL_SetRenderDrawColor(r,
        (argb >> 16) & 0xFF,
        (argb >>  8) & 0xFF,
        (argb >>  0) & 0xFF,
        (argb >> 24) & 0xFF);
}

static void fill_circle(SDL_Renderer *r, float cx, float cy, float radius, int segs)
{
    if (segs < 8) segs = 8;
    SDL_Vertex verts[3];
    verts[0].position.x = cx;
    verts[0].position.y = cy;
    SDL_GetRenderDrawColor(r,
        (Uint8 *)&verts[0].color.r,
        (Uint8 *)&verts[0].color.g,
        (Uint8 *)&verts[0].color.b,
        (Uint8 *)&verts[0].color.a);
    verts[1].color = verts[0].color;
    verts[2].color = verts[0].color;
    verts[0].tex_coord.x = verts[0].tex_coord.y = 0;
    verts[1].tex_coord.x = verts[1].tex_coord.y = 0;
    verts[2].tex_coord.x = verts[2].tex_coord.y = 0;

    for (int i = 0; i < segs; i++) {
        float a0 = (float)i       / (float)segs * 2.0f * (float)M_PI;
        float a1 = (float)(i + 1) / (float)segs * 2.0f * (float)M_PI;
        verts[1].position.x = cx + cosf(a0) * radius;
        verts[1].position.y = cy + sinf(a0) * radius;
        verts[2].position.x = cx + cosf(a1) * radius;
        verts[2].position.y = cy + sinf(a1) * radius;
        SDL_RenderGeometry(r, NULL, verts, 3, NULL, 0);
    }
}

static void fill_rounded_rect(SDL_Renderer *r, SDL_FRect rect, float rad)
{
    if (rad <= 0) { SDL_RenderFillRect(r, &rect); return; }
    if (rad > rect.w / 2) rad = rect.w / 2;
    if (rad > rect.h / 2) rad = rect.h / 2;

    SDL_FRect hr = { rect.x, rect.y + rad, rect.w, rect.h - 2 * rad };
    SDL_RenderFillRect(r, &hr);

    SDL_FRect vr = { rect.x + rad, rect.y, rect.w - 2 * rad, rad };
    SDL_RenderFillRect(r, &vr);
    vr.y = rect.y + rect.h - rad;
    SDL_RenderFillRect(r, &vr);

    fill_circle(r, rect.x + rad,          rect.y + rad,          rad, 24);
    fill_circle(r, rect.x + rect.w - rad, rect.y + rad,          rad, 24);
    fill_circle(r, rect.x + rad,          rect.y + rect.h - rad, rad, 24);
    fill_circle(r, rect.x + rect.w - rad, rect.y + rect.h - rad, rad, 24);
}

/* angle_rad: rotação da elipse em torno do seu centro (0 = sem rotação) */
static void fill_ellipse_rot(SDL_Renderer *r, float cx, float cy,
                             float rx, float ry, float angle_rad, int segs)
{
    if (segs < 12) segs = 12;
    float ca = cosf(angle_rad), sa = sinf(angle_rad);
    SDL_Vertex verts[3];
    SDL_GetRenderDrawColor(r,
        (Uint8 *)&verts[0].color.r,
        (Uint8 *)&verts[0].color.g,
        (Uint8 *)&verts[0].color.b,
        (Uint8 *)&verts[0].color.a);
    verts[1].color = verts[0].color;
    verts[2].color = verts[0].color;
    verts[0].tex_coord.x = verts[0].tex_coord.y = 0;
    verts[1].tex_coord.x = verts[1].tex_coord.y = 0;
    verts[2].tex_coord.x = verts[2].tex_coord.y = 0;
    verts[0].position.x = cx;
    verts[0].position.y = cy;
    for (int i = 0; i < segs; i++) {
        float a0 = (float)i       / (float)segs * 2.0f * (float)M_PI;
        float a1 = (float)(i + 1) / (float)segs * 2.0f * (float)M_PI;
        float lx0 = cosf(a0) * rx, ly0 = sinf(a0) * ry;
        float lx1 = cosf(a1) * rx, ly1 = sinf(a1) * ry;
        verts[1].position.x = cx + lx0 * ca - ly0 * sa;
        verts[1].position.y = cy + lx0 * sa + ly0 * ca;
        verts[2].position.x = cx + lx1 * ca - ly1 * sa;
        verts[2].position.y = cy + lx1 * sa + ly1 * ca;
        SDL_RenderGeometry(r, NULL, verts, 3, NULL, 0);
    }
}

static void fill_ellipse(SDL_Renderer *r, float cx, float cy, float rx, float ry, int segs)
{
    fill_ellipse_rot(r, cx, cy, rx, ry, 0.0f, segs);
}

/* retângulo sólido rotacionado em torno do seu centro */
static void fill_rotated_rect(SDL_Renderer *r,
                              float cx, float cy,
                              float half_w, float half_h,
                              float angle_rad)
{
    float ca = cosf(angle_rad), sa = sinf(angle_rad);
    float corners[4][2] = {
        { -half_w, -half_h },
        {  half_w, -half_h },
        {  half_w,  half_h },
        { -half_w,  half_h },
    };
    SDL_FColor col;
    SDL_GetRenderDrawColor(r,
        (Uint8 *)&col.r, (Uint8 *)&col.g,
        (Uint8 *)&col.b, (Uint8 *)&col.a);
    SDL_Vertex v[4];
    for (int i = 0; i < 4; i++) {
        v[i].position.x = cx + corners[i][0] * ca - corners[i][1] * sa;
        v[i].position.y = cy + corners[i][0] * sa + corners[i][1] * ca;
        v[i].color = col;
        v[i].tex_coord.x = v[i].tex_coord.y = 0;
    }
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
}

/* triângulo sólido — usado nas setas do D-Pad */
static void fill_triangle(SDL_Renderer *r,
                          float x0, float y0,
                          float x1, float y1,
                          float x2, float y2)
{
    SDL_Vertex v[3];
    SDL_GetRenderDrawColor(r,
        (Uint8 *)&v[0].color.r, (Uint8 *)&v[0].color.g,
        (Uint8 *)&v[0].color.b, (Uint8 *)&v[0].color.a);
    v[1].color = v[2].color = v[0].color;
    v[0].tex_coord.x = v[0].tex_coord.y = 0;
    v[1].tex_coord.x = v[1].tex_coord.y = 0;
    v[2].tex_coord.x = v[2].tex_coord.y = 0;
    v[0].position.x = x0; v[0].position.y = y0;
    v[1].position.x = x1; v[1].position.y = y1;
    v[2].position.x = x2; v[2].position.y = y2;
    SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

/* grid de pixels e reflexo no LCD */
static void draw_lcd_overlay(SDL_Renderer *r, SDL_FRect lcd, int px_w, int px_h)
{
    /* grid sutil — linhas finas a cada pixel lógico; skip quando célula < 3px */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 40);
    float cell_w = lcd.w / (float)px_w;
    float cell_h = lcd.h / (float)px_h;
    int step_x = (cell_w < 3.0f) ? (int)(3.0f / cell_w) + 1 : 1;
    int step_y = (cell_h < 3.0f) ? (int)(3.0f / cell_h) + 1 : 1;
    for (int x = step_x; x < px_w; x += step_x) {
        float fx = lcd.x + (float)x * cell_w;
        SDL_RenderLine(r, fx, lcd.y, fx, lcd.y + lcd.h);
    }
    for (int y = step_y; y < px_h; y += step_y) {
        float fy = lcd.y + (float)y * cell_h;
        SDL_RenderLine(r, lcd.x, fy, lcd.x + lcd.w, fy);
    }

    /* reflexo diagonal — gradiente de 3 vértices, canto sup-esq mais brilhante */
    SDL_Vertex rv[3];
    rv[0].position.x = lcd.x;         rv[0].position.y = lcd.y;
    rv[1].position.x = lcd.x+lcd.w;   rv[1].position.y = lcd.y;
    rv[2].position.x = lcd.x;         rv[2].position.y = lcd.y+lcd.h;
    rv[0].color = (SDL_FColor){1,1,1, 0.10f};
    rv[1].color = (SDL_FColor){1,1,1, 0.0f};
    rv[2].color = (SDL_FColor){1,1,1, 0.0f};
    rv[0].tex_coord.x = rv[0].tex_coord.y = 0;
    rv[1].tex_coord.x = rv[1].tex_coord.y = 0;
    rv[2].tex_coord.x = rv[2].tex_coord.y = 0;
    SDL_RenderGeometry(r, NULL, rv, 3, NULL, 0);
}

/* ─── Fonte 5×7 ─── */
static const uint8_t FONT5X7[][5] = {
    {0x7C,0x12,0x11,0x12,0x7C}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x41,0x41}, /* E */
    {0x7F,0x09,0x09,0x01,0x01}, /* F */
    {0x3E,0x41,0x41,0x51,0x72}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x04,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x03,0x04,0x78,0x04,0x03}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
};

static void draw_char(SDL_Renderer *r, char c, float x, float y, float px_size)
{
    if (c < 'A' || c > 'Z') return;
    const uint8_t *col_data = FONT5X7[(int)(c - 'A')];
    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            if (col_data[col] & (1 << row)) {
                SDL_FRect dot = {
                    x + (float)col * px_size,
                    y + (float)row * px_size,
                    px_size, px_size
                };
                SDL_RenderFillRect(r, &dot);
            }
        }
    }
}

static void draw_text(SDL_Renderer *r, const char *text, float x, float y, float px_size)
{
    float cx = x;
    for (; *text; text++) {
        char c = (*text >= 'a' && *text <= 'z') ? (char)(*text - 32) : *text;
        if (c == ' ') { cx += 4 * px_size; continue; }
        draw_char(r, c, cx, y, px_size);
        cx += 6 * px_size;
    }
}

static float text_width(const char *text, float px_size)
{
    float w = 0;
    for (; *text; text++) {
        if (*text == ' ') w += 4 * px_size;
        else              w += 6 * px_size;
    }
    return w;
}


/* ══════════════════════════════════════════════════════════════════════
 *  LAYOUT E RENDER — DMG
 * ══════════════════════════════════════════════════════════════════════ */

static void compute_dmg_layout(struct dmg_layout *L, int win_w, int win_h)
{
    float sx = (float)win_w / DMG_ASPECT_W;
    float sy = (float)win_h / DMG_ASPECT_H;
    float scale = sx < sy ? sx : sy;

    L->W  = DMG_ASPECT_W * scale;
    L->H  = DMG_ASPECT_H * scale;
    L->ox = ((float)win_w - L->W) / 2.0f;
    L->oy = ((float)win_h - L->H) / 2.0f;

#define X(n)  (L->ox + (n) * L->W)
#define Y(n)  (L->oy + (n) * L->H)
#define S(n)  ((n) * L->W)
#define SH(n) ((n) * L->H)

    L->corner_r = S(0.06f);
    L->body = (SDL_FRect){ X(0), Y(0), L->W, L->H };

    /* bezel cinza-azulado, começa a ~7% do topo, margem lateral ~7% */
    float bx = X(0.07f), by = Y(0.07f);
    float bw = S(0.86f), bh = SH(0.355f);
    L->bezel = (SDL_FRect){ bx, by, bw, bh };

    /* faixas decorativas roxo+azul — ACIMA do LCD, dentro do bezel */
    float fy = by + SH(0.018f);
    float fw = bw * 0.82f;
    float fh = SH(0.007f);
    float fx = bx + (bw - fw) / 2.0f;
    L->stripe_purp = (SDL_FRect){ fx,         fy,         fw, fh };
    L->stripe_blue = (SDL_FRect){ fx, fy + fh * 1.6f, fw, fh };

    /* LCD dentro do bezel, com pad maior em cima (para as faixas) */
    float lpad_h = SH(0.045f);
    float lpad_v = S(0.045f);
    L->lcd = (SDL_FRect){
        bx + lpad_v,
        by + lpad_h + fh * 3.5f,
        bw - 2 * lpad_v,
        bh - lpad_h - fh * 3.5f - lpad_v
    };

    /* LED de bateria: dentro do bezel, à esquerda do LCD, ~45% da altura do bezel */
    L->led_cx = bx + S(0.055f);
    L->led_cy = by + bh * 0.52f;
    L->led_r  = S(0.025f);

    /* D-Pad: ~27% X, ~70% Y */
    L->dpad_cx    = X(0.27f);
    L->dpad_cy    = Y(0.715f);
    L->dpad_arm_w = S(0.095f);
    L->dpad_arm_h = S(0.095f);

    /* Botões A/B */
    L->btn_r    = S(0.082f);
    L->btn_a_cx = X(0.745f);
    L->btn_a_cy = Y(0.675f);
    L->btn_b_cx = X(0.595f);
    L->btn_b_cy = Y(0.725f);

    /* Select / Start — elipses pequenas, inclinadas, ~84% Y */
    float sw = S(0.13f), sh = SH(0.028f);
    L->sel_rect = (SDL_FRect){ X(0.27f) - sw/2, Y(0.845f) - sh/2, sw, sh };
    L->sta_rect = (SDL_FRect){ X(0.48f) - sw/2, Y(0.845f) - sh/2, sw, sh };

    /* Alto-falante: grades diagonais, canto inf. direito */
    L->spk_x      = X(0.58f);
    L->spk_y      = Y(0.775f);
    L->spk_slot_w = S(0.028f);
    L->spk_slot_h = SH(0.022f);
    L->spk_gap    = S(0.042f);
    L->spk_slots  = 6;

    /* interruptor ON/OFF — canto superior direito, acima do bezel (como no DMG real) */
    float track_w = S(0.14f), track_h = SH(0.022f);
    float track_x = X(0.78f), track_y = Y(0.022f);
    L->sw_track = (SDL_FRect){ track_x, track_y, track_w, track_h };
    /* knob na posição ON (lado direito do track) */
    float knob_w = track_w * 0.45f;
    L->sw_knob = (SDL_FRect){ track_x + track_w - knob_w, track_y, knob_w, track_h };

#undef X
#undef Y
#undef S
#undef SH
}

static void draw_dmg_dpad(SDL_Renderer *r, struct dmg_layout *L, bool *btns)
{
    float cx = L->dpad_cx, cy = L->dpad_cy;
    float aw = L->dpad_arm_w, ah = L->dpad_arm_h;

    uint32_t col_ud = (btns[VEC_BTN_UP] || btns[VEC_BTN_DOWN]) ? DMG_DPAD_PRESS : DMG_DPAD;
    set_color(r, col_ud);
    SDL_FRect vert = { cx - aw/2, cy - ah*1.5f, aw, ah*3.0f };
    fill_rounded_rect(r, vert, aw * 0.12f);

    uint32_t col_lr = (btns[VEC_BTN_LEFT] || btns[VEC_BTN_RIGHT]) ? DMG_DPAD_PRESS : DMG_DPAD;
    set_color(r, col_lr);
    SDL_FRect horiz = { cx - ah*1.5f, cy - aw/2, ah*3.0f, aw };
    fill_rounded_rect(r, horiz, aw * 0.12f);

    /* sulco X diagonal — dois retângulos rotacionados ±45° */
    set_color(r, 0xFF080808);
    float xs = aw * 0.12f;
    float xlen = ah * 1.55f; /* metade do comprimento de cada braço */
    fill_rotated_rect(r, cx, cy, xlen, xs/2, (float)M_PI * 0.25f);
    fill_rotated_rect(r, cx, cy, xlen, xs/2, (float)M_PI * -0.25f);

    /* setas nas pontas */
    float as = aw * 0.28f;
    set_color(r, 0xFF555555);
    fill_triangle(r, cx, cy-ah*1.5f+as*0.3f, cx-as, cy-ah*1.5f+as*1.1f, cx+as, cy-ah*1.5f+as*1.1f); /* cima */
    fill_triangle(r, cx, cy+ah*1.5f-as*0.3f, cx-as, cy+ah*1.5f-as*1.1f, cx+as, cy+ah*1.5f-as*1.1f); /* baixo */
    fill_triangle(r, cx-ah*1.5f+as*0.3f, cy, cx-ah*1.5f+as*1.1f, cy-as, cx-ah*1.5f+as*1.1f, cy+as); /* esq */
    fill_triangle(r, cx+ah*1.5f-as*0.3f, cy, cx+ah*1.5f-as*1.1f, cy-as, cx+ah*1.5f-as*1.1f, cy+as); /* dir */

    if (btns[VEC_BTN_UP])    { set_color(r, DMG_DPAD_PRESS); SDL_FRect u = { cx-aw/2, cy-ah*1.5f, aw, ah }; SDL_RenderFillRect(r, &u); }
    if (btns[VEC_BTN_DOWN])  { set_color(r, DMG_DPAD_PRESS); SDL_FRect d = { cx-aw/2, cy+ah*0.5f, aw, ah }; SDL_RenderFillRect(r, &d); }
    if (btns[VEC_BTN_LEFT])  { set_color(r, DMG_DPAD_PRESS); SDL_FRect l = { cx-ah*1.5f, cy-aw/2, ah, aw }; SDL_RenderFillRect(r, &l); }
    if (btns[VEC_BTN_RIGHT]) { set_color(r, DMG_DPAD_PRESS); SDL_FRect rt = { cx+ah*0.5f, cy-aw/2, ah, aw }; SDL_RenderFillRect(r, &rt); }
}

/* angle_rad: eixo de compressão da elipse — alinhado com a linha A→B (~25°) */
static void draw_dmg_round_btn(SDL_Renderer *r, float cx, float cy, float radius, bool pressed)
{
    /* leve achatamento na direção perpendicular à linha A→B (~25°) para relevo inclinado */
    float ang = (float)M_PI / 7.2f; /* ~25° */
    float rx = radius, ry = radius * 0.93f;

    set_color(r, DMG_BTN_AB_RING);
    fill_ellipse_rot(r, cx, cy, rx + rx * 0.10f, ry + ry * 0.10f, ang, 40);

    set_color(r, pressed ? DMG_BTN_AB_PRESS : 0xFF500000);
    fill_ellipse_rot(r, cx, cy + ry * 0.06f, rx, ry, ang, 40);

    set_color(r, pressed ? DMG_BTN_AB_PRESS : DMG_BTN_AB);
    fill_ellipse_rot(r, cx, cy - ry * 0.04f, rx * 0.92f, ry * 0.92f, ang, 40);

    if (!pressed) {
        set_color(r, 0xFFBB3333);
        fill_circle(r, cx - radius * 0.18f, cy - radius * 0.22f, radius * 0.32f, 20);
    }
}

/* angle_rad: inclinação do botão (no DMG real ~25° = M_PI/7.2) */
static void draw_dmg_small_btn_rot(SDL_Renderer *r, SDL_FRect rect, bool pressed, float angle_rad)
{
    float cx = rect.x + rect.w / 2;
    float cy = rect.y + rect.h / 2;
    float rx = rect.w / 2;
    float ry = rect.h / 2;

    set_color(r, 0xFF333355);
    fill_ellipse_rot(r, cx, cy + ry * 0.2f, rx, ry, angle_rad, 32);

    set_color(r, pressed ? DMG_BTN_SMALL_P : DMG_BTN_SMALL);
    fill_ellipse_rot(r, cx, cy, rx * 0.95f, ry * 0.90f, angle_rad, 32);
}


/* Alto-falante DMG: grades retangulares inclinadas */
static void draw_dmg_speaker(SDL_Renderer *r, struct dmg_layout *L)
{
    set_color(r, DMG_SPEAKER);
    /* seis slots retangulares com inclinação diagonal */
    for (int i = 0; i < L->spk_slots; i++) {
        float offset = (float)i * L->spk_gap;
        /* leve inclinação: deslocamento vertical conforme avança */
        float diag = (float)i * L->spk_gap * 0.18f;
        SDL_FRect slot = {
            L->spk_x + offset,
            L->spk_y + diag,
            L->spk_slot_w,
            L->spk_slot_h
        };
        fill_rounded_rect(r, slot, L->spk_slot_w * 0.5f);
    }
}

static void render_dmg(struct vec_ctx *ctx, struct dmg_layout *L)
{
    SDL_Renderer *r = ctx->renderer;

    SDL_SetRenderDrawColor(r, 8, 8, 8, 255);
    SDL_RenderClear(r);

    /* ── Corpo — sombra externa ── */
    set_color(r, DMG_BODY_EDGE);
    fill_rounded_rect(r, L->body, L->corner_r);

    /* ── Corpo — face frontal (inset 3px) ── */
    SDL_FRect face = { L->body.x+3, L->body.y+3, L->body.w-6, L->body.h-6 };
    set_color(r, DMG_BODY_LIGHT);
    fill_rounded_rect(r, face, L->corner_r - 3);

    /* ── Interruptor ON/OFF ── */
    set_color(r, DMG_BODY_EDGE);
    fill_rounded_rect(r, L->sw_track, L->sw_track.h * 0.4f);
    set_color(r, DMG_BODY_DARK);
    fill_rounded_rect(r, L->sw_knob,  L->sw_knob.h  * 0.4f);
    {
        float lbl_px = L->W * 0.0036f;
        set_color(r, DMG_LABEL);
        float off_x = L->sw_track.x + L->sw_track.w * 0.07f;
        draw_text(r, "OFF", off_x, L->sw_track.y + L->sw_track.h + lbl_px * 0.5f, lbl_px);
        float on_w = text_width("ON", lbl_px);
        float on_x = L->sw_track.x + L->sw_track.w - on_w - L->sw_track.w * 0.07f;
        draw_text(r, "ON", on_x, L->sw_track.y + L->sw_track.h + lbl_px * 0.5f, lbl_px);
    }

    /* ── Bezel cinza-azulado ── */
    set_color(r, DMG_BEZEL);
    fill_rounded_rect(r, L->bezel, L->corner_r * 0.35f);

    /* ── Faixas roxo+azul dentro do bezel, acima do LCD ── */
    set_color(r, DMG_STRIPE_PURP);
    fill_rounded_rect(r, L->stripe_purp, L->stripe_purp.h * 0.5f);
    set_color(r, DMG_STRIPE_BLU);
    fill_rounded_rect(r, L->stripe_blue, L->stripe_blue.h * 0.5f);

    /* ── Texto "DOT MATRIX WITH STEREO SOUND" entre as faixas e o LCD ── */
    float dot_px = L->W * 0.0042f;
    float dot_tw = text_width("DOT MATRIX WITH STEREO SOUND", dot_px);
    float dot_x  = L->bezel.x + (L->bezel.w - dot_tw) / 2.0f;
    float dot_y  = L->stripe_blue.y + L->stripe_blue.h + dot_px * 1.2f;
    set_color(r, DMG_DOT_MATRIX);
    draw_text(r, "DOT MATRIX WITH STEREO SOUND", dot_x, dot_y, dot_px);

    /* ── Fundo do LCD ── */
    set_color(r, DMG_SCREEN_BG);
    SDL_RenderFillRect(r, &L->lcd);

    /* ── LED de bateria (vermelho) ── */
    set_color(r, DMG_LED_RED);
    fill_circle(r, L->led_cx, L->led_cy, L->led_r, 24);
    set_color(r, 0xFFFF8888);
    fill_circle(r, L->led_cx - L->led_r * 0.3f, L->led_cy - L->led_r * 0.3f, L->led_r * 0.35f, 16);

    /* ── "Nintendo GAME BOY" abaixo do bezel ── */
    float brand_px = L->W * 0.0085f;
    set_color(r, DMG_BRAND);
    float brand_w = text_width("NINTENDO GAME BOY", brand_px);
    float brand_x = L->bezel.x + (L->bezel.w - brand_w) / 2.0f;
    float brand_y = L->bezel.y + L->bezel.h + L->W * 0.025f;
    draw_text(r, "NINTENDO GAME BOY", brand_x, brand_y, brand_px);

    /* ── D-Pad ── */
    draw_dmg_dpad(r, L, ctx->buttons);

    /* ── Botões A / B ── */
    draw_dmg_round_btn(r, L->btn_a_cx, L->btn_a_cy, L->btn_r, ctx->buttons[VEC_BTN_A]);
    draw_dmg_round_btn(r, L->btn_b_cx, L->btn_b_cy, L->btn_r, ctx->buttons[VEC_BTN_B]);

    float lbl_px = L->W * 0.0068f;
    set_color(r, DMG_LABEL);
    draw_text(r, "A", L->btn_a_cx + L->btn_r * 1.25f, L->btn_a_cy - 3.5f * lbl_px, lbl_px);
    draw_text(r, "B", L->btn_b_cx + L->btn_r * 1.25f, L->btn_b_cy - 3.5f * lbl_px, lbl_px);

    /* ── Select / Start — inclinados ~25° como no hardware real ── */
    float btn_angle = (float)M_PI / 7.2f; /* ~25° */
    draw_dmg_small_btn_rot(r, L->sel_rect, ctx->buttons[VEC_BTN_SELECT], btn_angle);
    draw_dmg_small_btn_rot(r, L->sta_rect, ctx->buttons[VEC_BTN_START],  btn_angle);

    float lbl_sm = L->W * 0.0052f;
    set_color(r, DMG_LABEL);
    {
        float sw = text_width("SELECT", lbl_sm);
        draw_text(r, "SELECT",
            L->sel_rect.x + (L->sel_rect.w - sw) / 2.0f,
            L->sel_rect.y + L->sel_rect.h + lbl_sm * 1.4f, lbl_sm);
    }
    {
        float sw = text_width("START", lbl_sm);
        draw_text(r, "START",
            L->sta_rect.x + (L->sta_rect.w - sw) / 2.0f,
            L->sta_rect.y + L->sta_rect.h + lbl_sm * 1.4f, lbl_sm);
    }

    /* ── Alto-falante ── */
    draw_dmg_speaker(r, L);

    /* ── Tela do jogo ── */
    SDL_RenderTexture(r, ctx->game_tex, NULL, &L->lcd);
    draw_lcd_overlay(r, L->lcd, GB_LCD_WIDTH, GB_LCD_HEIGHT);

    SDL_RenderPresent(r);
}

/* ══════════════════════════════════════════════════════════════════════
 *  LAYOUT E RENDER — GBC
 * ══════════════════════════════════════════════════════════════════════ */

static void compute_gbc_layout(struct gbc_layout *L, int win_w, int win_h)
{
    /* corpo preenche a janela inteira — sem barras pretas */
    L->W  = (float)win_w;
    L->H  = (float)win_h;
    L->ox = 0.0f;
    L->oy = 0.0f;

#define X(n)  (L->ox + (n) * L->W)
#define Y(n)  (L->oy + (n) * L->H)
#define S(n)  ((n) * L->W)
#define SH(n) ((n) * L->H)

    /* GBC tem cantos bem arredondados */
    L->corner_r = S(0.10f);
    L->body = (SDL_FRect){ X(0), Y(0), L->W, L->H };

    /* bezel preto: margem lateral ~7%, topo ~3%, altura ~45% do corpo */
    float bx = X(0.07f), by = Y(0.030f);
    float bw = S(0.86f), bh = SH(0.450f);
    L->bezel = (SDL_FRect){ bx, by, bw, bh };

    /* LCD: pad lateral ~8% do bezel, pad topo ~6%, fundo deixa ~20% para o logo */
    float lcd_pad_x = bw * 0.08f;
    float lcd_pad_t = bh * 0.06f;
    float lcd_pad_b = bh * 0.20f;
    float lcd_w = bw - 2.0f * lcd_pad_x;
    float lcd_h = bh - lcd_pad_t - lcd_pad_b;
    L->lcd = (SDL_FRect){
        bx + lcd_pad_x,
        by + lcd_pad_t,
        lcd_w,
        lcd_h
    };

    /* D-Pad: ~26% X, ~72% Y */
    L->dpad_cx    = X(0.26f);
    L->dpad_cy    = Y(0.720f);
    L->dpad_arm_w = S(0.095f);
    L->dpad_arm_h = S(0.095f);

    /* Botões A/B */
    L->btn_r    = S(0.075f);
    L->btn_a_cx = X(0.765f);
    L->btn_a_cy = Y(0.700f);
    L->btn_b_cx = X(0.630f);
    L->btn_b_cy = Y(0.735f);

    /* Select/Start — retângulos embutidos ~87% Y */
    float sw = S(0.095f), sh = SH(0.014f);
    L->sel_rect = (SDL_FRect){ X(0.27f) - sw/2, Y(0.870f) - sh/2, sw, sh };
    L->sta_rect = (SDL_FRect){ X(0.45f) - sw/2, Y(0.870f) - sh/2, sw, sh };

    /* oval Nintendo: ~50% X, ~56% Y (entre logo e D-Pad) */
    L->oval_cx = X(0.50f);
    L->oval_cy = Y(0.560f);
    L->oval_rx = S(0.22f);
    L->oval_ry = SH(0.022f);

    /* alto-falante: grade de pontos, canto inf. direito */
    L->spk_x    = X(0.55f);
    L->spk_y    = Y(0.810f);
    L->spk_dot  = S(0.020f);
    L->spk_gap  = S(0.038f);
    L->spk_cols = 5;
    L->spk_rows = 5;

#undef X
#undef Y
#undef S
#undef SH
}

static void draw_gbc_dpad(SDL_Renderer *r, struct gbc_layout *L, bool *btns)
{
    float cx = L->dpad_cx, cy = L->dpad_cy;
    float aw = L->dpad_arm_w, ah = L->dpad_arm_h;

    uint32_t col_ud = (btns[VEC_BTN_UP] || btns[VEC_BTN_DOWN]) ? GBC_DPAD_PRESS : GBC_DPAD;
    set_color(r, col_ud);
    SDL_FRect vert = { cx - aw/2, cy - ah*1.5f, aw, ah*3.0f };
    fill_rounded_rect(r, vert, aw * 0.12f);

    uint32_t col_lr = (btns[VEC_BTN_LEFT] || btns[VEC_BTN_RIGHT]) ? GBC_DPAD_PRESS : GBC_DPAD;
    set_color(r, col_lr);
    SDL_FRect horiz = { cx - ah*1.5f, cy - aw/2, ah*3.0f, aw };
    fill_rounded_rect(r, horiz, aw * 0.12f);

    /* sulco X diagonal — dois retângulos rotacionados ±45° */
    set_color(r, 0xFF080808);
    float xs = aw * 0.12f;
    float xlen = ah * 1.55f;
    fill_rotated_rect(r, cx, cy, xlen, xs/2, (float)M_PI * 0.25f);
    fill_rotated_rect(r, cx, cy, xlen, xs/2, (float)M_PI * -0.25f);

    /* setas nas pontas */
    float as = aw * 0.28f;
    set_color(r, 0xFF555555);
    fill_triangle(r, cx, cy-ah*1.5f+as*0.3f, cx-as, cy-ah*1.5f+as*1.1f, cx+as, cy-ah*1.5f+as*1.1f);
    fill_triangle(r, cx, cy+ah*1.5f-as*0.3f, cx-as, cy+ah*1.5f-as*1.1f, cx+as, cy+ah*1.5f-as*1.1f);
    fill_triangle(r, cx-ah*1.5f+as*0.3f, cy, cx-ah*1.5f+as*1.1f, cy-as, cx-ah*1.5f+as*1.1f, cy+as);
    fill_triangle(r, cx+ah*1.5f-as*0.3f, cy, cx+ah*1.5f-as*1.1f, cy-as, cx+ah*1.5f-as*1.1f, cy+as);

    if (btns[VEC_BTN_UP])    { set_color(r, GBC_DPAD_PRESS); SDL_FRect u = { cx-aw/2, cy-ah*1.5f, aw, ah }; SDL_RenderFillRect(r, &u); }
    if (btns[VEC_BTN_DOWN])  { set_color(r, GBC_DPAD_PRESS); SDL_FRect d = { cx-aw/2, cy+ah*0.5f, aw, ah }; SDL_RenderFillRect(r, &d); }
    if (btns[VEC_BTN_LEFT])  { set_color(r, GBC_DPAD_PRESS); SDL_FRect l = { cx-ah*1.5f, cy-aw/2, ah, aw }; SDL_RenderFillRect(r, &l); }
    if (btns[VEC_BTN_RIGHT]) { set_color(r, GBC_DPAD_PRESS); SDL_FRect rt = { cx+ah*0.5f, cy-aw/2, ah, aw }; SDL_RenderFillRect(r, &rt); }
}

static void draw_gbc_round_btn(SDL_Renderer *r, float cx, float cy, float radius, bool pressed,
                               const char *label, float lbl_px, uint32_t lbl_col)
{
    /* anel */
    set_color(r, GBC_BTN_AB_RING);
    fill_circle(r, cx, cy, radius + radius * 0.08f, 40);

    /* sombra */
    set_color(r, pressed ? GBC_BTN_AB_PRESS : 0xFF080808);
    fill_circle(r, cx, cy + radius * 0.06f, radius, 40);

    /* face */
    set_color(r, pressed ? GBC_BTN_AB_PRESS : GBC_BTN_AB);
    fill_circle(r, cx, cy - radius * 0.04f, radius * 0.92f, 40);

    /* reflexo sutil */
    if (!pressed) {
        set_color(r, 0xFF333333);
        fill_circle(r, cx - radius * 0.20f, cy - radius * 0.22f, radius * 0.28f, 16);
    }

    /* label dentro do botão */
    float lw = text_width(label, lbl_px);
    set_color(r, lbl_col);
    draw_text(r, label, cx - lw / 2.0f, cy - 3.5f * lbl_px, lbl_px);
}

static void draw_gbc_small_btn(SDL_Renderer *r, SDL_FRect rect, bool pressed)
{
    /* retângulo embutido — reentrância no corpo */
    set_color(r, pressed ? GBC_BTN_SMALL_P : GBC_BTN_SMALL);
    fill_rounded_rect(r, rect, rect.h * 0.45f);
    if (!pressed) {
        /* brilho superior */
        set_color(r, 0xFF1A1A1A);
        SDL_FRect top = { rect.x + rect.w * 0.1f, rect.y + rect.h * 0.1f,
                          rect.w * 0.8f, rect.h * 0.35f };
        fill_rounded_rect(r, top, top.h * 0.4f);
    }
}

/* Alto-falante GBC: grade densa de pontinhos */
static void draw_gbc_speaker(SDL_Renderer *r, struct gbc_layout *L)
{
    set_color(r, GBC_SPEAKER);
    for (int row = 0; row < L->spk_rows; row++) {
        for (int col = 0; col < L->spk_cols; col++) {
            float px = L->spk_x + (float)col * L->spk_gap;
            float py = L->spk_y + (float)row * L->spk_gap;
            SDL_FRect dot = { px, py, L->spk_dot, L->spk_dot };
            fill_rounded_rect(r, dot, L->spk_dot * 0.5f);
        }
    }
}

/* Logo "GAME BOY COLOR" multicolorido */
static void draw_gbc_logo(SDL_Renderer *r, struct gbc_layout *L)
{
    /* cada letra tem sua cor: G A M E = vermelho/laranja/laranja/amarelo
       B O Y = verde/azul/roxo   C o l o r = mesmas cores */
    static const struct { const char *ch; uint32_t col; } letters[] = {
        {"G", GBC_LOGO_G}, {"A", GBC_LOGO_A}, {"M", GBC_LOGO_M}, {"E", GBC_LOGO_E},
        {" ", 0},
        {"B", GBC_LOGO_B}, {"O", GBC_LOGO_O}, {"Y", GBC_LOGO_Y},
        {" ", 0},
        {"C", GBC_LOGO_C}, {"O", GBC_LOGO_OL}, {"L", GBC_LOGO_LO},
        {"O", GBC_LOGO_OR}, {"R", GBC_LOGO_R},
    };

    float px = L->W * 0.0090f;
    /* calcular largura total */
    float total_w = 0;
    for (int i = 0; i < (int)(sizeof(letters)/sizeof(letters[0])); i++) {
        if (letters[i].ch[0] == ' ') total_w += 4 * px;
        else total_w += 6 * px;
    }

    /* logo dentro do bezel, centralizado na faixa abaixo do LCD */
    float logo_zone_top = L->lcd.y + L->lcd.h;
    float logo_zone_bot = L->bezel.y + L->bezel.h;
    float logo_y = logo_zone_top + (logo_zone_bot - logo_zone_top - 7.0f * px) / 2.0f;
    float cx = L->bezel.x + (L->bezel.w - total_w) / 2.0f;

    for (int i = 0; i < (int)(sizeof(letters)/sizeof(letters[0])); i++) {
        if (letters[i].ch[0] == ' ') { cx += 4 * px; continue; }
        set_color(r, letters[i].col);
        draw_char(r, letters[i].ch[0], cx, logo_y, px);
        cx += 6 * px;
    }
}

static void render_gbc(struct vec_ctx *ctx, struct gbc_layout *L)
{
    SDL_Renderer *r = ctx->renderer;

    /* corpo preenche janela inteira — sem arredondamento, sem clear preto */
    set_color(r, GBC_BODY);
    SDL_RenderFillRect(r, &L->body);

    /* ── Bezel preto arredondado ── */
    set_color(r, GBC_BEZEL);
    fill_rounded_rect(r, L->bezel, L->bezel.w * 0.045f);

    /* ── Fundo do LCD arredondado ── */
    float lcd_r = L->lcd.w * 0.04f;
    set_color(r, GBC_SCREEN_BG);
    fill_rounded_rect(r, L->lcd, lcd_r);

    /* ── Logo "GAME BOY COLOR" multicolorido ── */
    draw_gbc_logo(r, L);

    /* ── Oval Nintendo no meio do corpo ── */
    set_color(r, GBC_NINTENDO_OV);
    fill_ellipse(r, L->oval_cx, L->oval_cy, L->oval_rx, L->oval_ry, 48);

    /* borda escura do oval */
    set_color(r, GBC_BODY_DARK);
    /* simula borda desenhando elipse maior fina */
    float bthick = L->oval_ry * 0.18f;
    fill_ellipse(r, L->oval_cx, L->oval_cy, L->oval_rx + bthick, L->oval_ry + bthick, 48);
    set_color(r, GBC_NINTENDO_OV);
    fill_ellipse(r, L->oval_cx, L->oval_cy, L->oval_rx, L->oval_ry, 48);

    /* texto "NINTENDO" dentro do oval */
    float ntd_px = L->W * 0.0068f;
    float ntd_w  = text_width("NINTENDO", ntd_px);
    set_color(r, GBC_NINTENDO_TXT);
    draw_text(r, "NINTENDO",
              L->oval_cx - ntd_w / 2.0f,
              L->oval_cy - 3.5f * ntd_px, ntd_px);

    /* ── D-Pad ── */
    draw_gbc_dpad(r, L, ctx->buttons);

    /* ── Botões A / B ── */
    float btn_lbl_px = L->W * 0.0058f;
    draw_gbc_round_btn(r, L->btn_a_cx, L->btn_a_cy, L->btn_r,
                       ctx->buttons[VEC_BTN_A], "A", btn_lbl_px, 0xFFCCCCCC);
    draw_gbc_round_btn(r, L->btn_b_cx, L->btn_b_cy, L->btn_r,
                       ctx->buttons[VEC_BTN_B], "B", btn_lbl_px, 0xFFCCCCCC);

    /* ── Select / Start ── */
    draw_gbc_small_btn(r, L->sel_rect, ctx->buttons[VEC_BTN_SELECT]);
    draw_gbc_small_btn(r, L->sta_rect, ctx->buttons[VEC_BTN_START]);

    float lbl_sm = L->W * 0.0048f;
    set_color(r, 0xFF1A3300);
    {
        float sw = text_width("SELECT", lbl_sm);
        draw_text(r, "SELECT",
            L->sel_rect.x + (L->sel_rect.w - sw) / 2.0f,
            L->sel_rect.y + L->sel_rect.h + lbl_sm * 1.3f, lbl_sm);
    }
    {
        float sw = text_width("START", lbl_sm);
        draw_text(r, "START",
            L->sta_rect.x + (L->sta_rect.w - sw) / 2.0f,
            L->sta_rect.y + L->sta_rect.h + lbl_sm * 1.3f, lbl_sm);
    }

    /* ── Alto-falante ── */
    draw_gbc_speaker(r, L);

    /* ── Tela do jogo ── */
    SDL_RenderTexture(r, ctx->game_tex, NULL, &L->lcd);

    /* ── Máscara de cantos arredondados do LCD ── */
    {
        float lcd_r = L->lcd.w * 0.04f;
        float lx = L->lcd.x, ly2 = L->lcd.y;
        float lw = L->lcd.w, lh = L->lcd.h;
        /* pinta o canto quadrado e depois sobrepõe o arco da cor do bezel */
        SDL_FRect corners[4] = {
            { lx,           ly2,           lcd_r, lcd_r },
            { lx + lw - lcd_r, ly2,           lcd_r, lcd_r },
            { lx,           ly2 + lh - lcd_r, lcd_r, lcd_r },
            { lx + lw - lcd_r, ly2 + lh - lcd_r, lcd_r, lcd_r },
        };
        float cxs[4] = { lx + lcd_r, lx + lw - lcd_r, lx + lcd_r, lx + lw - lcd_r };
        float cys[4] = { ly2 + lcd_r, ly2 + lcd_r, ly2 + lh - lcd_r, ly2 + lh - lcd_r };
        for (int i = 0; i < 4; i++) {
            set_color(r, GBC_BEZEL);
            SDL_RenderFillRect(r, &corners[i]);
            fill_circle(r, cxs[i], cys[i], lcd_r, 24);
            set_color(r, GBC_BEZEL);
            SDL_RenderFillRect(r, &corners[i]);
        }
        draw_lcd_overlay(r, L->lcd, GB_LCD_WIDTH, GB_LCD_HEIGHT);
    }

    SDL_RenderPresent(r);
}

/* ─── Callbacks de vídeo ─── */
static void draw_line_dmg(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
    struct vec_ctx *ctx = gb->frontend.data;
    if (ly >= GB_LCD_HEIGHT) return;
    for (unsigned i = 0; i < GB_LCD_WIDTH; i++)
        ctx->pixels[ly * GB_LCD_WIDTH + i] = DMG_PALETTE[line[i].dmg_color & 3];
}

static void draw_line_gbc(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
    struct vec_ctx *ctx = gb->frontend.data;
    if (ly >= GB_LCD_HEIGHT) return;
    for (unsigned i = 0; i < GB_LCD_WIDTH; i++) {
        uint16_t c = line[i].gbc_color;
        uint32_t rv = ((c & 0x1f) << 3) | ((c & 0x1f) >> 2);
        uint32_t gv = (((c >> 5) & 0x1f) << 3) | (((c >> 5) & 0x1f) >> 2);
        uint32_t bv = (((c >> 10) & 0x1f) << 3) | (((c >> 10) & 0x1f) >> 2);
        ctx->pixels[ly * GB_LCD_WIDTH + i] = 0xFF000000 | (rv << 16) | (gv << 8) | bv;
    }
}

static void flip_frame(struct gb *gb)
{
    struct vec_ctx *ctx = gb->frontend.data;

    /* sincroniza flag is_gbc com o estado atual da ROM */
    ctx->is_gbc = gb->gbc;

    SDL_UpdateTexture(ctx->game_tex, NULL, ctx->pixels,
                      GB_LCD_WIDTH * (int)sizeof(uint32_t));

    int ww, wh;
    SDL_GetWindowSizeInPixels(ctx->window, &ww, &wh);

    if (ctx->is_gbc) {
        struct gbc_layout L;
        compute_gbc_layout(&L, ww, wh);
        render_gbc(ctx, &L);
    } else {
        struct dmg_layout L;
        compute_dmg_layout(&L, ww, wh);
        render_dmg(ctx, &L);
    }
}

static void refresh_input(struct gb *gb) { (void)gb; }

static void destroy_frontend(struct gb *gb)
{
    struct vec_ctx *ctx = gb->frontend.data;
    if (!ctx) return;
    if (ctx->audio_stream) SDL_DestroyAudioStream(ctx->audio_stream);
    if (ctx->game_tex)     SDL_DestroyTexture(ctx->game_tex);
    if (ctx->renderer)     SDL_DestroyRenderer(ctx->renderer);
    if (ctx->window)       SDL_DestroyWindow(ctx->window);
    SDL_Quit();
    free(ctx);
    gb->frontend.data = NULL;
}

/* ─── Áudio ─── */
static void SDLCALL audio_callback(void *userdata, SDL_AudioStream *stream,
                                   int additional_amount, int total_amount)
{
    struct gb *gb = userdata;
    struct vec_ctx *ctx = gb->frontend.data;
    (void)total_amount;

    int needed = additional_amount;
    while (needed > 0) {
        struct gb_spu_sample_buffer *buf = &gb->spu.buffers[ctx->audio_buf_index];

        if (ctx->audio_buf_offset == 0 && sem_trywait(&buf->ready) != 0) {
            static const int16_t silence[GB_SPU_SAMPLE_BUFFER_LENGTH * 2] = {0};
            int chunk = needed < (int)sizeof(silence) ? needed : (int)sizeof(silence);
            SDL_PutAudioStreamData(stream, silence, chunk);
            needed -= chunk;
            continue;
        }

        size_t rem = sizeof(buf->samples) - ctx->audio_buf_offset;
        int chunk  = (int)rem < needed ? (int)rem : needed;
        SDL_PutAudioStreamData(stream,
                               (const uint8_t *)buf->samples + ctx->audio_buf_offset,
                               chunk);
        ctx->audio_buf_offset += (size_t)chunk;
        needed -= chunk;

        if (ctx->audio_buf_offset == sizeof(buf->samples)) {
            ctx->audio_buf_offset = 0;
            ctx->audio_buf_index  = (ctx->audio_buf_index + 1) % GB_SPU_SAMPLE_BUFFER_COUNT;
            sem_post(&buf->free);
        }
    }
}

/* ─── Input ─── */
static void set_button(struct gb *gb, int btn_idx, unsigned input, bool pressed)
{
    struct vec_ctx *ctx = gb->frontend.data;
    ctx->buttons[btn_idx] = pressed;
    gb_input_set(gb, input, pressed);
}

static void handle_key(struct gb *gb, SDL_Keycode key, bool pressed)
{
    struct vec_ctx *ctx = gb->frontend.data;
    switch (key) {
    case SDLK_Q:
    case SDLK_ESCAPE: if (pressed) gb->quit = true; break;
    case SDLK_RETURN: set_button(gb, VEC_BTN_START,  GB_INPUT_START,  pressed); break;
    case SDLK_RSHIFT: set_button(gb, VEC_BTN_SELECT, GB_INPUT_SELECT, pressed); break;
    case SDLK_LCTRL:  set_button(gb, VEC_BTN_A,      GB_INPUT_A,      pressed); break;
    case SDLK_LSHIFT: set_button(gb, VEC_BTN_B,      GB_INPUT_B,      pressed); break;
    case SDLK_UP:     set_button(gb, VEC_BTN_UP,     GB_INPUT_UP,     pressed); break;
    case SDLK_DOWN:   set_button(gb, VEC_BTN_DOWN,   GB_INPUT_DOWN,   pressed); break;
    case SDLK_LEFT:   set_button(gb, VEC_BTN_LEFT,   GB_INPUT_LEFT,   pressed); break;
    case SDLK_RIGHT:  set_button(gb, VEC_BTN_RIGHT,  GB_INPUT_RIGHT,  pressed); break;
    case SDLK_TAB:    ctx->fast_forward = pressed; break;
    case SDLK_F11:
        if (pressed) {
            ctx->fullscreen = !ctx->fullscreen;
            SDL_SetWindowFullscreen(ctx->window, ctx->fullscreen);
        }
        break;
    default: break;
    }
}

static void handle_gamepad_button(struct gb *gb, SDL_GamepadButton btn, bool pressed)
{
    switch (btn) {
    case SDL_GAMEPAD_BUTTON_START:      set_button(gb, VEC_BTN_START,  GB_INPUT_START,  pressed); break;
    case SDL_GAMEPAD_BUTTON_BACK:       set_button(gb, VEC_BTN_SELECT, GB_INPUT_SELECT, pressed); break;
    case SDL_GAMEPAD_BUTTON_EAST:       set_button(gb, VEC_BTN_A,      GB_INPUT_A,      pressed); break;
    case SDL_GAMEPAD_BUTTON_SOUTH:      set_button(gb, VEC_BTN_B,      GB_INPUT_B,      pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    set_button(gb, VEC_BTN_UP,     GB_INPUT_UP,     pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  set_button(gb, VEC_BTN_DOWN,   GB_INPUT_DOWN,   pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  set_button(gb, VEC_BTN_LEFT,   GB_INPUT_LEFT,   pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: set_button(gb, VEC_BTN_RIGHT,  GB_INPUT_RIGHT,  pressed); break;
    default: break;
    }
}

/* ─── Init ─── */
static bool frontend_init(struct gb *gb)
{
    struct vec_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { perror("calloc"); return false; }
    gb->frontend.data = ctx;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        free(ctx); return false;
    }

    /* janela inicial com proporção DMG; será reusada para GBC */
    int win_h = 720;
    int win_w = (int)((float)win_h * DMG_ASPECT_W / DMG_ASPECT_H);

    ctx->window = SDL_CreateWindow("Gaembuoy", win_w, win_h,
                                   SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!ctx->window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit(); free(ctx); return false;
    }

    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(ctx->window); SDL_Quit(); free(ctx); return false;
    }
    SDL_SetRenderVSync(ctx->renderer, 1);
    SDL_SetRenderDrawBlendMode(ctx->renderer, SDL_BLENDMODE_BLEND);

    ctx->game_tex = SDL_CreateTexture(ctx->renderer,
                                      SDL_PIXELFORMAT_XRGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      GB_LCD_WIDTH, GB_LCD_HEIGHT);
    if (!ctx->game_tex) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ctx->renderer);
        SDL_DestroyWindow(ctx->window);
        SDL_Quit(); free(ctx); return false;
    }
    SDL_SetTextureScaleMode(ctx->game_tex, SDL_SCALEMODE_NEAREST);

    SDL_AudioSpec spec = {
        .format   = SDL_AUDIO_S16,
        .channels = 2,
        .freq     = GB_SPU_SAMPLE_RATE_HZ,
    };
    ctx->audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, gb);
    if (ctx->audio_stream)
        SDL_ResumeAudioStreamDevice(ctx->audio_stream);
    else
        fprintf(stderr, "Aviso: sem áudio — %s\n", SDL_GetError());

    gb->frontend.draw_line_dmg = draw_line_dmg;
    gb->frontend.draw_line_gbc = draw_line_gbc;
    gb->frontend.flip          = flip_frame;
    gb->frontend.refresh_input = refresh_input;
    gb->frontend.destroy       = destroy_frontend;

    return true;
}

/* ─── Reset / carrega ROM ─── */
static void load_rom(struct gb *gb, const char *path)
{
    gb_cart_unload(gb);
    gb_cart_load(gb, path);

    gb->speed_switch_pending = false;
    gb->double_speed         = false;
    gb->timestamp            = 0;
    gb->serial_data          = 0x00;
    gb->serial_control       = 0x7e;
    gb->iram_high_bank       = 1;
    gb->vram_high_bank       = false;
    gb->ir_port              = 0;

    memset(gb->iram,  0, sizeof(gb->iram));
    memset(gb->zram,  0, sizeof(gb->zram));
    memset(gb->vram,  0, sizeof(gb->vram));
    memset(&gb->hdma, 0, sizeof(gb->hdma));
    gb->hdma.length = 0x7f;

    gb_sync_reset(gb);
    gb_irq_reset(gb);
    gb_cpu_reset(gb);
    gb_gpu_reset(gb);
    gb_input_reset(gb);
    gb_dma_reset(gb);
    gb_timer_reset(gb);
    gb_spu_reset(gb);

    {
        struct vec_ctx *ctx = gb->frontend.data;
        if (ctx) {
            ctx->audio_buf_index  = 0;
            ctx->audio_buf_offset = 0;
            ctx->is_gbc           = gb->gbc;
        }
    }
    gb_debug_init(gb);
    gb->debug.enabled = false;
    gb->debug.state   = GB_DEBUG_RUNNING;
}

/* ─── main ─── */
int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <rom.gb>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *rom_file = argv[1];

    struct gb *gb = calloc(1, sizeof(*gb));
    if (!gb) { perror("calloc"); return EXIT_FAILURE; }

    if (!frontend_init(gb)) { free(gb); return EXIT_FAILURE; }

    load_rom(gb, rom_file);
    gb->quit = false;

    SDL_Gamepad *gamepad = NULL;
    {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        if (ids && count > 0)
            gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }

    uint64_t last_ns = SDL_GetTicksNS();

    while (!gb->quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                gb->quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (!e.key.repeat)
                    handle_key(gb, e.key.key, e.key.down);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                handle_gamepad_button(gb, (SDL_GamepadButton)e.gbutton.button,
                                      e.gbutton.down);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!gamepad)
                    gamepad = SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (gamepad && SDL_GetGamepadID(gamepad) == e.gdevice.which) {
                    SDL_CloseGamepad(gamepad);
                    gamepad = NULL;
                }
                break;
            case SDL_EVENT_DROP_FILE:
                load_rom(gb, e.drop.data);
                last_ns = SDL_GetTicksNS();
                SDL_free((void *)e.drop.data);
                break;
            }
        }

        struct vec_ctx *ctx = gb->frontend.data;
        float speed = ctx->fast_forward ? 2.0f : 1.0f;

        uint64_t now_ns  = SDL_GetTicksNS();
        uint64_t elapsed = now_ns - last_ns;
        last_ns = now_ns;

        if (elapsed > 50000000ULL) elapsed = 50000000ULL;

        int32_t cycles = (int32_t)(
            (float)(elapsed * (uint64_t)GB_CPU_FREQ_HZ / 1000000000ULL) * speed);
        if (cycles > 0)
            gb_cpu_run_cycles(gb, cycles);
    }

    if (gamepad) SDL_CloseGamepad(gamepad);
    gb->frontend.destroy(gb);
    gb_cart_unload(gb);
    free(gb->bootrom);
    free(gb);

    return 0;
}
