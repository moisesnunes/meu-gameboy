#ifndef GBA_DEBUG_UI_PANELS_H
#define GBA_DEBUG_UI_PANELS_H

#include <stdint.h>

struct gba;

void gba_debug_ui_panels_init(void);
void gba_debug_ui_panels_shutdown(void);

void gba_draw_panel_cpu_control(struct gba *gba);
void gba_draw_panel_disasm(struct gba *gba);
void gba_draw_panel_memory(struct gba *gba);
void gba_draw_panel_gpu(struct gba *gba);
void gba_draw_panel_oam(struct gba *gba);
void gba_draw_panel_apu(struct gba *gba);
void gba_draw_panel_profiler(struct gba *gba);

extern uint32_t g_gba_mem_addr;

#endif /* GBA_DEBUG_UI_PANELS_H */
