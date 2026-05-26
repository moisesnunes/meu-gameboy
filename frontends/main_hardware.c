/*
 * main_hardware.c — Frontend "hardware virtual": exibe a imagem real do Game Boy
 * com a tela do jogo sobreposta no LCD e highlight nos botões quando pressionados.
 * As coordenadas são lidas de um arquivo skin.ini — veja skins/default/skin.ini.
 *
 * Uso: gameboy-hardware <rom.gb>
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
#include <semaphore.h>
#include <SDL3/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "gb.h"

/* ── Paleta DMG ── */
static const uint32_t DMG_PALETTE[4] = {
    0xFF75A32C,
    0xFF387A21,
    0xFF255116,
    0xFF12280B,
};

/* ── Skin ── */

struct skin_button {
    float x, y, w, h;  /* bounding box na imagem original */
    float cx, cy;       /* centro (usado para botões circulares durante o parse) */
    float r;
};

struct hw_skin {
    char  image[512];   /* path relativo à pasta da skin */
    float img_w, img_h;

    float lcd_x, lcd_y, lcd_w, lcd_h;

    float dpad_x, dpad_y, dpad_w, dpad_h;

    struct skin_button btn_a;
    struct skin_button btn_b;
    struct skin_button btn_start;
    struct skin_button btn_select;
};

/* Parser INI minimalista: chave=valor, seções [nome], comentários com ; ou # */
static void ini_parse(const char *path, struct hw_skin *skin)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "skin: não abriu %s\n", path); return; }

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        /* strip newline */
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';

        /* strip leading whitespace */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;

        if (*p == '\0' || *p == ';' || *p == '#') continue;

        if (*p == '[') {
            char *end = strchr(p + 1, ']');
            if (end) {
                size_t len = (size_t)(end - p - 1);
                if (len >= sizeof(section)) len = sizeof(section) - 1;
                memcpy(section, p + 1, len);
                section[len] = '\0';
            }
            continue;
        }

        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        /* strip trailing whitespace from key */
        char *ke = key + strlen(key) - 1;
        while (ke >= key && (*ke == ' ' || *ke == '\t')) *ke-- = '\0';

        /* strip leading whitespace from val */
        while (*val == ' ' || *val == '\t') val++;

#define MATCH(s, k) (strcmp(section, (s)) == 0 && strcmp(key, (k)) == 0)
#define F(field)    (float)atof(val)

        if      (MATCH("image",        "file"))   strncpy(skin->image, val, sizeof(skin->image) - 1);
        else if (MATCH("image",        "width"))  skin->img_w = F();
        else if (MATCH("image",        "height")) skin->img_h = F();
        else if (MATCH("lcd",          "x"))      skin->lcd_x = F();
        else if (MATCH("lcd",          "y"))      skin->lcd_y = F();
        else if (MATCH("lcd",          "w"))      skin->lcd_w = F();
        else if (MATCH("lcd",          "h"))      skin->lcd_h = F();
        else if (MATCH("dpad",         "x"))      skin->dpad_x = F();
        else if (MATCH("dpad",         "y"))      skin->dpad_y = F();
        else if (MATCH("dpad",         "w"))      skin->dpad_w = F();
        else if (MATCH("dpad",         "h"))      skin->dpad_h = F();
        else if (MATCH("button.a",     "cx"))     skin->btn_a.cx = F();
        else if (MATCH("button.a",     "cy"))     skin->btn_a.cy = F();
        else if (MATCH("button.a",     "r"))      skin->btn_a.r  = F();
        else if (MATCH("button.b",     "cx"))     skin->btn_b.cx = F();
        else if (MATCH("button.b",     "cy"))     skin->btn_b.cy = F();
        else if (MATCH("button.b",     "r"))      skin->btn_b.r  = F();
        else if (MATCH("button.start", "x"))      skin->btn_start.x = F();
        else if (MATCH("button.start", "y"))      skin->btn_start.y = F();
        else if (MATCH("button.start", "w"))      skin->btn_start.w = F();
        else if (MATCH("button.start", "h"))      skin->btn_start.h = F();
        else if (MATCH("button.select","x"))      skin->btn_select.x = F();
        else if (MATCH("button.select","y"))      skin->btn_select.y = F();
        else if (MATCH("button.select","w"))      skin->btn_select.w = F();
        else if (MATCH("button.select","h"))      skin->btn_select.h = F();

