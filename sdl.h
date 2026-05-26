#ifndef _GB_SDL_H_
#define _GB_SDL_H_

#include <SDL3/SDL.h>
#include <stdint.h>

struct gb;

void gb_sdl_frontend_init(struct gb *gb);

void gb_sdl_process_event(struct gb *gb, SDL_Event *e);

/* Retorna o ponteiro para a janela SDL (usada pelo debug_ui para criar o GL context) */
SDL_Window     *gb_sdl_get_window(struct gb *gb);

/* Retorna o buffer de pixels do frame atual (160×144 XRGB8888) */
const uint32_t *gb_sdl_get_pixels(struct gb *gb);

/* Controla o ganho do AudioStream (0.0 = mudo, 1.0 = normal) */
void gb_sdl_set_audio_gain(struct gb *gb, float gain);

/* Reseta o estado do ring-buffer de áudio do frontend (chamar junto com gb_spu_reset) */
void gb_sdl_reset_audio_buffer(struct gb *gb);

void gb_sdl_set_key_mapping(unsigned button, SDL_Keycode key);
SDL_Keycode gb_sdl_get_key_mapping(unsigned button);
void gb_sdl_reset_key_mapping(void);

#endif
