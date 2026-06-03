/*
 * main_gba_vector.c — "Game Boy Advance vetorial": desenha o hardware GBA (AGB-001)
 * usando primitivas SDL3 (retângulos arredondados, círculos, triângulos).
 * 100% desenhado em código — sem fotos ou sprites externos.
 * Standalone: não depende do emulador GB, exibe tela preta no LCD.
 *
 * Uso: gameboy-advance-vector
 *
 * Controles:
 *   Setas         D-Pad
 *   LCtrl         A
 *   LShift        B
 *   Z             L (shoulder)
 *   X             R (shoulder)
 *   Enter         Start
 *   RShift        Select
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
#include <stdint.h>
#include <SDL3/SDL.h>

/* ── helper: lê a cor de desenho atual como SDL_FColor (0..1) ── */
static SDL_FColor current_fcolor(SDL_Renderer *r)
{
     Uint8 cr = 0, cg = 0, cb = 0, ca = 255;
     SDL_GetRenderDrawColor(r, &cr, &cg, &cb, &ca);
     SDL_FColor f = {cr / 255.0f, cg / 255.0f, cb / 255.0f, ca / 255.0f};
     return f;
}

/* ── forward declarations ── */
struct gba_layout;
static void set_color(SDL_Renderer *r, uint32_t argb);
static void fill_circle(SDL_Renderer *r, float cx, float cy, float radius, int segs);
static void fill_rounded_rect(SDL_Renderer *r, SDL_FRect rect, float rad);
static void fill_rotated_rect(SDL_Renderer *r, float cx, float cy,
                              float half_w, float half_h, float angle_rad);
static void fill_triangle(SDL_Renderer *r, float x0, float y0,
                          float x1, float y1, float x2, float y2);
static void draw_text(SDL_Renderer *r, const char *text, float x, float y, float px_size);
static float text_width(const char *text, float px_size);
static void compute_gba_layout(struct gba_layout *L, int win_w, int win_h);
static void draw_gba_dpad(SDL_Renderer *r, struct gba_layout *L, bool *btns);
static void draw_gba_round_btn(SDL_Renderer *r, float cx, float cy, float radius, bool pressed,
                               uint32_t col_btn, uint32_t col_ring, uint32_t col_shade, uint32_t col_press);
static void draw_gba_small_btn(SDL_Renderer *r, SDL_FRect rect, bool pressed);
static void draw_gba_shoulder(SDL_Renderer *r, SDL_FRect sh, bool pressed, bool is_left);
static void draw_gba_speaker(SDL_Renderer *r, struct gba_layout *L);

/* ══════════════════════════════════════════════════════════════════════
 *  CORES GBA (AGB-001 — roxo índigo)
 * ══════════════════════════════════════════════════════════════════════ */
#define GBA_BODY 0xFF4848B8         /* roxo índigo — face frontal              */
#define GBA_BODY_DARK 0xFF303090    /* roxo escuro — borda/lateral             */
#define GBA_BODY_EDGE 0xFF202060    /* roxo muito escuro — sombra externa      */
#define GBA_BEZEL 0xFF0A0A0A        /* bezel quase preto                       */
#define GBA_SCREEN_BG 0xFF1A1A1A    /* LCD apagado (preto fosco)               */
#define GBA_BTN_AB 0xFFCCCCCC       /* cinza claro — botões A/B                */
#define GBA_BTN_AB_PRESS 0xFFEEEEEE /* botões A/B pressionados                 */
#define GBA_BTN_AB_RING 0xFF888888  /* anel ao redor dos botões                */
#define GBA_BTN_AB_SHADE 0xFF555555 /* sombra inferior do botão                */
#define GBA_DPAD 0xFF282828         /* cruzeta quase preta                     */
#define GBA_DPAD_PRESS 0xFF505050   /* cruzeta pressionada                     */
#define GBA_BTN_SMALL 0xFFAAAAAA    /* Select/Start — cinza claro              */
#define GBA_BTN_SMALL_P 0xFFCCCCCC  /* Select/Start pressionado                */
#define GBA_SHOULDER 0xFF3838A0     /* shoulder L/R — roxo mais escuro         */
#define GBA_SHOULDER_D 0xFF222270   /* sombra do shoulder                      */
#define GBA_SHOULDER_P 0xFF5858C8   /* shoulder pressionado                    */
#define GBA_LED_GREEN 0xFF22CC22    /* power LED verde                         */
#define GBA_LED_SHINE 0xFF88FF88    /* brilho do LED                           */
#define GBA_SPEAKER 0xFF303090      /* slots do alto-falante                   */
#define GBA_LABEL 0xFFDDDDDD        /* texto branco/cinza claro                */
#define GBA_BRAND 0xFFEEEEEE        /* "GAME BOY ADVANCE" / "Nintendo"         */

/* ─── Proporção GBA AGB-001: ~144mm × 82mm → W:H ≈ 1.756:1 ─── */
#define GBA_ASPECT_W 1.756f
#define GBA_ASPECT_H 1.0f

/* GBA LCD: 240×160 px */
#define GBA_LCD_W 240
#define GBA_LCD_H 160