#undef MATCH
#undef F
    }

    fclose(f);
}

/* Converte cx/cy/r para bounding box x/y/w/h */
static void skin_fixup_circles(struct hw_skin *skin)
{
    skin->btn_a.w = skin->btn_a.r * 2;
    skin->btn_a.h = skin->btn_a.r * 2;
    skin->btn_a.x = skin->btn_a.cx - skin->btn_a.r;
    skin->btn_a.y = skin->btn_a.cy - skin->btn_a.r;

    skin->btn_b.w = skin->btn_b.r * 2;
    skin->btn_b.h = skin->btn_b.r * 2;
    skin->btn_b.x = skin->btn_b.cx - skin->btn_b.r;
    skin->btn_b.y = skin->btn_b.cy - skin->btn_b.r;
}

/* ── Índices dos botões ── */

enum {
    HW_BTN_A = 0,
    HW_BTN_B,
    HW_BTN_START,
    HW_BTN_SELECT,
    HW_BTN_UP,
    HW_BTN_DOWN,
    HW_BTN_LEFT,
    HW_BTN_RIGHT,
    HW_BTN_COUNT
};

struct hw_button {
    SDL_FRect region;
    bool      pressed;
};

/* ── Contexto ── */

struct hw_ctx {
    SDL_Window   *window;
    SDL_Renderer *renderer;

    SDL_Texture  *hw_tex;
    SDL_Texture  *game_tex;
    SDL_Texture  *overlay_tex;

    uint32_t      pixels[GB_LCD_WIDTH * GB_LCD_HEIGHT];

    SDL_AudioStream *audio_stream;
    unsigned      audio_buf_index;
    size_t        audio_buf_offset;

    struct hw_button buttons[HW_BTN_COUNT];
    struct hw_skin   skin;

    float  scale;
    float  off_x, off_y;

    bool   fullscreen;
    bool   fast_forward;
};

/* ── Layout ── */

static SDL_FRect scale_rect(struct hw_ctx *ctx, float x, float y, float w, float h)
{
    return (SDL_FRect){
        ctx->off_x + x * ctx->scale,
        ctx->off_y + y * ctx->scale,
        w * ctx->scale,
        h * ctx->scale,
    };
}

static void recompute_layout(struct hw_ctx *ctx)
{
    struct hw_skin *s = &ctx->skin;
    int ww, wh;
    SDL_GetWindowSizeInPixels(ctx->window, &ww, &wh);

    float sx = (float)ww / s->img_w;
    float sy = (float)wh / s->img_h;
    ctx->scale = sx < sy ? sx : sy;
    ctx->off_x = ((float)ww - s->img_w * ctx->scale) / 2.0f;
    ctx->off_y = ((float)wh - s->img_h * ctx->scale) / 2.0f;

    /* D-Pad: divide a bbox em 9 células, usa as 4 laterais */
    float dw = s->dpad_w * ctx->scale / 3.0f;
    float dh = s->dpad_h * ctx->scale / 3.0f;
    float dx = ctx->off_x + s->dpad_x * ctx->scale;
    float dy = ctx->off_y + s->dpad_y * ctx->scale;

    ctx->buttons[HW_BTN_UP]    .region = (SDL_FRect){ dx + dw,   dy,        dw, dh };
    ctx->buttons[HW_BTN_DOWN]  .region = (SDL_FRect){ dx + dw,   dy + 2*dh, dw, dh };
    ctx->buttons[HW_BTN_LEFT]  .region = (SDL_FRect){ dx,        dy + dh,   dw, dh };
    ctx->buttons[HW_BTN_RIGHT] .region = (SDL_FRect){ dx + 2*dw, dy + dh,   dw, dh };

    ctx->buttons[HW_BTN_A]     .region = scale_rect(ctx, s->btn_a.x,      s->btn_a.y,      s->btn_a.w,      s->btn_a.h);
    ctx->buttons[HW_BTN_B]     .region = scale_rect(ctx, s->btn_b.x,      s->btn_b.y,      s->btn_b.w,      s->btn_b.h);
    ctx->buttons[HW_BTN_START] .region = scale_rect(ctx, s->btn_start.x,  s->btn_start.y,  s->btn_start.w,  s->btn_start.h);
    ctx->buttons[HW_BTN_SELECT].region = scale_rect(ctx, s->btn_select.x, s->btn_select.y, s->btn_select.w, s->btn_select.h);
}

