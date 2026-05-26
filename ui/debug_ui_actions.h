#ifndef DEBUG_UI_ACTIONS_H
#define DEBUG_UI_ACTIONS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gb;

const char *debug_ui_action_rom_title(struct gb *gb);
bool debug_ui_action_save_screenshot(struct gb *gb, char *message, size_t message_len);
bool debug_ui_action_state_slot_exists(struct gb *gb, int slot);
bool debug_ui_action_save_state_slot(struct gb *gb, int slot, char *message, size_t message_len);
bool debug_ui_action_load_state_slot(struct gb *gb, int slot, char *message, size_t message_len);
void debug_ui_action_reset(struct gb *gb, bool start_paused);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_UI_ACTIONS_H */
