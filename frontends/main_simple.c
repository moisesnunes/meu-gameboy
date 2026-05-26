/*
 * main_simple.c — Frontend minimalista: só a tela do jogo, sem debugger.
 *
 * Uso: gameboy-simple [--bootrom <path>] <rom.gb>
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
#include "gb.h"

/* ── Paleta DMG (verde Game Boy clássico) ── */
static const uint32_t DMG_PALETTE[4] = {
    0xFF75A32C, /* branco   */
    0xFF387A21, /* cinza claro */
    0xFF255116, /* cinza escuro */
    0xFF12280B, /* preto    */
};

/* ── Contexto do frontend simples ── */
struct simple_ctx {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *game_tex;   /* 160×144, XRGB8888 */
    uint32_t      pixels[GB_LCD_WIDTH * GB_LCD_HEIGHT];
    SDL_AudioStream *audio_stream;
    unsigned      audio_buf_index;
    size_t        audio_buf_offset;
    bool          fullscreen;
    bool          fast_forward;
};

/* ── Callbacks de vídeo ── */

static void draw_line_dmg(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
    struct simple_ctx *ctx = gb->frontend.data;
    if (ly >= GB_LCD_HEIGHT) return;
    for (unsigned i = 0; i < GB_LCD_WIDTH; i++)
        ctx->pixels[ly * GB_LCD_WIDTH + i] = DMG_PALETTE[line[i].dmg_color & 3];
}

static void draw_line_gbc(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
    struct simple_ctx *ctx = gb->frontend.data;
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
    struct simple_ctx *ctx = gb->frontend.data;

    /* Envia pixels para a textura e apresenta */
    SDL_UpdateTexture(ctx->game_tex, NULL, ctx->pixels,
                      GB_LCD_WIDTH * (int)sizeof(uint32_t));

    SDL_RenderClear(ctx->renderer);

    /* Escala inteira máxima que cabe na janela */
    int ww, wh;
    SDL_GetWindowSizeInPixels(ctx->window, &ww, &wh);
    int scale = 1;
    while ((scale + 1) * GB_LCD_WIDTH  <= ww &&
           (scale + 1) * GB_LCD_HEIGHT <= wh)
        scale++;
    int dw = GB_LCD_WIDTH  * scale;
    int dh = GB_LCD_HEIGHT * scale;
    SDL_FRect dst = {
        (float)((ww - dw) / 2),
        (float)((wh - dh) / 2),
        (float)dw,
        (float)dh,
    };
    SDL_RenderTexture(ctx->renderer, ctx->game_tex, NULL, &dst);
    SDL_RenderPresent(ctx->renderer);
}

static void refresh_input(struct gb *gb)
{
    (void)gb;
    /* O polling principal já trata SDL_PollEvent no loop principal */
}

static void destroy_frontend(struct gb *gb)
{
    struct simple_ctx *ctx = gb->frontend.data;
    if (!ctx) return;
    if (ctx->audio_stream) SDL_DestroyAudioStream(ctx->audio_stream);
    if (ctx->game_tex)     SDL_DestroyTexture(ctx->game_tex);
    if (ctx->renderer)     SDL_DestroyRenderer(ctx->renderer);
    if (ctx->window)       SDL_DestroyWindow(ctx->window);
    SDL_Quit();
    free(ctx);
    gb->frontend.data = NULL;
}

/* ── Callback de áudio ── */

static void SDLCALL audio_callback(void *userdata,
                                   SDL_AudioStream *stream,
                                   int additional_amount,
                                   int total_amount)
{
    struct gb *gb = userdata;
    struct simple_ctx *ctx = gb->frontend.data;
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

/* ── Tratamento de teclas ── */

static void handle_key(struct gb *gb, SDL_Keycode key, bool pressed)
{
    struct simple_ctx *ctx = gb->frontend.data;
    switch (key) {
    case SDLK_Q:
    case SDLK_ESCAPE:   if (pressed) gb->quit = true; break;
    case SDLK_RETURN:   gb_input_set(gb, GB_INPUT_START,  pressed); break;
    case SDLK_RSHIFT:   gb_input_set(gb, GB_INPUT_SELECT, pressed); break;
    case SDLK_LCTRL:    gb_input_set(gb, GB_INPUT_A,      pressed); break;
    case SDLK_LSHIFT:   gb_input_set(gb, GB_INPUT_B,      pressed); break;
    case SDLK_UP:       gb_input_set(gb, GB_INPUT_UP,     pressed); break;
    case SDLK_DOWN:     gb_input_set(gb, GB_INPUT_DOWN,   pressed); break;
    case SDLK_LEFT:     gb_input_set(gb, GB_INPUT_LEFT,   pressed); break;
    case SDLK_RIGHT:    gb_input_set(gb, GB_INPUT_RIGHT,  pressed); break;
    case SDLK_TAB:
        ctx->fast_forward = pressed;
        break;
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
    case SDL_GAMEPAD_BUTTON_START:      gb_input_set(gb, GB_INPUT_START,  pressed); break;
    case SDL_GAMEPAD_BUTTON_BACK:       gb_input_set(gb, GB_INPUT_SELECT, pressed); break;
    case SDL_GAMEPAD_BUTTON_EAST:       gb_input_set(gb, GB_INPUT_A,      pressed); break;
    case SDL_GAMEPAD_BUTTON_SOUTH:      gb_input_set(gb, GB_INPUT_B,      pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    gb_input_set(gb, GB_INPUT_UP,     pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  gb_input_set(gb, GB_INPUT_DOWN,   pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  gb_input_set(gb, GB_INPUT_LEFT,   pressed); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: gb_input_set(gb, GB_INPUT_RIGHT,  pressed); break;
    default: break;
    }
}

/* ── Init do frontend ── */

static bool frontend_init(struct gb *gb)
{
    struct simple_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { perror("calloc"); return false; }
    gb->frontend.data = ctx;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        free(ctx);
        return false;
    }

    ctx->window = SDL_CreateWindow(
        "Gaembuoy",
        GB_LCD_WIDTH * 4, GB_LCD_HEIGHT * 4,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!ctx->window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit(); free(ctx);
        return false;
    }

    ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
    if (!ctx->renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(ctx->window); SDL_Quit(); free(ctx);
        return false;
    }
    SDL_SetRenderVSync(ctx->renderer, 1);
    SDL_SetRenderDrawColor(ctx->renderer, 18, 40, 11, 255);

    ctx->game_tex = SDL_CreateTexture(ctx->renderer,
                                      SDL_PIXELFORMAT_XRGB8888,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      GB_LCD_WIDTH, GB_LCD_HEIGHT);
    if (!ctx->game_tex) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(ctx->renderer);
        SDL_DestroyWindow(ctx->window);
        SDL_Quit(); free(ctx);
        return false;
    }
    SDL_SetTextureScaleMode(ctx->game_tex, SDL_SCALEMODE_NEAREST);

    /* Áudio */
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

    return true;
}

/* ── Carrega boot ROM ── */

#if 0
static bool load_bootrom(struct gb *gb, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);

    if (size != 0x100 && size != 0x900) {
        fprintf(stderr, "Boot ROM inválida: tamanho %ld\n", size);
        fclose(f); return false;
    }

    uint8_t *buf = malloc((size_t)size);
    if (!buf) { perror("malloc"); fclose(f); return false; }

    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "Erro ao ler boot ROM\n");
        free(buf); fclose(f); return false;
    }
    fclose(f);

    free(gb->bootrom);
    gb->bootrom      = buf;
    gb->bootrom_size = (uint32_t)size;
    printf("Boot ROM carregada: %s (%ld bytes)\n", path, size);
    return true;
}

