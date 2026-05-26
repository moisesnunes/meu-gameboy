#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <assert.h>
#include "gb.h"
#include "sdl.h"

struct gb_sdl_context
{
     SDL_Window        *window;
     SDL_AudioStream   *audio_stream;
     uint32_t           pixels[GB_LCD_WIDTH * GB_LCD_HEIGHT];
     SDL_Gamepad       *gamepad;
     unsigned           audio_buffer_index;
     size_t             audio_buffer_offset;
};

static SDL_Keycode s_key_map[8] = {
     [GB_INPUT_RIGHT]  = SDLK_RIGHT,
     [GB_INPUT_LEFT]   = SDLK_LEFT,
     [GB_INPUT_UP]     = SDLK_UP,
     [GB_INPUT_DOWN]   = SDLK_DOWN,
     [GB_INPUT_A]      = SDLK_LCTRL,
     [GB_INPUT_B]      = SDLK_LSHIFT,
     [GB_INPUT_SELECT] = SDLK_RSHIFT,
     [GB_INPUT_START]  = SDLK_RETURN,
};

static const SDL_Keycode s_default_key_map[8] = {
     [GB_INPUT_RIGHT]  = SDLK_RIGHT,
     [GB_INPUT_LEFT]   = SDLK_LEFT,
     [GB_INPUT_UP]     = SDLK_UP,
     [GB_INPUT_DOWN]   = SDLK_DOWN,
     [GB_INPUT_A]      = SDLK_LCTRL,
     [GB_INPUT_B]      = SDLK_LSHIFT,
     [GB_INPUT_SELECT] = SDLK_RSHIFT,
     [GB_INPUT_START]  = SDLK_RETURN,
};

/* ── Callbacks de desenho ── */

static void gb_sdl_draw_line_dmg(struct gb *gb, unsigned ly,
                                 union gb_gpu_color line[GB_LCD_WIDTH])
{
     struct gb_sdl_context *ctx = gb->frontend.data;

     static const uint32_t col_map[4] = {
         [GB_COL_WHITE]     = 0xff75a32c,
         [GB_COL_LIGHTGREY] = 0xff387a21,
         [GB_COL_DARKGREY]  = 0xff255116,
         [GB_COL_BLACK]     = 0xff12280b,
     };

     for (unsigned i = 0; i < GB_LCD_WIDTH; i++)
          ctx->pixels[ly * GB_LCD_WIDTH + i] = col_map[line[i].dmg_color];
}

static uint32_t gb_sdl_5_to_8bits(uint32_t v) { return (v << 3) | (v >> 2); }

static uint32_t gb_sdl_gbc_to_xrgb8888(uint16_t c)
{
     uint32_t r = gb_sdl_5_to_8bits(c & 0x1f);
     uint32_t g = gb_sdl_5_to_8bits((c >> 5) & 0x1f);
     uint32_t b = gb_sdl_5_to_8bits((c >> 10) & 0x1f);
     return 0xff000000 | (r << 16) | (g << 8) | b;
}

static void gb_sdl_draw_line_gbc(struct gb *gb, unsigned ly,
                                 union gb_gpu_color line[GB_LCD_WIDTH])
{
     struct gb_sdl_context *ctx = gb->frontend.data;
     for (unsigned i = 0; i < GB_LCD_WIDTH; i++)
          ctx->pixels[ly * GB_LCD_WIDTH + i] =
              gb_sdl_gbc_to_xrgb8888(line[i].gbc_color);
}

/* flip: pixels já estão em ctx->pixels; apresentação feita pelo debug_ui */
static void gb_sdl_flip(struct gb *gb) { (void)gb; }

/* ── Input ── */

static void gb_sdl_handle_key(struct gb *gb, SDL_Keycode key, bool pressed)
{
     if (key == SDLK_Q || key == SDLK_ESCAPE)
     {
          if (pressed) gb->quit = true;
          return;
     }

     for (unsigned button = 0; button < 8; button++)
     {
          if (key == s_key_map[button])
          {
               gb_input_set(gb, button, pressed);
               return;
          }
     }
}

