/* hw_schematic_map.h — Fase B: semantic mapping of schematic nets and components.
 *
 * Links KiCad net names and component refs to emulator subsystems with an
 * explicit confidence level.  Nothing here affects emulation — it is purely
 * metadata used by the UI to colour wires, show tooltips, and later to
 * project trace events onto the schematic.
 *
 * Confidence levels (HwMapConfidence):
 *   CONFIRMED  — verified against the DMG schematics or die photographs.
 *   PROBABLE   — inferred from net name, bus position, or component ref.
 *   PROXY      — useful visual grouping, but not a direct physical net.
 *   UNKNOWN    — present in the schematic; no mapping yet.
 */
#ifndef HW_SCHEMATIC_MAP_H
#define HW_SCHEMATIC_MAP_H

#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Signal kind — what logical role does this net play?
 * ------------------------------------------------------------------------- */
typedef enum {
    HW_SIG_ADDR     = 0,  /* address bus bit (A0..A15)           */
    HW_SIG_DATA     = 1,  /* data bus bit (D0..D7)               */
    HW_SIG_CTRL_RD  = 2,  /* /RD read strobe                     */
    HW_SIG_CTRL_WR  = 3,  /* /WR write strobe                    */
    HW_SIG_CTRL_CS  = 4,  /* chip-select (cart /CS, WRAM /MCS)   */
    HW_SIG_CLOCK    = 5,  /* system clock (PHI, 4 MHz)           */
    HW_SIG_RESET    = 6,  /* /RES reset line                     */
    HW_SIG_IRQ      = 7,  /* interrupt request                   */
    HW_SIG_LCD      = 8,  /* LCD pixel / control signals         */
    HW_SIG_AUDIO    = 9,  /* audio output (SO1/SO2/VEE)          */
    HW_SIG_SERIAL   = 10, /* serial link (SCK/SIN/SOUT)          */
    HW_SIG_JOYPAD   = 11, /* joypad matrix lines (P1x)           */
    HW_SIG_POWER    = 12, /* power rails (VCC/VDD/VIN/GND)       */
    HW_SIG_WRAM_ADDR= 13, /* WRAM-internal address bus (MA0..12) */
    HW_SIG_WRAM_DATA= 14, /* WRAM-internal data bus (MD0..7)     */
    HW_SIG_UNKNOWN  = 15, /* not yet classified                  */
} HwSignalKind;

/* -------------------------------------------------------------------------
 * Component kind — which emulator subsystem owns this component?
 * ------------------------------------------------------------------------- */
typedef enum {
    HW_COMP_CPU         = 0,  /* DMG SoC (CPU + PPU + APU + timers) */
    HW_COMP_CART        = 1,  /* cartridge connector / MBC          */
    HW_COMP_WRAM        = 2,  /* work RAM chip (U2, U3)             */
    HW_COMP_VRAM        = 3,  /* video RAM (integrated in DMG SoC)  */
    HW_COMP_PPU_LCD     = 4,  /* LCD connector / pixel output       */
    HW_COMP_APU_AUDIO   = 5,  /* audio amp / headphone jack         */
    HW_COMP_TIMER_CLOCK = 6,  /* crystal oscillator / timing        */
    HW_COMP_JOYPAD      = 7,  /* button matrix                      */
    HW_COMP_SERIAL      = 8,  /* link port connector                */
    HW_COMP_POWER       = 9,  /* power supply / decoupling caps     */
    HW_COMP_MISC        = 10, /* passive: R, C, etc. (no subsystem) */
    HW_COMP_UNKNOWN     = 11,
} HwComponentKind;

/* -------------------------------------------------------------------------
 * Confidence in a mapping assertion
 * ------------------------------------------------------------------------- */
typedef enum {
    HW_CONF_CONFIRMED = 0, /* verified                    */
    HW_CONF_PROBABLE  = 1, /* inferred from name/position */
    HW_CONF_PROXY     = 2, /* visual grouping only        */
    HW_CONF_UNKNOWN   = 3, /* no mapping yet              */
} HwMapConfidence;

/* -------------------------------------------------------------------------
 * Per-net semantic record
 *
 * net_id        — index into hw_nets[] from hw_schematic_data.h
 * schematic_name— exact KiCad net name (e.g. "~{RD}", "A7")
 * canonical_name— normalised name used by the rest of the UI (e.g. "nRD","A7")
 * kind          — HwSignalKind
 * bit           — bit index for addr/data/wram buses; -1 if not a bus bit
 * active_low    — true when the signal is asserted low
 * confidence    — how certain is this mapping
 * ------------------------------------------------------------------------- */
typedef struct {
    int16_t        net_id;
    const char    *schematic_name;
    const char    *canonical_name;
    HwSignalKind   kind;
    int8_t         bit;
    bool           active_low;
    HwMapConfidence confidence;
} HwNetSemantic;

/* -------------------------------------------------------------------------
 * Per-component semantic record
 *
 * component_id   — index into hw_components[] from hw_schematic_data.h
 * ref            — KiCad reference (e.g. "U1", "P1")
 * value          — KiCad value string
 * kind           — HwComponentKind
 * emulator_owner — short description of the gb struct field(s) that own it
 * confidence     — how certain is this mapping
 * ------------------------------------------------------------------------- */
typedef struct {
    int16_t          component_id;
    const char      *ref;
    const char      *value;
    HwComponentKind  kind;
    const char      *emulator_owner;
    HwMapConfidence  confidence;
} HwComponentSemantic;

/* -------------------------------------------------------------------------
 * Tables — defined in hw_schematic_map.c
 * ------------------------------------------------------------------------- */
extern const HwNetSemantic       hw_net_map[];
extern const HwComponentSemantic hw_component_map[];
extern const int                 hw_net_map_count;
extern const int                 hw_component_map_count;

/* -------------------------------------------------------------------------
 * Lookup helpers (linear scan — for init / tooltip only, not hot path)
 * ------------------------------------------------------------------------- */

/* Find the semantic record for a net by its net_id (-1 index → NULL). */
const HwNetSemantic       *hw_map_find_net(int net_id);

/* Find the semantic record for a component by its component_id. */
const HwComponentSemantic *hw_map_find_component(int component_id);

/* Human-readable strings for enum values (for tooltips). */
const char *hw_signal_kind_name(HwSignalKind k);
const char *hw_component_kind_name(HwComponentKind k);
const char *hw_confidence_name(HwMapConfidence c);

#endif /* HW_SCHEMATIC_MAP_H */