/* ─── Enum botões ─── */
enum
{
     GBA_BTN_A = 0,
     GBA_BTN_B,
     GBA_BTN_START,
     GBA_BTN_SELECT,
     GBA_BTN_UP,
     GBA_BTN_DOWN,
     GBA_BTN_LEFT,
     GBA_BTN_RIGHT,
     GBA_BTN_L,
     GBA_BTN_R,
     GBA_BTN_COUNT
};

struct gba_vec_ctx
{
     SDL_Window *window;
     SDL_Renderer *renderer;
     bool buttons[GBA_BTN_COUNT];
     bool fullscreen;
};

/* ─── Geometria GBA ─── */
struct gba_layout
{
     float W, H;
     float ox, oy;

     /* corpo principal */
     SDL_FRect body;
     float corner_r;

     /* bezel + LCD */
     SDL_FRect bezel;
     SDL_FRect lcd;

     /* D-Pad */
     float dpad_cx, dpad_cy;
     float dpad_arm_w, dpad_arm_h;

     /* botões A/B */
     float btn_a_cx, btn_a_cy, btn_r_a;
     float btn_b_cx, btn_b_cy, btn_r_b;

     /* Select / Start */
     SDL_FRect sel_rect, sta_rect;

     /* Shoulders */
     SDL_FRect shoulder_l, shoulder_r;

     /* LED */
     float led_cx, led_cy, led_r;

     /* Speaker */
     float spk_x, spk_y;
     float spk_slot_w, spk_slot_h, spk_gap;
     int spk_slots;
};

/* ══════════════════════════════════════════════════════════════════════
 *  PRIMITIVAS DE DESENHO (SDL3)
 * ══════════════════════════════════════════════════════════════════════ */

static void set_color(SDL_Renderer *r, uint32_t argb)
{
     SDL_SetRenderDrawColor(r,
                            (argb >> 16) & 0xFF,
                            (argb >> 8) & 0xFF,
                            (argb >> 0) & 0xFF,
                            (argb >> 24) & 0xFF);
}

