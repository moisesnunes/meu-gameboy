/*
 * gba_main.c — Frontend SDL3 para o GBA.
 *
 * Uso: gba [--bios <path>] <rom.gba>
 *
 * Controles:
 *   Setas              D-Pad
 *   Z / LCtrl          A
 *   X / LShift         B
 *   A                  L
 *   S                  R
 *   Enter              Start
 *   RShift / Backspace Select
 *   Tab (segurar)      Fast-forward 2x
 *   F11                Fullscreen
 *   Q / Escape         Sair
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <semaphore.h>

#include <SDL3/SDL.h>

#include "gba/gba.h"
#include "ui/gba_debug_ui.h"

/* ── Contexto do frontend ── */

struct gba_ctx
{
     SDL_Window *window;
     SDL_Renderer *renderer;
     SDL_Texture *game_tex; /* 240×160, XRGB8888 */
     uint32_t pixels[GBA_LCD_W * GBA_LCD_H];
     SDL_AudioStream *audio_stream;
     int buf_read;      /* index into apu.buf double-buffer */
     size_t buf_offset; /* byte offset within current half-buffer */
     bool fullscreen;
     bool fast_forward;
     bool debug_ui;
     struct gba *gba;
};

/* ── Callbacks de vídeo ── */

/* Convert RGB555 → XRGB8888 */
static inline uint32_t rgb555_to_xrgb(uint16_t c)
{
     uint32_t r = ((c & 0x001f) << 3) | ((c & 0x001f) >> 2);
     uint32_t g = (((c >> 5) & 0x1f) << 3) | (((c >> 5) & 0x1f) >> 2);
     uint32_t b = (((c >> 10) & 0x1f) << 3) | (((c >> 10) & 0x1f) >> 2);
     return 0xFF000000 | (r << 16) | (g << 8) | b;
}

static void draw_line(void *data, uint8_t line, const uint16_t *pixels)
{
     struct gba_ctx *ctx = data;
     if (line >= GBA_LCD_H)
          return;
     uint32_t *row = ctx->pixels + (unsigned)line * GBA_LCD_W;
     for (unsigned i = 0; i < GBA_LCD_W; i++)
          row[i] = rgb555_to_xrgb(pixels[i]);
}

static void flip(void *data)
{
     struct gba_ctx *ctx = data;

     if (ctx->debug_ui)
          return;

     if (!SDL_UpdateTexture(ctx->game_tex, NULL, ctx->pixels,
                            GBA_LCD_W * (int)sizeof(uint32_t)))
          fprintf(stderr, "[SDL] UpdateTexture failed: %s\n", SDL_GetError());

     SDL_RenderClear(ctx->renderer);

     int ww, wh;
     SDL_GetWindowSizeInPixels(ctx->window, &ww, &wh);
     int scale = 1;
     while ((scale + 1) * GBA_LCD_W <= ww &&
            (scale + 1) * GBA_LCD_H <= wh)
          scale++;
     int dw = GBA_LCD_W * scale;
     int dh = GBA_LCD_H * scale;
     SDL_FRect dst = {
         (float)((ww - dw) / 2),
         (float)((wh - dh) / 2),
         (float)dw,
         (float)dh,
     };
     if (!SDL_RenderTexture(ctx->renderer, ctx->game_tex, NULL, &dst))
          fprintf(stderr, "[SDL] RenderTexture failed: %s\n", SDL_GetError());
     if (!SDL_RenderPresent(ctx->renderer))
          fprintf(stderr, "[SDL] RenderPresent failed: %s\n", SDL_GetError());
}

static void refresh_input(void *data)
{
     (void)data;
}

static void destroy_frontend(void *data)
{
     struct gba_ctx *ctx = data;
     if (!ctx)
          return;
     if (ctx->debug_ui)
          gba_debug_ui_destroy();
     if (ctx->audio_stream)
          SDL_DestroyAudioStream(ctx->audio_stream);
     if (ctx->game_tex)
          SDL_DestroyTexture(ctx->game_tex);
     if (ctx->renderer)
          SDL_DestroyRenderer(ctx->renderer);
     if (ctx->window)
          SDL_DestroyWindow(ctx->window);
     SDL_Quit();
     free(ctx);
}

/* ── Callback de áudio ── */

