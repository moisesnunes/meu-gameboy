#ifndef DEBUG_UI_PANELS_H
#define DEBUG_UI_PANELS_H

struct gb;

/* Aloca as texturas OpenGL dos painéis visuais (chamar após contexto GL pronto) */
void debug_ui_panels_init(void);

/* Libera texturas OpenGL */
void debug_ui_panels_shutdown(void);

/* Painéis de debug */
void draw_panel_tile_viewer(struct gb *gb);
void draw_panel_tilemap_viewer(struct gb *gb);
void draw_popup_rom_info(struct gb *gb);
void draw_panel_call_stack(struct gb *gb);
void draw_panel_cpu_control(struct gb *gb);
void draw_panel_control(struct gb *gb);
void draw_panel_registers(struct gb *gb);
void draw_panel_disasm(struct gb *gb);
void draw_panel_gpu(struct gb *gb);
void draw_panel_memory(struct gb *gb);
void draw_panel_breakpoints(struct gb *gb);
void draw_panel_watchpoints(struct gb *gb);
void draw_panel_oam(struct gb *gb);
void draw_panel_profiler(struct gb *gb);
void draw_panel_hw_viz(struct gb *gb);
void draw_panel_cpu_viz(struct gb *gb);
void draw_panel_transistor_viz(struct gb *gb);
void draw_panel_hw_schematic(struct gb *gb);

/* Estado compartilhado com debug_ui.cpp (memory viewer e popups) */
extern int  g_mem_addr;
extern int  g_mem_mode;
extern bool g_show_rom_info;

#endif /* DEBUG_UI_PANELS_H */