static void fill_circle(SDL_Renderer *r, float cx, float cy, float radius, int segs)
{
     if (segs < 8)
          segs = 8;
     SDL_Vertex verts[3];
     verts[0].position.x = cx;
     verts[0].position.y = cy;
     verts[0].color = current_fcolor(r);
     verts[1].color = verts[0].color;
     verts[2].color = verts[0].color;
     verts[0].tex_coord.x = verts[0].tex_coord.y = 0;
     verts[1].tex_coord.x = verts[1].tex_coord.y = 0;
     verts[2].tex_coord.x = verts[2].tex_coord.y = 0;

     for (int i = 0; i < segs; i++)
     {
          float a0 = (float)i / (float)segs * 2.0f * (float)M_PI;
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
     if (rad <= 0)
     {
          SDL_RenderFillRect(r, &rect);
          return;
     }
     if (rad > rect.w / 2)
          rad = rect.w / 2;
     if (rad > rect.h / 2)
          rad = rect.h / 2;

     SDL_FRect hr = {rect.x, rect.y + rad, rect.w, rect.h - 2 * rad};
     SDL_RenderFillRect(r, &hr);
     SDL_FRect vr = {rect.x + rad, rect.y, rect.w - 2 * rad, rad};
     SDL_RenderFillRect(r, &vr);
     vr.y = rect.y + rect.h - rad;
     SDL_RenderFillRect(r, &vr);

     fill_circle(r, rect.x + rad, rect.y + rad, rad, 24);
     fill_circle(r, rect.x + rect.w - rad, rect.y + rad, rad, 24);
     fill_circle(r, rect.x + rad, rect.y + rect.h - rad, rad, 24);
     fill_circle(r, rect.x + rect.w - rad, rect.y + rect.h - rad, rad, 24);
}

static void fill_rotated_rect(SDL_Renderer *r,
                              float cx, float cy,
                              float half_w, float half_h,
                              float angle_rad)
{
     float ca = cosf(angle_rad), sa = sinf(angle_rad);
     float corners[4][2] = {
         {-half_w, -half_h},
         {half_w, -half_h},
         {half_w, half_h},
         {-half_w, half_h},
     };
     SDL_FColor col = current_fcolor(r);
     SDL_Vertex v[4];
     for (int i = 0; i < 4; i++)
     {
          v[i].position.x = cx + corners[i][0] * ca - corners[i][1] * sa;
          v[i].position.y = cy + corners[i][0] * sa + corners[i][1] * ca;
          v[i].color = col;
          v[i].tex_coord.x = v[i].tex_coord.y = 0;
     }
     int idx[6] = {0, 1, 2, 0, 2, 3};
     SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
}

static void fill_triangle(SDL_Renderer *r,
                          float x0, float y0,
                          float x1, float y1,
                          float x2, float y2)
{
     SDL_Vertex v[3];
     v[0].color = current_fcolor(r);
     v[1].color = v[2].color = v[0].color;
     v[0].tex_coord.x = v[0].tex_coord.y = 0;
     v[1].tex_coord.x = v[1].tex_coord.y = 0;
     v[2].tex_coord.x = v[2].tex_coord.y = 0;
     v[0].position.x = x0;
     v[0].position.y = y0;
     v[1].position.x = x1;
     v[1].position.y = y1;
     v[2].position.x = x2;
     v[2].position.y = y2;
     SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

static void draw_lcd_overlay(SDL_Renderer *r, SDL_FRect lcd)
{
     SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
     SDL_SetRenderDrawColor(r, 0, 0, 0, 40);
     float cell_w = lcd.w / (float)GBA_LCD_W;
     float cell_h = lcd.h / (float)GBA_LCD_H;
     int step_x = (cell_w < 3.0f) ? (int)(3.0f / cell_w) + 1 : 1;
     int step_y = (cell_h < 3.0f) ? (int)(3.0f / cell_h) + 1 : 1;
     for (int x = step_x; x < GBA_LCD_W; x += step_x)
     {
          float fx = lcd.x + (float)x * cell_w;
          SDL_RenderLine(r, fx, lcd.y, fx, lcd.y + lcd.h);
     }
     for (int y = step_y; y < GBA_LCD_H; y += step_y)
     {
          float fy = lcd.y + (float)y * cell_h;
          SDL_RenderLine(r, lcd.x, fy, lcd.x + lcd.w, fy);
     }

     /* reflexo de luz no canto superior esquerdo */
     SDL_Vertex rv[3];
     rv[0].position.x = lcd.x;
     rv[0].position.y = lcd.y;
     rv[1].position.x = lcd.x + lcd.w * 0.5f;
     rv[1].position.y = lcd.y;
     rv[2].position.x = lcd.x;
     rv[2].position.y = lcd.y + lcd.h * 0.4f;
     rv[0].color = (SDL_FColor){1, 1, 1, 0.10f};
     rv[1].color = (SDL_FColor){1, 1, 1, 0.00f};
     rv[2].color = (SDL_FColor){1, 1, 1, 0.00f};
     rv[0].tex_coord.x = rv[0].tex_coord.y = 0;
     rv[1].tex_coord.x = rv[1].tex_coord.y = 0;
     rv[2].tex_coord.x = rv[2].tex_coord.y = 0;
     SDL_RenderGeometry(r, NULL, rv, 3, NULL, 0);
}

/* ─── Fonte 5×7 ─── */
static const uint8_t FONT5X7[][5] = {
    {0x7C, 0x12, 0x11, 0x12, 0x7C}, /* A */
    {0x7F, 0x49, 0x49, 0x49, 0x36}, /* B */
    {0x3E, 0x41, 0x41, 0x41, 0x22}, /* C */
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, /* D */
    {0x7F, 0x49, 0x49, 0x41, 0x41}, /* E */
    {0x7F, 0x09, 0x09, 0x01, 0x01}, /* F */
    {0x3E, 0x41, 0x41, 0x51, 0x72}, /* G */
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, /* H */
    {0x00, 0x41, 0x7F, 0x41, 0x00}, /* I */
    {0x20, 0x40, 0x41, 0x3F, 0x01}, /* J */
    {0x7F, 0x08, 0x14, 0x22, 0x41}, /* K */
    {0x7F, 0x40, 0x40, 0x40, 0x40}, /* L */
    {0x7F, 0x02, 0x04, 0x02, 0x7F}, /* M */
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, /* N */
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, /* O */
    {0x7F, 0x09, 0x09, 0x09, 0x06}, /* P */
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, /* Q */
    {0x7F, 0x09, 0x19, 0x29, 0x46}, /* R */
    {0x46, 0x49, 0x49, 0x49, 0x31}, /* S */
    {0x01, 0x01, 0x7F, 0x01, 0x01}, /* T */
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, /* U */
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, /* V */
    {0x7F, 0x20, 0x18, 0x20, 0x7F}, /* W */
    {0x63, 0x14, 0x08, 0x14, 0x63}, /* X */
    {0x03, 0x04, 0x78, 0x04, 0x03}, /* Y */
    {0x61, 0x51, 0x49, 0x45, 0x43}, /* Z */
};

static void draw_char(SDL_Renderer *r, char c, float x, float y, float px_size)
{
     if (c < 'A' || c > 'Z')
          return;
     const uint8_t *col_data = FONT5X7[(int)(c - 'A')];
     for (int col = 0; col < 5; col++)
          for (int row = 0; row < 7; row++)
               if (col_data[col] & (1 << row))
               {
                    SDL_FRect dot = {
                        x + (float)col * px_size,
                        y + (float)row * px_size,
                        px_size, px_size};
                    SDL_RenderFillRect(r, &dot);
               }
}

static void draw_text(SDL_Renderer *r, const char *text, float x, float y, float px_size)
{
     float cx = x;
     for (; *text; text++)
     {
          char c = (*text >= 'a' && *text <= 'z') ? (char)(*text - 32) : *text;
          if (c == ' ')
          {
               cx += 4 * px_size;
               continue;
          }
          draw_char(r, c, cx, y, px_size);
          cx += 6 * px_size;
     }
}

static float text_width(const char *text, float px_size)
{
     float w = 0;
     for (; *text; text++)
          w += (*text == ' ') ? 4 * px_size : 6 * px_size;
     return w;
}

/* ══════════════════════════════════════════════════════════════════════
 *  CORPO DO GBA — perfil AGB-001
 *
 *  O AGB-001 tem formato de "elipse achatada": os cantos são muito
 *  arredondados — especialmente os inferiores — e os grips laterais
 *  se projetam para fora na metade inferior do corpo.
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_gba_body_shape(SDL_Renderer *r, SDL_FRect body, float corner_r, uint32_t fill)
{
     set_color(r, fill);

     /* retângulo central + arredondamento uniforme */
     fill_rounded_rect(r, body, corner_r);

     /* grips laterais: projeção para fora na metade inferior
      * (dão o perfil "asa" característico do AGB-001) */
     float grip_w = body.w * 0.055f;
     float grip_h = body.h * 0.62f;
     float grip_cy = body.y + body.h * 0.58f;

     SDL_FRect grip_l = {
         body.x - grip_w * 0.70f,
         grip_cy - grip_h / 2.0f,
         grip_w * 1.4f, grip_h};
     SDL_FRect grip_r = {
         body.x + body.w - grip_w * 0.70f,
         grip_cy - grip_h / 2.0f,
         grip_w * 1.4f, grip_h};
     fill_rounded_rect(r, grip_l, grip_w * 0.5f);
     fill_rounded_rect(r, grip_r, grip_w * 0.5f);
}

/* ══════════════════════════════════════════════════════════════════════
 *  LAYOUT GBA
 * ══════════════════════════════════════════════════════════════════════ */

static void compute_gba_layout(struct gba_layout *L, int win_w, int win_h)
{
     float sx = (float)win_w / GBA_ASPECT_W;
     float sy = (float)win_h / GBA_ASPECT_H;
     float scale = sx < sy ? sx : sy;

     L->W = GBA_ASPECT_W * scale;
     L->H = GBA_ASPECT_H * scale;
     L->ox = ((float)win_w - L->W) / 2.0f;
     L->oy = ((float)win_h - L->H) / 2.0f;

     /* corpo: recuado lateralmente para deixar espaço aos grips */
     float inset_x = L->W * 0.048f;
     float inset_y = L->H * 0.025f;
     L->body = (SDL_FRect){
         L->ox + inset_x,
         L->oy + inset_y,
         L->W - 2.0f * inset_x,
         L->H - 2.0f * inset_y};
     L->corner_r = L->body.h * 0.28f; /* cantos muito arredondados — AGB-001 */

/* macros relativas ao corpo */
#define X(n) (L->body.x + (n) * L->body.w)
#define Y(n) (L->body.y + (n) * L->body.h)
#define S(n) ((n) * L->body.w)
#define SH(n) ((n) * L->body.h)

     /* ── bezel: centralizado, ocupa ~58% da largura, mais para o topo ── */
     float bw = S(0.590f);
     float bh = SH(0.580f);
     float bx = X(0.500f) - bw / 2.0f;
     float by = Y(0.090f);
     L->bezel = (SDL_FRect){bx, by, bw, bh};

     /* ── LCD dentro do bezel ── */
     float lpad_x = bw * 0.055f;
     float lpad_t = bh * 0.100f;
     float lpad_b = bh * 0.065f;
     L->lcd = (SDL_FRect){
         bx + lpad_x,
         by + lpad_t,
         bw - 2.0f * lpad_x,
         bh - lpad_t - lpad_b};

     /* ── D-Pad: lado esquerdo, verticalmente acima do meio ──
      * Na foto o D-Pad fica na metade esquerda, bem mais para cima
      * que o centro do corpo. */
     L->dpad_cx = X(0.130f);
     L->dpad_cy = Y(0.530f);
     L->dpad_arm_w = S(0.058f);
     L->dpad_arm_h = S(0.058f);

     /* ── Botões A e B: diagonal AGB ──
      * Na foto: B fica acima/esquerda, A fica abaixo/direita.
      * A é ligeiramente maior que B. */
     L->btn_r_b = S(0.042f);
     L->btn_r_a = S(0.048f);
     L->btn_b_cx = X(0.830f);
     L->btn_b_cy = Y(0.440f);
     L->btn_a_cx = X(0.910f);
     L->btn_a_cy = Y(0.580f);

     /* ── Select / Start: dois botões pequenos à esquerda do centro,
      * abaixo do LCD, empilhados verticalmente (Start acima, Select abaixo)
      * — na foto ficam à esquerda do centro da largura do corpo. ── */
     float ssw = S(0.072f);
     float ssh = SH(0.055f);
     /* Start fica acima de Select na foto */
     L->sta_rect = (SDL_FRect){X(0.390f) - ssw / 2.0f, Y(0.730f), ssw, ssh};
     L->sel_rect = (SDL_FRect){X(0.390f) - ssw / 2.0f, Y(0.730f) + ssh * 1.55f, ssw, ssh};

     /* ── Shoulders L/R: faixas na borda superior dos cantos ── */
     float sh_w = S(0.195f);
     float sh_h = SH(0.118f);
     L->shoulder_l = (SDL_FRect){X(0.000f), Y(-0.005f), sh_w, sh_h};
     L->shoulder_r = (SDL_FRect){X(1.000f) - sh_w, Y(-0.005f), sh_w, sh_h};

     /* ── LED: canto superior DIREITO, ao lado do bezel ── */
     L->led_cx = X(0.890f);
     L->led_cy = Y(0.175f);
     L->led_r = S(0.012f);

     /* ── Speaker: 6 slots horizontais paralelos, canto inferior direito ── */
     L->spk_x = X(0.820f);
     L->spk_y = Y(0.570f);
     L->spk_slot_w = S(0.090f);
     L->spk_slot_h = SH(0.030f);
     L->spk_gap = SH(0.058f);
     L->spk_slots = 6;

#undef X
#undef Y
#undef S
#undef SH
}

/* ══════════════════════════════════════════════════════════════════════
 *  COMPONENTES DE DESENHO
 * ══════════════════════════════════════════════════════════════════════ */

static void draw_gba_dpad(SDL_Renderer *r, struct gba_layout *L, bool *btns)
{
     float cx = L->dpad_cx, cy = L->dpad_cy;
     float aw = L->dpad_arm_w, ah = L->dpad_arm_h;

     uint32_t col_ud = (btns[GBA_BTN_UP] || btns[GBA_BTN_DOWN]) ? GBA_DPAD_PRESS : GBA_DPAD;
     uint32_t col_lr = (btns[GBA_BTN_LEFT] || btns[GBA_BTN_RIGHT]) ? GBA_DPAD_PRESS : GBA_DPAD;

     /* braço vertical */
     set_color(r, col_ud);
     SDL_FRect vert = {cx - aw / 2, cy - ah * 1.5f, aw, ah * 3.0f};
     fill_rounded_rect(r, vert, aw * 0.14f);

     /* braço horizontal */
     set_color(r, col_lr);
     SDL_FRect horiz = {cx - ah * 1.5f, cy - aw / 2, ah * 3.0f, aw};
     fill_rounded_rect(r, horiz, aw * 0.14f);

     /* sulco diagonal (acabamento) */
     set_color(r, 0xFF080808);
     float xs = aw * 0.13f;
     float xlen = ah * 1.55f;
     fill_rotated_rect(r, cx, cy, xlen, xs / 2, (float)M_PI * 0.25f);
     fill_rotated_rect(r, cx, cy, xlen, xs / 2, -(float)M_PI * 0.25f);

     /* setas nas pontas */
     float as = aw * 0.26f;
     set_color(r, 0xFF666666);
     fill_triangle(r,
                   cx, cy - ah * 1.5f + as * 0.3f,
                   cx - as, cy - ah * 1.5f + as * 1.1f,
                   cx + as, cy - ah * 1.5f + as * 1.1f);
     fill_triangle(r,
                   cx, cy + ah * 1.5f - as * 0.3f,
                   cx - as, cy + ah * 1.5f - as * 1.1f,
                   cx + as, cy + ah * 1.5f - as * 1.1f);
     fill_triangle(r,
                   cx - ah * 1.5f + as * 0.3f, cy,
                   cx - ah * 1.5f + as * 1.1f, cy - as,
                   cx - ah * 1.5f + as * 1.1f, cy + as);
     fill_triangle(r,
                   cx + ah * 1.5f - as * 0.3f, cy,
                   cx + ah * 1.5f - as * 1.1f, cy - as,
                   cx + ah * 1.5f - as * 1.1f, cy + as);

     /* highlight do braço pressionado */
     if (btns[GBA_BTN_UP])
     {
          set_color(r, GBA_DPAD_PRESS);
          SDL_FRect u = {cx - aw / 2, cy - ah * 1.5f, aw, ah};
          SDL_RenderFillRect(r, &u);
     }
     if (btns[GBA_BTN_DOWN])
     {
          set_color(r, GBA_DPAD_PRESS);
          SDL_FRect d = {cx - aw / 2, cy + ah * 0.5f, aw, ah};
          SDL_RenderFillRect(r, &d);
     }
     if (btns[GBA_BTN_LEFT])
     {
          set_color(r, GBA_DPAD_PRESS);
          SDL_FRect l = {cx - ah * 1.5f, cy - aw / 2, ah, aw};
          SDL_RenderFillRect(r, &l);
     }
     if (btns[GBA_BTN_RIGHT])
     {
          set_color(r, GBA_DPAD_PRESS);
          SDL_FRect rt = {cx + ah * 0.5f, cy - aw / 2, ah, aw};
          SDL_RenderFillRect(r, &rt);
     }
}

static void draw_gba_round_btn(SDL_Renderer *r,
                               float cx, float cy, float radius, bool pressed,
                               uint32_t col_btn, uint32_t col_ring,
                               uint32_t col_shade, uint32_t col_press)
{
     /* anel externo */
     set_color(r, col_ring);
     fill_circle(r, cx, cy, radius + radius * 0.12f, 40);

     /* sombra inferior */
     set_color(r, col_shade);
     fill_circle(r, cx, cy + radius * 0.07f, radius, 40);

     /* face do botão */
     set_color(r, pressed ? col_press : col_btn);
     fill_circle(r, cx, cy - radius * 0.04f, radius * 0.92f, 40);

     /* brilho */
     if (!pressed)
     {
          set_color(r, 0xFFEEEEEE);
          fill_circle(r, cx - radius * 0.20f, cy - radius * 0.25f, radius * 0.28f, 20);
     }
}

static void draw_gba_small_btn(SDL_Renderer *r, SDL_FRect rect, bool pressed)
{
     float ry = rect.h / 2.0f;

     /* sombra */
     set_color(r, 0xFF606060);
     SDL_FRect shadow = {rect.x + 1, rect.y + 2, rect.w, rect.h};
     fill_rounded_rect(r, shadow, ry * 0.5f);

     set_color(r, pressed ? GBA_BTN_SMALL_P : GBA_BTN_SMALL);
     fill_rounded_rect(r, rect, ry * 0.5f);

     /* brilho */
     if (!pressed)
     {
          set_color(r, 0xFFDDDDDD);
          SDL_FRect shine = {rect.x + rect.w * 0.10f, rect.y + rect.h * 0.10f,
                             rect.w * 0.55f, rect.h * 0.40f};
          fill_rounded_rect(r, shine, ry * 0.3f);
     }
}

static void draw_gba_shoulder(SDL_Renderer *r, SDL_FRect sh, bool pressed, bool is_left)
{
     /* sombra */
     set_color(r, GBA_SHOULDER_D);
     SDL_FRect shadow = {sh.x + 2, sh.y + 3, sh.w, sh.h};
     fill_rounded_rect(r, shadow, sh.h * 0.38f);

     set_color(r, pressed ? GBA_SHOULDER_P : GBA_SHOULDER);
     fill_rounded_rect(r, sh, sh.h * 0.38f);

     /* brilho na borda interna */
     if (!pressed)
     {
          set_color(r, 0xFF5858D8);
          float bx = is_left ? sh.x + sh.w * 0.55f : sh.x + sh.w * 0.05f;
          SDL_FRect shine = {bx, sh.y + sh.h * 0.15f, sh.w * 0.38f, sh.h * 0.38f};
          fill_rounded_rect(r, shine, sh.h * 0.15f);
     }

     /* label L ou R */
     float lbl_px = sh.h * 0.42f;
     set_color(r, 0xFFCCCCEE);
     float lw = text_width(is_left ? "L" : "R", lbl_px);
     float lx = is_left
                    ? sh.x + sh.w * 0.38f - lw / 2
                    : sh.x + sh.w * 0.62f - lw / 2;
     draw_text(r, is_left ? "L" : "R", lx, sh.y + sh.h * 0.22f, lbl_px);
}

/* slots horizontais paralelos — perfil real do AGB-001 */
static void draw_gba_speaker(SDL_Renderer *r, struct gba_layout *L)
{
     for (int i = 0; i < L->spk_slots; i++)
     {
          float oy = (float)i * L->spk_gap;
          SDL_FRect slot = {L->spk_x, L->spk_y + oy, L->spk_slot_w, L->spk_slot_h};

          /* sombra do slot */
          set_color(r, 0xFF202060);
          SDL_FRect sh = {slot.x + 1, slot.y + 2, slot.w, slot.h};
          fill_rounded_rect(r, sh, slot.h * 0.4f);

          set_color(r, GBA_SPEAKER);
          fill_rounded_rect(r, slot, slot.h * 0.4f);
     }
}

/* ══════════════════════════════════════════════════════════════════════
 *  RENDER PRINCIPAL
 * ══════════════════════════════════════════════════════════════════════ */

static void render_gba(struct gba_vec_ctx *ctx, struct gba_layout *L)
{
     SDL_Renderer *r = ctx->renderer;
     SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

     /* fundo preto */
     SDL_SetRenderDrawColor(r, 8, 8, 8, 255);
     SDL_RenderClear(r);

     /* ── sombra externa do corpo ── */
     draw_gba_body_shape(r, L->body, L->corner_r, GBA_BODY_EDGE);

     /* ── face frontal (inset 3px) ── */
     SDL_FRect face = {L->body.x + 3, L->body.y + 3,
                       L->body.w - 6, L->body.h - 6};
     draw_gba_body_shape(r, face, L->corner_r - 3, GBA_BODY);

     /* ── brilho sutil na metade superior ── */
     SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
     {
          SDL_Vertex rv[3];
          rv[0].position.x = face.x;
          rv[0].position.y = face.y;
          rv[1].position.x = face.x + face.w;
          rv[1].position.y = face.y;
          rv[2].position.x = face.x + face.w / 2;
          rv[2].position.y = face.y + face.h * 0.42f;
          rv[0].color = (SDL_FColor){1, 1, 1, 0.07f};
          rv[1].color = (SDL_FColor){1, 1, 1, 0.07f};
          rv[2].color = (SDL_FColor){1, 1, 1, 0.00f};
          rv[0].tex_coord.x = rv[0].tex_coord.y = 0;
          rv[1].tex_coord.x = rv[1].tex_coord.y = 0;
          rv[2].tex_coord.x = rv[2].tex_coord.y = 0;
          SDL_RenderGeometry(r, NULL, rv, 3, NULL, 0);
     }
     SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

     /* ── Shoulder L e R ── */
     draw_gba_shoulder(r, L->shoulder_l, ctx->buttons[GBA_BTN_L], true);
     draw_gba_shoulder(r, L->shoulder_r, ctx->buttons[GBA_BTN_R], false);

     /* ── "Nintendo" impressa no corpo, acima do bezel ── */
     {
          float px = L->body.w * 0.0095f;
          float tw = text_width("NINTENDO", px);
          float tx = L->bezel.x + (L->bezel.w - tw) / 2.0f;
          float ty = L->body.y + (L->bezel.y - L->body.y) * 0.30f;
          set_color(r, 0xFFCCCCEE);
          draw_text(r, "NINTENDO", tx, ty, px);
     }

     /* ── Bezel preto ── */
     set_color(r, GBA_BEZEL);
     fill_rounded_rect(r, L->bezel, L->bezel.h * 0.060f);

     /* ── Fundo do LCD ── */
     set_color(r, GBA_SCREEN_BG);
     SDL_RenderFillRect(r, &L->lcd);

     /* ── Overlay do LCD ── */
     SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
     draw_lcd_overlay(r, L->lcd);
     SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

     /* ── LED verde (power) — canto superior direito ── */
     set_color(r, GBA_LED_GREEN);
     fill_circle(r, L->led_cx, L->led_cy, L->led_r, 24);
     set_color(r, GBA_LED_SHINE);
     fill_circle(r, L->led_cx - L->led_r * 0.3f, L->led_cy - L->led_r * 0.3f,
                 L->led_r * 0.40f, 16);

     /* "POWER" ao lado do LED */
     {
          float px = L->led_r * 0.55f;
          set_color(r, 0xFF88BB88);
          draw_text(r, "POWER", L->led_cx + L->led_r * 1.6f, L->led_cy - px * 3.5f, px);
     }

     /* ── D-Pad ── */
     draw_gba_dpad(r, L, ctx->buttons);

     /* ── Botões A e B ── */
     draw_gba_round_btn(r, L->btn_a_cx, L->btn_a_cy, L->btn_r_a,
                        ctx->buttons[GBA_BTN_A],
                        GBA_BTN_AB, GBA_BTN_AB_RING, GBA_BTN_AB_SHADE, GBA_BTN_AB_PRESS);
     draw_gba_round_btn(r, L->btn_b_cx, L->btn_b_cy, L->btn_r_b,
                        ctx->buttons[GBA_BTN_B],
                        GBA_BTN_AB, GBA_BTN_AB_RING, GBA_BTN_AB_SHADE, GBA_BTN_AB_PRESS);

     /* labels A e B */
     {
          float lbl_px = L->btn_r_a * 0.58f;
          set_color(r, GBA_LABEL);

          /* "B" acima do botão B */
          float bw = text_width("B", lbl_px);
          draw_text(r, "B",
                    L->btn_b_cx - bw / 2,
                    L->btn_b_cy - L->btn_r_b - lbl_px * 8.5f,
                    lbl_px);

          /* "A" acima do botão A */
          float aw = text_width("A", lbl_px);
          draw_text(r, "A",
                    L->btn_a_cx - aw / 2,
                    L->btn_a_cy - L->btn_r_a - lbl_px * 8.5f,
                    lbl_px);
     }

     /* ── Select / Start ── */
     draw_gba_small_btn(r, L->sel_rect, ctx->buttons[GBA_BTN_SELECT]);
     draw_gba_small_btn(r, L->sta_rect, ctx->buttons[GBA_BTN_START]);

     /* labels acima de cada botão */
     {
          float lbl_sm = L->sel_rect.w * 0.085f;
          set_color(r, GBA_LABEL);

          float stw = text_width("START", lbl_sm);
          draw_text(r, "START",
                    L->sta_rect.x + (L->sta_rect.w - stw) / 2.0f,
                    L->sta_rect.y - lbl_sm * 9.5f,
                    lbl_sm);

          float sw = text_width("SELECT", lbl_sm);
          draw_text(r, "SELECT",
                    L->sel_rect.x + (L->sel_rect.w - sw) / 2.0f,
                    L->sel_rect.y - lbl_sm * 9.5f,
                    lbl_sm);
     }

     /* ── Speaker: slots horizontais ── */
     draw_gba_speaker(r, L);

     /* ── "GAME BOY ADVANCE" abaixo do bezel ── */
     {
          float brand_px = L->W * 0.0058f;
          set_color(r, GBA_BRAND);
          float brand_w = text_width("GAME BOY ADVANCE", brand_px);
          float brand_x = L->bezel.x + (L->bezel.w - brand_w) / 2.0f;
          float brand_y = L->bezel.y + L->bezel.h + L->H * 0.028f;
          draw_text(r, "GAME BOY ADVANCE", brand_x, brand_y, brand_px);
     }

     SDL_RenderPresent(r);
}

/* ══════════════════════════════════════════════════════════════════════
 *  ENTRADA
 * ══════════════════════════════════════════════════════════════════════ */

static void handle_key(struct gba_vec_ctx *ctx, SDL_Keycode key, bool pressed)
{
     switch (key)
     {
     case SDLK_Q:
     case SDLK_ESCAPE:
          break; /* tratado no loop */
     case SDLK_RETURN:
          ctx->buttons[GBA_BTN_START] = pressed;
          break;
     case SDLK_RSHIFT:
          ctx->buttons[GBA_BTN_SELECT] = pressed;
          break;
     case SDLK_LCTRL:
          ctx->buttons[GBA_BTN_A] = pressed;
          break;
     case SDLK_LSHIFT:
          ctx->buttons[GBA_BTN_B] = pressed;
          break;
     case SDLK_Z:
          ctx->buttons[GBA_BTN_L] = pressed;
          break;
     case SDLK_X:
          ctx->buttons[GBA_BTN_R] = pressed;
          break;
     case SDLK_UP:
          ctx->buttons[GBA_BTN_UP] = pressed;
          break;
     case SDLK_DOWN:
          ctx->buttons[GBA_BTN_DOWN] = pressed;
          break;
     case SDLK_LEFT:
          ctx->buttons[GBA_BTN_LEFT] = pressed;
          break;
     case SDLK_RIGHT:
          ctx->buttons[GBA_BTN_RIGHT] = pressed;
          break;
     case SDLK_F11:
          if (pressed)
          {
               ctx->fullscreen = !ctx->fullscreen;
               SDL_SetWindowFullscreen(ctx->window, ctx->fullscreen);
          }
          break;
     default:
          break;
     }
}

static void handle_gamepad_button(struct gba_vec_ctx *ctx,
                                  SDL_GamepadButton btn, bool pressed)
{
     switch (btn)
     {
     case SDL_GAMEPAD_BUTTON_START:
          ctx->buttons[GBA_BTN_START] = pressed;
          break;
     case SDL_GAMEPAD_BUTTON_BACK:
          ctx->buttons[GBA_BTN_SELECT] = pressed;
          break;
     case SDL_GAMEPAD_BUTTON_EAST:
          ctx->buttons[GBA_BTN_A] = pressed;
          break;
     case SDL_GAMEPAD_BUTTON_SOUTH:
          ctx->buttons[GBA_BTN_B] = pressed;
          break;
     case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
          ctx->buttons[GBA_BTN_L] = pressed;
          break;
     case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
          ctx->buttons[GBA_BTN_R] = pressed;
          break;
     case SDL_GAMEPAD_BUTTON_DPAD_UP:
          ctx->buttons[GBA_BTN_UP] = pressed;
          break;
     case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
          ctx->buttons[GBA_BTN_DOWN] = pressed;
          break;
     case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
          ctx->buttons[GBA_BTN_LEFT] = pressed;
          break;
     case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
          ctx->buttons[GBA_BTN_RIGHT] = pressed;
          break;
     default:
          break;
     }
}

/* ══════════════════════════════════════════════════════════════════════
 *  MAIN
 * ══════════════════════════════════════════════════════════════════════ */

int main(int argc, char **argv)
{
     (void)argc;
     (void)argv;

     if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
     {
          fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
          return 1;
     }

     int win_h = 512;
     int win_w = (int)((float)win_h * GBA_ASPECT_W / GBA_ASPECT_H);

     SDL_Window *window = SDL_CreateWindow("Game Boy Advance — vetorial",
                                           win_w, win_h,
                                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
     if (!window)
     {
          fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
          SDL_Quit();
          return 1;
     }

     SDL_Renderer *renderer = SDL_CreateRenderer(window, NULL);
     if (!renderer)
     {
          fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
          SDL_DestroyWindow(window);
          SDL_Quit();
          return 1;
     }
     SDL_SetRenderVSync(renderer, 1);
     SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

     struct gba_vec_ctx ctx = {0};
     ctx.window = window;
     ctx.renderer = renderer;

     SDL_Gamepad *gamepad = NULL;
     {
          int count = 0;
          SDL_JoystickID *ids = SDL_GetGamepads(&count);
          if (ids && count > 0)
               gamepad = SDL_OpenGamepad(ids[0]);
          SDL_free(ids);
     }

     bool running = true;
     while (running)
     {
          SDL_Event ev;
          while (SDL_PollEvent(&ev))
          {
               switch (ev.type)
               {
               case SDL_EVENT_QUIT:
                    running = false;
                    break;
               case SDL_EVENT_KEY_DOWN:
                    if (ev.key.key == SDLK_Q || ev.key.key == SDLK_ESCAPE)
                         running = false;
                    else if (!ev.key.repeat)
                         handle_key(&ctx, ev.key.key, true);
                    break;
               case SDL_EVENT_KEY_UP:
                    handle_key(&ctx, ev.key.key, false);
                    break;
               case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
               case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    handle_gamepad_button(&ctx,
                                          (SDL_GamepadButton)ev.gbutton.button,
                                          ev.gbutton.down);
                    break;
               case SDL_EVENT_GAMEPAD_ADDED:
                    if (!gamepad)
                         gamepad = SDL_OpenGamepad(ev.gdevice.which);
                    break;
               case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad && SDL_GetGamepadID(gamepad) == ev.gdevice.which)
                    {
                         SDL_CloseGamepad(gamepad);
                         gamepad = NULL;
                    }
                    break;
               default:
                    break;
               }
          }

          int ww, wh;
          SDL_GetWindowSizeInPixels(window, &ww, &wh);

          struct gba_layout L;
          compute_gba_layout(&L, ww, wh);
          render_gba(&ctx, &L);
     }

     if (gamepad)
          SDL_CloseGamepad(gamepad);
     SDL_DestroyRenderer(renderer);
     SDL_DestroyWindow(window);
     SDL_Quit();
     return 0;
}
