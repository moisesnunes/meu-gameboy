#ifndef SM83_SIGNAL_OVERLAY_H
#define SM83_SIGNAL_OVERLAY_H

#include <stdint.h>
#include <stdbool.h>

/* sm83_signal_overlay.h
 * Maps emulator CPU state to visual signal activity on the die.
 * Updated each step/run/pause from gb->cpu; used by draw_panel_transistor_viz()
 * to color-highlight named instances.
 */

#define SM83_OVERLAY_SIGNAL_COUNT 128 /* capacity; current node map has ~100 signals */

typedef struct
{
     /* Normalized [0,1] position of the instance on the die */
     float nx, ny;
     /* Signal group index for coloring (SM83_SIG_GROUP_*) */
     int group;
     /* Signal value: 0 or 1 */
     uint8_t value;
     /* Fade timer: 1.0 on change, decays to 0 each frame */
     float fade;
     /* Label string */
     const char *label;
     /* True if this signal has a known die position */
     bool valid;
} Sm83OverlaySignal;

typedef struct
{
     Sm83OverlaySignal signals[SM83_OVERLAY_SIGNAL_COUNT];
     int count;
} Sm83SignalOverlay;

struct gb;

/* Initialize overlay: resolve instance names to die positions */
void sm83_overlay_init(Sm83SignalOverlay *ov);

/* Update overlay from current CPU state; call after each step/pause */
void sm83_overlay_update(Sm83SignalOverlay *ov, const struct gb *gb);

/* Decay fade timers each frame (dt in seconds) */
void sm83_overlay_tick(Sm83SignalOverlay *ov, float dt);

#endif /* SM83_SIGNAL_OVERLAY_H */
