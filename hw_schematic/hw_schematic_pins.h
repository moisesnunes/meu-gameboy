/* hw_schematic_pins.h — DMG CPU (U1) pin table.
 *
 * Source: Gekkio gb-schematics DMG-CPU-06 (CC BY 4.0), Pan Docs, SameSuite.
 * Each entry maps a physical pin number → net_id (from hw_schematic_data.h).
 *
 * Pin positions are normalised schematic coordinates relative to U1's body:
 *   side: 0=left, 1=right, 2=top, 3=bottom
 *   pos:  0..1 from top-to-bottom on left/right, left-to-right on top/bottom.
 *
 * These are used only for tooltip display and wire-to-pin highlight; they do
 * not affect emulation.
 */
#pragma once
#include <stdint.h>
#include "hw_schematic_data.h"

typedef struct {
    uint8_t     pin;        /* physical pin number (1-based)        */
    const char *name;       /* net / signal name (canonical)        */
    int16_t     net_id;     /* index into hw_nets[], -1 if n/c      */
    uint8_t     side;       /* 0=left 1=right 2=top 3=bottom        */
    float       pos;        /* position along side [0..1]           */
} HwU1Pin;

#define HW_U1_PIN_COUNT 80

extern const HwU1Pin hw_u1_pins[HW_U1_PIN_COUNT];

/* Return pin entry for a given pin number, or NULL. */
const HwU1Pin *hw_u1_pin_find(int pin_number);

/* Return the first pin that connects to net_id, or NULL. */
const HwU1Pin *hw_u1_pin_find_net(int net_id);