static void SDLCALL audio_callback(void *userdata,
                                   SDL_AudioStream *stream,
                                   int additional_amount,
                                   int total_amount)
{
     struct gba_ctx *ctx = userdata;
     struct gba_apu *apu = &ctx->gba->apu;
     (void)total_amount;

     int needed = additional_amount;
     while (needed > 0)
     {
          int idx = ctx->buf_read;

          if (ctx->buf_offset == 0 && sem_trywait(&apu->buf_ready) != 0)
          {
               /* No sample ready — output silence */
               static const int16_t silence[GBA_APU_BUF_SAMPLES * 2] = {0};
               int chunk = needed < (int)sizeof(silence) ? needed : (int)sizeof(silence);
               SDL_PutAudioStreamData(stream, silence, chunk);
               needed -= chunk;
               continue;
          }

          const uint8_t *src = (const uint8_t *)apu->buf[idx];
          size_t total_bytes = GBA_APU_BUF_SAMPLES * 2 * sizeof(int16_t);
          size_t rem = total_bytes - ctx->buf_offset;
          int chunk = (int)rem < needed ? (int)rem : needed;

          SDL_PutAudioStreamData(stream, src + ctx->buf_offset, chunk);
          ctx->buf_offset += (size_t)chunk;
          needed -= chunk;

          if (ctx->buf_offset >= total_bytes)
          {
               ctx->buf_offset = 0;
               ctx->buf_read = (ctx->buf_read + 1) & 1;
               sem_post(&apu->buf_free);
          }
     }
}

/* ── Tratamento de teclas ── */