/*
 * Tenta carregar a boot ROM automaticamente.
 * Procura por dmg_boot.bin / cgb_boot.bin na pasta bootroms/ ao lado do executável,
 * escolhendo a CGB se o cartucho for GBC, ou a DMG caso contrário.
 */
static void auto_load_bootrom(struct gb *gb, const char *exe_path)
{
    /* Descobre a pasta do executável */
    char dir[4096];
    const char *slash = strrchr(exe_path, '/');
    if (slash) {
        size_t len = (size_t)(slash - exe_path);
        if (len >= sizeof(dir)) len = sizeof(dir) - 1;
        memcpy(dir, exe_path, len);
        dir[len] = '\0';
    } else {
        dir[0] = '.'; dir[1] = '\0';
    }

    const char *name = gb->gbc ? "cgb_boot.bin" : "dmg_boot.bin";
    char path[4096];
    snprintf(path, sizeof(path), "%s/bootroms/%s", dir, name);

    if (!load_bootrom(gb, path)) {
        /* Tenta na pasta atual como fallback */
        snprintf(path, sizeof(path), "bootroms/%s", name);
        load_bootrom(gb, path);
    }
}
#endif

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

    memset(gb->iram, 0, sizeof(gb->iram));
    memset(gb->zram, 0, sizeof(gb->zram));
    memset(gb->vram, 0, sizeof(gb->vram));
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
    /* Reset frontend audio ring-buffer position to match the SPU reset above */
    {
        struct simple_ctx *ctx = gb->frontend.data;
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
    const char *rom_file = NULL;
    /* const char *bootrom_file = NULL; */

    for (int i = 1; i < argc; i++) {
        /* if (strcmp(argv[i], "--bootrom") == 0 && i + 1 < argc)
            bootrom_file = argv[++i];
        else */
        if (!rom_file)
            rom_file = argv[i];
        else {
            fprintf(stderr, "Uso: %s <rom.gb>\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!rom_file) {
        fprintf(stderr, "Uso: %s <rom.gb>\n", argv[0]);
        return EXIT_FAILURE;
    }

    struct gb *gb = calloc(1, sizeof(*gb));
    if (!gb) { perror("calloc"); return EXIT_FAILURE; }

    if (!frontend_init(gb)) {
        free(gb);
        return EXIT_FAILURE;
    }

    /* Carrega a ROM primeiro para saber se é DMG ou GBC */
    load_rom(gb, rom_file);

    /* Boot ROM: usa a fornecida via --bootrom, ou busca automaticamente */
    /* if (bootrom_file)
        load_bootrom(gb, bootrom_file);
    else
        auto_load_bootrom(gb, argv[0]); */

    /* Re-reseta a CPU para aplicar a boot ROM (load_rom já resetou sem ela) */
    /* if (gb->bootrom)
        gb_cpu_reset(gb); */

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
                /* Drag-and-drop de nova ROM */
                load_rom(gb, e.drop.data);
                last_ns = SDL_GetTicksNS();
                SDL_free((void *)e.drop.data);
                break;
            }
        }

        struct simple_ctx *ctx = gb->frontend.data;
        float speed = ctx->fast_forward ? 2.0f : 1.0f;

        uint64_t now_ns  = SDL_GetTicksNS();
        uint64_t elapsed = now_ns - last_ns;
        last_ns = now_ns;

        /* Limita a 50 ms para evitar spiral-of-death */
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
