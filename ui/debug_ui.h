#ifndef _GB_DEBUG_UI_H_
#define _GB_DEBUG_UI_H_

#include <SDL3/SDL.h>

struct gb;

/* Inicializa ImGui no contexto OpenGL da janela principal */
void debug_ui_init(struct gb *gb);

/* Processa um SDL_Event (ImGui + atalhos F5/F10/Ctrl+O + drag-and-drop) */
void debug_ui_process_event(struct gb *gb, SDL_Event *e);

/* Renderiza um frame completo (jogo + painéis de debug) */
void debug_ui_render(struct gb *gb);

/* Libera todos os recursos */
void debug_ui_destroy(struct gb *gb);

/* Retorna o path de ROM pendente (vindo do file dialog ou drag-and-drop),
 * ou NULL se não há nenhum.  Limpar com debug_ui_clear_pending_rom(). */
const char *debug_ui_pending_rom(void);

/* Registra o ROM como recente e limpa o path pendente */
void debug_ui_clear_pending_rom(const char *loaded_path);

/* Retorna o multiplicador de velocidade atual (1.0 = normal, >1.0 = fast-forward) */
float debug_ui_get_speed_multiplier(void);

/* Retorna se novas ROMs devem iniciar pausadas no debugger */
bool debug_ui_start_paused(void);

#endif /* _GB_DEBUG_UI_H_ */
