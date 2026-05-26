#ifndef GBA_DEBUG_UI_H
#define GBA_DEBUG_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gba;

bool gba_debug_ui_init(struct gba *gba, SDL_Window *window);
void gba_debug_ui_process_event(struct gba *gba, SDL_Event *event);
void gba_debug_ui_render(struct gba *gba, const uint32_t *pixels);
void gba_debug_ui_destroy(void);
float gba_debug_ui_speed_multiplier(void);

#ifdef __cplusplus
}
#endif

#endif /* GBA_DEBUG_UI_H */