static void gb_sdl_handle_button(struct gb *gb, SDL_GamepadButton button, bool pressed)
{
     switch (button)
     {
     case SDL_GAMEPAD_BUTTON_START:          gb_input_set(gb, GB_INPUT_START,  pressed); break;
     case SDL_GAMEPAD_BUTTON_BACK:           gb_input_set(gb, GB_INPUT_SELECT, pressed); break;
     case SDL_GAMEPAD_BUTTON_EAST:           gb_input_set(gb, GB_INPUT_A,      pressed); break;
     case SDL_GAMEPAD_BUTTON_SOUTH:          gb_input_set(gb, GB_INPUT_B,      pressed); break;
     case SDL_GAMEPAD_BUTTON_DPAD_UP:        gb_input_set(gb, GB_INPUT_UP,     pressed); break;
     case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      gb_input_set(gb, GB_INPUT_DOWN,   pressed); break;
     case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      gb_input_set(gb, GB_INPUT_LEFT,   pressed); break;
     case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     gb_input_set(gb, GB_INPUT_RIGHT,  pressed); break;
     default: break;
     }
}

static void gb_sdl_open_gamepad(struct gb *gb)
{
     struct gb_sdl_context *ctx = gb->frontend.data;
     if (ctx->gamepad) return;

     int count = 0;
     SDL_JoystickID *ids = SDL_GetGamepads(&count);
     if (ids && count > 0)
     {
          ctx->gamepad = SDL_OpenGamepad(ids[0]);
          if (ctx->gamepad)
               printf("Using gamepad '%s'\n", SDL_GetGamepadName(ctx->gamepad));
     }
     SDL_free(ids);
}

void gb_sdl_process_event(struct gb *gb, SDL_Event *e)
{
     struct gb_sdl_context *ctx = gb->frontend.data;

     switch (e->type)
     {
     case SDL_EVENT_QUIT:
          gb->quit = true;
          break;
     case SDL_EVENT_KEY_DOWN:
     case SDL_EVENT_KEY_UP:
          if (!e->key.repeat)
               gb_sdl_handle_key(gb, e->key.key, e->key.down);
          break;
     case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
     case SDL_EVENT_GAMEPAD_BUTTON_UP:
          gb_sdl_handle_button(gb, (SDL_GamepadButton)e->gbutton.button, e->gbutton.down);
          break;
     case SDL_EVENT_GAMEPAD_REMOVED:
          if (ctx->gamepad &&
              SDL_GetGamepadID(ctx->gamepad) == e->gdevice.which)
          {
               SDL_CloseGamepad(ctx->gamepad);
               ctx->gamepad = NULL;
               gb_sdl_open_gamepad(gb);
          }
          break;
     case SDL_EVENT_GAMEPAD_ADDED:
          gb_sdl_open_gamepad(gb);
          break;
     }
}

static void gb_sdl_refresh_input(struct gb *gb)
{
     SDL_Event e;
     while (SDL_PollEvent(&e))
          gb_sdl_process_event(gb, &e);
}

/* ── Áudio (SDL3 AudioStream) ── */

static void SDLCALL gb_sdl_audio_callback(void *userdata,
                                          SDL_AudioStream *stream,
                                          int additional_amount,
                                          int total_amount)
{
     struct gb *gb = userdata;
     struct gb_sdl_context *ctx = gb->frontend.data;
     (void)total_amount;

     /* Precisamos de additional_amount bytes; cada buffer de amostra é fixo. */
     int needed = additional_amount;
     while (needed > 0)
     {
          struct gb_spu_sample_buffer *buf;

          buf = &gb->spu.buffers[ctx->audio_buffer_index];

          if (ctx->audio_buffer_offset == 0 &&
              sem_trywait(&buf->ready) != 0)
          {
               /* Buffer ainda não pronto: insere silêncio sem avançar o ring. */
               static const int16_t silence[GB_SPU_SAMPLE_BUFFER_LENGTH * 2] = {0};
               int chunk = needed < (int)sizeof(silence) ? needed : (int)sizeof(silence);

               SDL_PutAudioStreamData(stream, silence, chunk);
               needed -= chunk;
               continue;
          }

          size_t remaining = sizeof(buf->samples) - ctx->audio_buffer_offset;
          int chunk = remaining < (size_t)needed ? (int)remaining : needed;
          const uint8_t *samples = (const uint8_t *)buf->samples;

          SDL_PutAudioStreamData(stream,
                                 samples + ctx->audio_buffer_offset,
                                 chunk);

          ctx->audio_buffer_offset += chunk;
          needed -= chunk;

          if (ctx->audio_buffer_offset == sizeof(buf->samples))
          {
               ctx->audio_buffer_offset = 0;
               ctx->audio_buffer_index =
                   (ctx->audio_buffer_index + 1) % GB_SPU_SAMPLE_BUFFER_COUNT;
               sem_post(&buf->free);
          }
     }
}