/* ── Callbacks de vídeo ── */

static void draw_line_dmg(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
    struct hw_ctx *ctx = gb->frontend.data;
    if (ly >= GB_LCD_HEIGHT) return;
    for (unsigned i = 0; i < GB_LCD_WIDTH; i++)
        ctx->pixels[ly * GB_LCD_WIDTH + i] = DMG_PALETTE[line[i].dmg_color & 3];
}

static void draw_line_gbc(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
    struct hw_ctx *ctx = gb->frontend.data;
    if (ly >= GB_LCD_HEIGHT) return;
    for (unsigned i = 0; i < GB_LCD_WIDTH; i++) {
        uint16_t c = line[i].gbc_color;
        uint32_t r = ((c & 0x1f) << 3) | ((c & 0x1f) >> 2);
        uint32_t g = (((c >> 5) & 0x1f) << 3) | (((c >> 5) & 0x1f) >> 2);
        uint32_t b = (((c >> 10) & 0x1f) << 3) | (((c >> 10) & 0x1f) >> 2);
        ctx->pixels[ly * GB_LCD_WIDTH + i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}

static void flip(struct gb *gb)
{
    struct hw_ctx *ctx = gb->frontend.data;
    struct hw_skin *s  = &ctx->skin;

    recompute_layout(ctx);

    SDL_UpdateTexture(ctx->game_tex, NULL, ctx->pixels,
                      GB_LCD_WIDTH * (int)sizeof(uint32_t));

    SDL_RenderClear(ctx->renderer);

    /* 1. Hardware background */
    SDL_FRect hw_dst = {
        ctx->off_x, ctx->off_y,
        s->img_w * ctx->scale, s->img_h * ctx->scale
    };
    SDL_RenderTexture(ctx->renderer, ctx->hw_tex, NULL, &hw_dst);

    /* 2. Tela do jogo */
    SDL_FRect lcd_dst = scale_rect(ctx, s->lcd_x, s->lcd_y, s->lcd_w, s->lcd_h);
    SDL_RenderTexture(ctx->renderer, ctx->game_tex, NULL, &lcd_dst);

    /* 3. Highlight dos botões pressionados */
    for (int i = 0; i < HW_BTN_COUNT; i++) {
        if (!ctx->buttons[i].pressed) continue;
        SDL_SetTextureAlphaMod(ctx->overlay_tex, 100);
        SDL_SetTextureColorMod(ctx->overlay_tex, 255, 255, 255);
        SDL_RenderTexture(ctx->renderer, ctx->overlay_tex,
                          NULL, &ctx->buttons[i].region);
    }

    SDL_RenderPresent(ctx->renderer);
}

static void refresh_input(struct gb *gb) { (void)gb; }

static void destroy_frontend(struct gb *gb)
{
    struct hw_ctx *ctx = gb->frontend.data;
    if (!ctx) return;
    if (ctx->audio_stream) SDL_DestroyAudioStream(ctx->audio_stream);
    if (ctx->overlay_tex)  SDL_DestroyTexture(ctx->overlay_tex);
    if (ctx->game_tex)     SDL_DestroyTexture(ctx->game_tex);
    if (ctx->hw_tex)       SDL_DestroyTexture(ctx->hw_tex);
    if (ctx->renderer)     SDL_DestroyRenderer(ctx->renderer);
    if (ctx->window)       SDL_DestroyWindow(ctx->window);
    SDL_Quit();
    free(ctx);
    gb->frontend.data = NULL;
}

/* ── Áudio ── */

static void SDLCALL audio_callback(void *userdata,
                                   SDL_AudioStream *stream,
                                   int additional_amount,
                                   int total_amount)
{
    struct gb *gb = userdata;
    struct hw_ctx *ctx = gb->frontend.data;
    (void)total_amount;

    int needed = additional_amount;
    while (needed > 0) {
        struct gb_spu_sample_buffer *buf =
            &gb->spu.buffers[ctx->audio_buf_index];

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

/* ── Teclas / gamepad ── */

static void set_button(struct gb *gb, int btn_idx, unsigned input, bool pressed)
{
    struct hw_ctx *ctx = gb->frontend.data;
    ctx->buttons[btn_idx].pressed = pressed;
    gb_input_set(gb, input, pressed);
}

static void handle_key(struct gb *gb, SDL_Keycode key, bool pressed)
{
    struct hw_ctx *ctx = gb->frontend.data;
    switch (key) {
    case SDLK_Q:
    case SDLK_ESCAPE:  if (pressed) gb->quit = true; break;
    case SDLK_RETURN:  set_button(gb, HW_BTN_START,  GB_INPUT_START,  pressed); break;
    case SDLK_RSHIFT:  set_button(gb, HW_BTN_SELECT, GB_INPUT_SELECT, pressed); break;
    case SDLK_LCTRL:   set_button(gb, HW_BTN_A,      GB_INPUT_A,      pressed); break;
    case SDLK_LSHIFT:  set_button(gb, HW_BTN_B,      GB_INPUT_B,      pressed); break;
    case SDLK_UP:      set_button(gb, HW_BTN_UP,     GB_INPUT_UP,     pressed); break;
    case SDLK_DOWN:    set_button(gb, HW_BTN_DOWN,   GB_INPUT_DOWN,   pressed); break;
    case SDLK_LEFT:    set_button(gb, HW_BTN_LEFT,   GB_INPUT_LEFT,   pressed); break;
    case SDLK_RIGHT:   set_button(gb, HW_BTN_RIGHT,  GB_INPUT_RIGHT,  pressed); break;
    case SDLK_TAB:     ctx->fast_forward = pressed; break;
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
    case SDL_GAMEPAD_BUTTON_START:      set_button(gb, HW_BTN_START,  GB_INPUT_START,  pressed); break;
    case SDL_GAMEPAD_BUTTON_BACK:       set_button(gb, HW_BTN_SELECT, GB_INPUT_SELECT, pressed); break;
    case SDL_GAMEPAD_BUTTON_EAST:       set_button(gb, HW_BTN_A,      GB_INPUT_A,      pressed); break;
    case SDL_GAMEPAD_BUTTON_SOUTH:      set_button(gb, HW_BTN_B,      GB_INPUT_B,      pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    set_button(gb, HW_BTN_UP,     GB_INPUT_UP,     pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  set_button(gb, HW_BTN_DOWN,   GB_INPUT_DOWN,   pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  set_button(gb, HW_BTN_LEFT,   GB_INPUT_LEFT,   pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: set_button(gb, HW_BTN_RIGHT,  GB_INPUT_RIGHT,  pressed); break;
    default: break;
    }
}

/* ── Carrega imagem como textura ── */

static SDL_Texture *load_image_as_texture(SDL_Renderer *renderer, const char *path)
{
    int w, h, ch;
    unsigned char *data = stbi_load(path, &w, &h, &ch, 4);
    if (!data) {
        fprintf(stderr, "stbi_load(%s): %s\n", path, stbi_failure_reason());
        return NULL;
    }

    SDL_Surface *surf = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA8888, data, w * 4);
    SDL_Texture *tex  = surf ? SDL_CreateTextureFromSurface(renderer, surf) : NULL;
    if (surf) SDL_DestroySurface(surf);
    stbi_image_free(data);

    if (!tex) fprintf(stderr, "SDL_CreateTextureFromSurface: %s\n", SDL_GetError());
    return tex;
}

/* ── Resolve caminho relativo ao diretório base ── */

static void resolve_path(const char *base_dir, const char *rel, char *out, size_t out_sz)
{
    snprintf(out, out_sz, "%s/%s", base_dir, rel);
}

/* Extrai o diretório de um path de executável */
static void exe_dir(const char *exe, char *out, size_t out_sz)
{
    const char *slash = strrchr(exe, '/');
    if (slash) {
        size_t len = (size_t)(slash - exe);
        if (len >= out_sz) len = out_sz - 1;
        memcpy(out, exe, len);
        out[len] = '\0';
    } else {
        out[0] = '.'; out[1] = '\0';
    }
}

/* ── Init ── */

static bool frontend_init(struct gb *gb, const char *exe_path, const char *skin_dir)
{
    struct hw_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { perror("calloc"); return false; }
    gb->frontend.data = ctx;

    /* Carrega skin.ini */
    char ini_path[4096];
    resolve_path(skin_dir, "skin.ini", ini_path, sizeof(ini_path));
    ini_parse(ini_path, &ctx->skin);
    skin_fixup_circles(&ctx->skin);

    if (ctx->skin.img_w <= 0 || ctx->skin.img_h <= 0) {
        fprintf(stderr, "skin inválida ou não encontrada em %s\n", ini_path);
        free(ctx); return false;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        free(ctx); return false;
    }

    int win_h = 720;
    int win_w = (int)((float)win_h * ctx->skin.img_w / ctx->skin.img_h);

    ctx->window = SDL_CreateWindow("meu-gameboy — Hardware",
                                   win_w, win_h,
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
    SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);

    /* Imagem do hardware: path relativo à pasta da skin */
    char img_path[4096];
    resolve_path(skin_dir, ctx->skin.image, img_path, sizeof(img_path));
    ctx->hw_tex = load_image_as_texture(ctx->renderer, img_path);
    if (!ctx->hw_tex) {
        fprintf(stderr, "Não foi possível carregar a imagem da skin: %s\n", img_path);
        SDL_DestroyRenderer(ctx->renderer);
        SDL_DestroyWindow(ctx->window);
        SDL_Quit(); free(ctx); return false;
    }
    SDL_SetTextureScaleMode(ctx->hw_tex, SDL_SCALEMODE_LINEAR);

    ctx->game_tex = SDL_CreateTexture(ctx->renderer,
                                      SDL_PIXELFORMAT_XRGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      GB_LCD_WIDTH, GB_LCD_HEIGHT);
    if (!ctx->game_tex) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyTexture(ctx->hw_tex);
        SDL_DestroyRenderer(ctx->renderer);
        SDL_DestroyWindow(ctx->window);
        SDL_Quit(); free(ctx); return false;
    }
    SDL_SetTextureScaleMode(ctx->game_tex, SDL_SCALEMODE_NEAREST);

    ctx->overlay_tex = SDL_CreateTexture(ctx->renderer,
                                         SDL_PIXELFORMAT_RGBA8888,
                                         SDL_TEXTUREACCESS_STATIC, 1, 1);
    if (ctx->overlay_tex) {
        uint32_t white = 0xFFFFFFFF;
        SDL_UpdateTexture(ctx->overlay_tex, NULL, &white, 4);
        SDL_SetTextureBlendMode(ctx->overlay_tex, SDL_BLENDMODE_BLEND);
    }

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
    gb->frontend.flip          = flip;
    gb->frontend.refresh_input = refresh_input;
    gb->frontend.destroy       = destroy_frontend;

    (void)exe_path;
    return true;
}

/* ── Reset / carrega ROM ── */

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
        struct hw_ctx *ctx = gb->frontend.data;
        if (ctx) {
            ctx->audio_buf_index  = 0;
            ctx->audio_buf_offset = 0;
        }
    }
    gb_debug_init(gb);
    gb->debug.enabled = false;
    gb->debug.state   = GB_DEBUG_RUNNING;
}

/* ── main ── */

int main(int argc, char **argv)
{
    const char *rom_file  = NULL;
    const char *skin_dir  = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--skin") == 0 && i + 1 < argc)
            skin_dir = argv[++i];
        else if (!rom_file)
            rom_file = argv[i];
        else {
            fprintf(stderr, "Uso: %s [--skin <dir>] <rom.gb>\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!rom_file) {
        fprintf(stderr, "Uso: %s [--skin <dir>] <rom.gb>\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Skin padrão: skins/default/ ao lado do executável */
    char default_skin[4096];
    if (!skin_dir) {
        char dir[4096];
        exe_dir(argv[0], dir, sizeof(dir));
        snprintf(default_skin, sizeof(default_skin), "%s/skins/default", dir);
        /* fallback para path relativo ao cwd */
        if (fopen(default_skin, "r") == NULL)
            snprintf(default_skin, sizeof(default_skin), "skins/default");
        skin_dir = default_skin;
    }

    struct gb *gb = calloc(1, sizeof(*gb));
    if (!gb) { perror("calloc"); return EXIT_FAILURE; }

    if (!frontend_init(gb, argv[0], skin_dir)) {
        free(gb);
        return EXIT_FAILURE;
    }

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

        struct hw_ctx *ctx = gb->frontend.data;
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