static void handle_key(struct gba *gba, SDL_Keycode key, bool pressed)
{
     struct gba_ctx *ctx = gba->frontend.data;
     switch (key)
     {
     case SDLK_Q:
     case SDLK_ESCAPE:
          if (pressed)
               gba->quit = true;
          break;
     case SDLK_Z:
     case SDLK_LCTRL:
          gba_input_set(gba, GBA_KEY_A, pressed);
          break;
     case SDLK_X:
     case SDLK_LSHIFT:
          gba_input_set(gba, GBA_KEY_B, pressed);
          break;
     case SDLK_A:
          gba_input_set(gba, GBA_KEY_L, pressed);
          break;
     case SDLK_S:
          gba_input_set(gba, GBA_KEY_R, pressed);
          break;
     case SDLK_RETURN:
          gba_input_set(gba, GBA_KEY_START, pressed);
          break;
     case SDLK_RSHIFT:
     case SDLK_BACKSPACE:
          gba_input_set(gba, GBA_KEY_SELECT, pressed);
          break;
     case SDLK_UP:
          gba_input_set(gba, GBA_KEY_UP, pressed);
          break;
     case SDLK_DOWN:
          gba_input_set(gba, GBA_KEY_DOWN, pressed);
          break;
     case SDLK_LEFT:
          gba_input_set(gba, GBA_KEY_LEFT, pressed);
          break;
     case SDLK_RIGHT:
          gba_input_set(gba, GBA_KEY_RIGHT, pressed);
          break;
     case SDLK_TAB:
          ctx->fast_forward = pressed;
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

static void handle_gamepad_button(struct gba *gba, SDL_GamepadButton btn, bool pressed)
{
     switch (btn)
     {
     case SDL_GAMEPAD_BUTTON_EAST:
          gba_input_set(gba, GBA_KEY_A, pressed);
          break;
     case SDL_GAMEPAD_BUTTON_SOUTH:
          gba_input_set(gba, GBA_KEY_B, pressed);
          break;
     case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
          gba_input_set(gba, GBA_KEY_L, pressed);
          break;
     case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
          gba_input_set(gba, GBA_KEY_R, pressed);
          break;
     case SDL_GAMEPAD_BUTTON_START:
          gba_input_set(gba, GBA_KEY_START, pressed);
          break;
     case SDL_GAMEPAD_BUTTON_BACK:
          gba_input_set(gba, GBA_KEY_SELECT, pressed);
          break;
     case SDL_GAMEPAD_BUTTON_DPAD_UP:
          gba_input_set(gba, GBA_KEY_UP, pressed);
          break;
     case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
          gba_input_set(gba, GBA_KEY_DOWN, pressed);
          break;
     case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
          gba_input_set(gba, GBA_KEY_LEFT, pressed);
          break;
     case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
          gba_input_set(gba, GBA_KEY_RIGHT, pressed);
          break;
     default:
          break;
     }
}

/* ── Init do frontend ── */

static bool frontend_init(struct gba *gba, bool debug_ui)
{
     struct gba_ctx *ctx = calloc(1, sizeof(*ctx));
     if (!ctx)
     {
          perror("calloc");
          return false;
     }
     ctx->gba = gba;
     ctx->debug_ui = debug_ui;
     gba->frontend.data = ctx;

     if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
     {
          fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
          free(ctx);
          return false;
     }

     if (debug_ui)
     {
          SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
          SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
          SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
          SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
          SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
     }

     ctx->window = SDL_CreateWindow(
         "meu-GBA",
         GBA_LCD_W * 4, GBA_LCD_H * 4,
         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
             (debug_ui ? SDL_WINDOW_OPENGL : 0));
     if (!ctx->window)
     {
          fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
          SDL_Quit();
          free(ctx);
          return false;
     }

     if (debug_ui)
     {
          if (!gba_debug_ui_init(gba, ctx->window))
          {
               SDL_DestroyWindow(ctx->window);
               SDL_Quit();
               free(ctx);
               return false;
          }
     }
     else
     {
          ctx->renderer = SDL_CreateRenderer(ctx->window, NULL);
          if (!ctx->renderer)
          {
               fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
               SDL_DestroyWindow(ctx->window);
               SDL_Quit();
               free(ctx);
               return false;
          }
          SDL_SetRenderVSync(ctx->renderer, 1);
          SDL_SetRenderDrawColor(ctx->renderer, 0, 0, 0, 255);

          ctx->game_tex = SDL_CreateTexture(ctx->renderer,
                                            SDL_PIXELFORMAT_XRGB8888,
                                            SDL_TEXTUREACCESS_STREAMING,
                                            GBA_LCD_W, GBA_LCD_H);
          if (!ctx->game_tex)
          {
               fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
               SDL_DestroyRenderer(ctx->renderer);
               SDL_DestroyWindow(ctx->window);
               SDL_Quit();
               free(ctx);
               return false;
          }
          SDL_SetTextureScaleMode(ctx->game_tex, SDL_SCALEMODE_NEAREST);
     }

     SDL_AudioSpec spec = {
         .format = SDL_AUDIO_S16,
         .channels = 2,
         .freq = GBA_APU_SAMPLE_RATE,
     };
     ctx->audio_stream = SDL_OpenAudioDeviceStream(
         SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audio_callback, ctx);
     if (ctx->audio_stream)
          SDL_ResumeAudioStreamDevice(ctx->audio_stream);
     else
          fprintf(stderr, "Aviso: sem áudio — %s\n", SDL_GetError());

     gba->frontend.draw_line = draw_line;
     gba->frontend.flip = flip;
     gba->frontend.refresh_input = refresh_input;
     gba->frontend.destroy = destroy_frontend;

     return true;
}

/* ── main ── */

int main(int argc, char **argv)
{
     const char *rom_file = NULL;
     const char *bios_file = NULL;
     const char *trace_file = NULL;
     bool debug_ui = false;
     uint64_t trace_limit = 0;

     for (int i = 1; i < argc; i++)
     {
          if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc)
               bios_file = argv[++i];
          else if (strcmp(argv[i], "--debug") == 0)
               debug_ui = true;
          else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc)
          {
               /* --trace [N] [file]  — N = max instructions (0=unlimited), file = output path */
               char *end;
               unsigned long long n = strtoull(argv[i + 1], &end, 10);
               if (*end == '\0')
               {
                    trace_limit = (uint64_t)n;
                    i++;
                    if (i + 1 < argc && argv[i + 1][0] != '-')
                         trace_file = argv[++i];
               }
               else
               {
                    trace_file = argv[++i];
               }
          }
          else if (!rom_file)
               rom_file = argv[i];
          else
          {
               fprintf(stderr, "Uso: %s [--debug] [--bios <gba_bios.bin>] [--trace [N] [file]] <rom.gba>\n", argv[0]);
               return EXIT_FAILURE;
          }
     }

     if (!rom_file)
     {
          fprintf(stderr, "Uso: %s [--debug] [--bios <gba_bios.bin>] <rom.gba>\n", argv[0]);
          return EXIT_FAILURE;
     }

     struct gba *gba = gba_create();
     if (!gba)
     {
          perror("gba_create");
          return EXIT_FAILURE;
     }

     if (!frontend_init(gba, debug_ui))
     {
          gba_destroy(gba);
          return EXIT_FAILURE;
     }

     if (bios_file)
     {
          if (!gba_load_bios(gba, bios_file))
          {
               fprintf(stderr, "Aviso: falha ao carregar BIOS, usando HLE\n");
          }
     }

     if (!gba_cart_load(gba, rom_file))
     {
          fprintf(stderr, "Erro: não foi possível carregar '%s'\n", rom_file);
          gba->frontend.destroy(gba->frontend.data);
          gba_destroy(gba);
          return EXIT_FAILURE;
     }

     gba_reset(gba);
     gba->quit = false;

     /* Enable CPU execution tracer if requested */
     if (trace_limit || trace_file)
     {
          gba->debug.trace_enabled = true;
          gba->debug.trace_limit = trace_limit;
          if (trace_file)
          {
               gba->debug.trace_fp = fopen(trace_file, "w");
               if (!gba->debug.trace_fp)
               {
                    perror(trace_file);
                    gba->debug.trace_fp = stderr;
               }
               else
                    fprintf(stderr, "[trace] Escrevendo em '%s'\n", trace_file);
          }
          if (!trace_limit)
               fprintf(stderr, "[trace] Ativo (ilimitado) — Ctrl+C para parar\n");
          else
               fprintf(stderr, "[trace] Ativo para %llu instruções\n",
                       (unsigned long long)trace_limit);
     }

     SDL_Gamepad *gamepad = NULL;
     {
          int count = 0;
          SDL_JoystickID *ids = SDL_GetGamepads(&count);
          if (ids && count > 0)
               gamepad = SDL_OpenGamepad(ids[0]);
          SDL_free(ids);
     }

     struct gba_ctx *ctx = gba->frontend.data;
     uint64_t last_ns = SDL_GetTicksNS();

     while (!gba->quit)
     {
          SDL_Event e;
          while (SDL_PollEvent(&e))
          {
               switch (e.type)
               {
               case SDL_EVENT_QUIT:
                    gba->quit = true;
                    break;
               case SDL_EVENT_KEY_DOWN:
               case SDL_EVENT_KEY_UP:
                    if (debug_ui)
                         gba_debug_ui_process_event(gba, &e);
                    if (!e.key.repeat)
                         handle_key(gba, e.key.key, e.key.down);
                    break;
               case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
               case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    if (debug_ui)
                         gba_debug_ui_process_event(gba, &e);
                    handle_gamepad_button(gba, (SDL_GamepadButton)e.gbutton.button,
                                          e.gbutton.down);
                    break;
               case SDL_EVENT_GAMEPAD_ADDED:
                    if (debug_ui)
                         gba_debug_ui_process_event(gba, &e);
                    if (!gamepad)
                         gamepad = SDL_OpenGamepad(e.gdevice.which);
                    break;
               case SDL_EVENT_GAMEPAD_REMOVED:
                    if (debug_ui)
                         gba_debug_ui_process_event(gba, &e);
                    if (gamepad && SDL_GetGamepadID(gamepad) == e.gdevice.which)
                    {
                         SDL_CloseGamepad(gamepad);
                         gamepad = NULL;
                    }
                    break;
               case SDL_EVENT_DROP_FILE:
                    if (debug_ui)
                         gba_debug_ui_process_event(gba, &e);
                    /* Drag-and-drop: recarrega ROM */
                    gba_cart_unload(gba);
                    if (gba_cart_load(gba, e.drop.data))
                         gba_reset(gba);
                    last_ns = SDL_GetTicksNS();
                    SDL_free((void *)e.drop.data);
                    break;
               default:
                    if (debug_ui)
                         gba_debug_ui_process_event(gba, &e);
                    break;
               }
          }

          if (debug_ui)
               gba_debug_ui_render(gba, ctx->pixels);

          if (debug_ui && gba->debug.state == GBA_DEBUG_PAUSED)
          {
               SDL_Delay(8);
               last_ns = SDL_GetTicksNS();
               continue;
          }

          float speed = ctx->fast_forward ? 2.0f : 1.0f;
          if (debug_ui)
               speed = gba_debug_ui_speed_multiplier();
          uint64_t now_ns = SDL_GetTicksNS();
          uint64_t elapsed = now_ns - last_ns;
          last_ns = now_ns;

          /* Clamp para evitar spiral-of-death */
          if (elapsed > 50000000ULL)
               elapsed = 50000000ULL;

          /* Quantos frames caber nesse tempo */
          uint64_t frame_ns = (uint64_t)(1000000000.0 / 59.7275);
          uint64_t frames = (uint64_t)((float)elapsed * speed / (float)frame_ns);
          if (frames < 1)
               frames = 1;
          if (frames > 4)
               frames = 4;

          for (uint64_t f = 0; f < frames && !gba->quit; f++)
               gba_run_frame(gba);
     }

     if (gamepad)
          SDL_CloseGamepad(gamepad);
     gba->frontend.destroy(gba->frontend.data);
     gba->frontend.data = NULL;

     if (gba->debug.trace_fp && gba->debug.trace_fp != stderr)
          fclose(gba->debug.trace_fp);

     gba_destroy(gba);

     return 0;
}