/* ── Destroy ── */

static void gb_sdl_destroy(struct gb *gb)
{
     struct gb_sdl_context *ctx = gb->frontend.data;

     if (ctx->gamepad)
          SDL_CloseGamepad(ctx->gamepad);
     if (ctx->audio_stream)
          SDL_DestroyAudioStream(ctx->audio_stream);

     /* Janela e GL context são destruídos pelo debug_ui */
     SDL_Quit();
     free(ctx);
     gb->frontend.data = NULL;
}

/* ── Init ── */

void gb_sdl_frontend_init(struct gb *gb)
{
     struct gb_sdl_context *ctx = malloc(sizeof(*ctx));
     if (!ctx) { perror("malloc"); die(); }

     memset(ctx, 0, sizeof(*ctx));
     gb->frontend.data = ctx;

     if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD))
     {
          fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
          die();
     }

     /* Atributos OpenGL — antes de criar a janela */
     SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
     SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
     SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
     SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
     SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
     SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

     ctx->window = SDL_CreateWindow(
         "Gaembuoy",
         1600, 900,
         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
     if (!ctx->window)
     {
          fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
          die();
     }

     /* Áudio via AudioStream */
     SDL_AudioSpec spec = {
         .format   = SDL_AUDIO_S16,
         .channels = 2,
         .freq     = GB_SPU_SAMPLE_RATE_HZ,
     };
     ctx->audio_stream = SDL_OpenAudioDeviceStream(
         SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
         gb_sdl_audio_callback, gb);
     if (!ctx->audio_stream)
     {
          fprintf(stderr, "SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
          die();
     }
     SDL_ResumeAudioStreamDevice(ctx->audio_stream);

     gb->frontend.draw_line_dmg  = gb_sdl_draw_line_dmg;
     gb->frontend.draw_line_gbc  = gb_sdl_draw_line_gbc;
     gb->frontend.flip           = gb_sdl_flip;
     gb->frontend.refresh_input  = gb_sdl_refresh_input;
     gb->frontend.destroy        = gb_sdl_destroy;

     memset(ctx->pixels, 0, sizeof(ctx->pixels));
     gb_sdl_open_gamepad(gb);
}

void gb_sdl_reset_audio_buffer(struct gb *gb)
{
     struct gb_sdl_context *ctx = gb->frontend.data;
     ctx->audio_buffer_index  = 0;
     ctx->audio_buffer_offset = 0;
}

/* ── Getters para o debug_ui ── */

SDL_Window *gb_sdl_get_window(struct gb *gb)
{
     return ((struct gb_sdl_context *)gb->frontend.data)->window;
}

const uint32_t *gb_sdl_get_pixels(struct gb *gb)
{
     return ((struct gb_sdl_context *)gb->frontend.data)->pixels;
}

void gb_sdl_set_audio_gain(struct gb *gb, float gain)
{
     struct gb_sdl_context *ctx = gb->frontend.data;
     if (ctx->audio_stream)
          SDL_SetAudioStreamGain(ctx->audio_stream, gain);
}

void gb_sdl_set_key_mapping(unsigned button, SDL_Keycode key)
{
     if (button < 8 && key != SDLK_UNKNOWN)
          s_key_map[button] = key;
}

SDL_Keycode gb_sdl_get_key_mapping(unsigned button)
{
     return button < 8 ? s_key_map[button] : SDLK_UNKNOWN;
}

void gb_sdl_reset_key_mapping(void)
{
     memcpy(s_key_map, s_default_key_map, sizeof(s_key_map));
}
