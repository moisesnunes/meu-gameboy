/* hw_schematic_map.c — Fase B: semantic mapping tables for the DMG schematic.
 *
 * Net and component indices are stable relative to the generated
 * hw_schematic_data.c / hw_schematic_data.h produced by kicad_extractor.py
 * from DMG-CPU-06.kicad_sch (Gekkio, CC BY 4.0).
 *
 * Confidence key:
 *   CONFIRMED — name matches DMG schematics / known pinout.
 *   PROBABLE  — inferred from KiCad name convention or bus position.
 *   PROXY     — visual/grouping only; not a direct physical net.
 *   UNKNOWN   — present but unclassified.
 */
#include "hw_schematic_map.h"
#include <stddef.h>

/* Short aliases for readability inside this file only */
#define CONF  HW_CONF_CONFIRMED
#define PROB  HW_CONF_PROBABLE
#define PROX  HW_CONF_PROXY
#define UNK   HW_CONF_UNKNOWN

/* -------------------------------------------------------------------------
 * Net semantic table
 *
 * Columns: net_id, schematic_name, canonical_name, kind, bit, active_low, conf
 *
 * net_ids come from the order nets appear in hw_schematic_data.c.
 * Run `python3 tools/check_hw_schematic.py --summary` to refresh them.
 * ------------------------------------------------------------------------- */
const HwNetSemantic hw_net_map[] = {

    /* ── Address bus A0..A15 ─────────────────────────────────────────────── */
    {  8, "A0",  "A0",  HW_SIG_ADDR, 0,  false, CONF },
    { 20, "A1",  "A1",  HW_SIG_ADDR, 1,  false, CONF },
    { 29, "A2",  "A2",  HW_SIG_ADDR, 2,  false, CONF },
    { 44, "A3",  "A3",  HW_SIG_ADDR, 3,  false, CONF },
    {  4, "A4",  "A4",  HW_SIG_ADDR, 4,  false, CONF },
    { 24, "A5",  "A5",  HW_SIG_ADDR, 5,  false, CONF },
    { 86, "A6",  "A6",  HW_SIG_ADDR, 6,  false, CONF },
    { 26, "A7",  "A7",  HW_SIG_ADDR, 7,  false, CONF },
    { 73, "A8",  "A8",  HW_SIG_ADDR, 8,  false, CONF },
    { 66, "A9",  "A9",  HW_SIG_ADDR, 9,  false, CONF },
    { 30, "A10", "A10", HW_SIG_ADDR, 10, false, CONF },
    { 83, "A11", "A11", HW_SIG_ADDR, 11, false, CONF },
    { 22, "A12", "A12", HW_SIG_ADDR, 12, false, CONF },
    { 19, "A13", "A13", HW_SIG_ADDR, 13, false, CONF },
    { 47, "A14", "A14", HW_SIG_ADDR, 14, false, CONF },
    { 85, "A15", "A15", HW_SIG_ADDR, 15, false, CONF },

    /* ── Data bus D0..D7 ─────────────────────────────────────────────────── */
    { 74, "D0", "D0", HW_SIG_DATA, 0, false, CONF },
    { 62, "D1", "D1", HW_SIG_DATA, 1, false, CONF },
    { 54, "D2", "D2", HW_SIG_DATA, 2, false, CONF },
    { 27, "D3", "D3", HW_SIG_DATA, 3, false, CONF },
    { 72, "D4", "D4", HW_SIG_DATA, 4, false, CONF },
    { 10, "D5", "D5", HW_SIG_DATA, 5, false, CONF },
    { 67, "D6", "D6", HW_SIG_DATA, 6, false, CONF },
    {  2, "D7", "D7", HW_SIG_DATA, 7, false, CONF },

    /* ── WRAM address bus MA0..MA12 ──────────────────────────────────────── */
    { 81, "MA0",  "MA0",  HW_SIG_WRAM_ADDR, 0,  false, CONF },
    { 56, "MA1",  "MA1",  HW_SIG_WRAM_ADDR, 1,  false, CONF },
    {  0, "MA2",  "MA2",  HW_SIG_WRAM_ADDR, 2,  false, CONF },
    { 90, "MA3",  "MA3",  HW_SIG_WRAM_ADDR, 3,  false, CONF },
    { 88, "MA4",  "MA4",  HW_SIG_WRAM_ADDR, 4,  false, CONF },
    { 37, "MA5",  "MA5",  HW_SIG_WRAM_ADDR, 5,  false, CONF },
    { 75, "MA6",  "MA6",  HW_SIG_WRAM_ADDR, 6,  false, CONF },
    { 57, "MA7",  "MA7",  HW_SIG_WRAM_ADDR, 7,  false, CONF },
    { 14, "MA8",  "MA8",  HW_SIG_WRAM_ADDR, 8,  false, CONF },
    { 91, "MA9",  "MA9",  HW_SIG_WRAM_ADDR, 9,  false, CONF },
    { 39, "MA10", "MA10", HW_SIG_WRAM_ADDR, 10, false, CONF },
    { 28, "MA11", "MA11", HW_SIG_WRAM_ADDR, 11, false, CONF },
    { 89, "MA12", "MA12", HW_SIG_WRAM_ADDR, 12, false, CONF },

    /* ── WRAM data bus MD0..MD7 ──────────────────────────────────────────── */
    { 63, "MD0", "MD0", HW_SIG_WRAM_DATA, 0, false, CONF },
    { 53, "MD1", "MD1", HW_SIG_WRAM_DATA, 1, false, CONF },
    { 55, "MD2", "MD2", HW_SIG_WRAM_DATA, 2, false, CONF },
    {  5, "MD3", "MD3", HW_SIG_WRAM_DATA, 3, false, CONF },
    { 64, "MD4", "MD4", HW_SIG_WRAM_DATA, 4, false, CONF },
    { 61, "MD5", "MD5", HW_SIG_WRAM_DATA, 5, false, CONF },
    { 49, "MD6", "MD6", HW_SIG_WRAM_DATA, 6, false, CONF },
    { 58, "MD7", "MD7", HW_SIG_WRAM_DATA, 7, false, CONF },

    /* ── Bus control ──────────────────────────────────────────────────────── */
    { 82, "~{RD}",  "nRD",  HW_SIG_CTRL_RD, -1, true,  CONF },
    { 80, "~{WR}",  "nWR",  HW_SIG_CTRL_WR, -1, true,  CONF },
    { 76, "~{CS}",  "nCS",  HW_SIG_CTRL_CS, -1, true,  CONF },  /* cart /CS */
    { 33, "~{MCS}", "nMCS", HW_SIG_CTRL_CS, -1, true,  CONF },  /* WRAM /CS */
    { 15, "~{MRD}", "nMRD", HW_SIG_CTRL_RD, -1, true,  PROB },  /* WRAM /RD */
    { 46, "~{MWR}", "nMWR", HW_SIG_CTRL_WR, -1, true,  PROB },  /* WRAM /WR */

    /* ── Clock and reset ─────────────────────────────────────────────────── */
    { 70, "PHI",    "PHI",  HW_SIG_CLOCK, -1, false, CONF },
    { 65, "~{RES}", "nRES", HW_SIG_RESET, -1, true,  CONF },

    /* ── Serial link ─────────────────────────────────────────────────────── */
    { 84, "SCK",  "SCK",  HW_SIG_SERIAL, -1, false, CONF },
    { 21, "SIN",  "SIN",  HW_SIG_SERIAL, -1, false, CONF },
    { 12, "SOUT", "SOUT", HW_SIG_SERIAL, -1, false, CONF },

    /* ── Audio output ────────────────────────────────────────────────────── */
    {  9, "SO1", "SO1", HW_SIG_AUDIO, -1, false, CONF },
    { 42, "SO2", "SO2", HW_SIG_AUDIO, -1, false, CONF },
    {  6, "VEE", "VEE", HW_SIG_AUDIO, -1, false, CONF },  /* audio ground ref */

    /* ── LCD pixel / control ──────────────────────────────────────────────── */
    { 38, "LD0", "LD0", HW_SIG_LCD, 0,  false, CONF },
    { 71, "LD1", "LD1", HW_SIG_LCD, 1,  false, CONF },
    { 50, "CP",  "CP",  HW_SIG_LCD, -1, false, CONF },   /* pixel clock       */
    { 16, "CPG", "CPG", HW_SIG_LCD, -1, false, CONF },   /* clock pulse gate   */
    { 31, "CPL", "CPL", HW_SIG_LCD, -1, false, CONF },   /* clock pulse latch  */
    { 60, "FR",  "FR",  HW_SIG_LCD, -1, false, CONF },   /* frame signal       */
    { 87, "S",   "S",   HW_SIG_LCD, -1, false, CONF },   /* scan signal        */
    { 35, "ST",  "ST",  HW_SIG_LCD, -1, false, CONF },   /* start pulse        */

    /* ── Joypad matrix (P1x connector pins) ────────────────────────────── */
    { 17, "P10", "P10", HW_SIG_JOYPAD, 0, false, PROB },
    { 51, "P11", "P11", HW_SIG_JOYPAD, 1, false, PROB },
    { 36, "P12", "P12", HW_SIG_JOYPAD, 2, false, PROB },
    { 45, "P13", "P13", HW_SIG_JOYPAD, 3, false, PROB },
    { 59, "P14", "P14", HW_SIG_JOYPAD, 4, false, PROB },
    {  3, "P15", "P15", HW_SIG_JOYPAD, 5, false, PROB },

    /* ── Power rails ─────────────────────────────────────────────────────── */
    { 13, "VCC", "VCC", HW_SIG_POWER, -1, false, CONF },
    {  7, "VDD", "VDD", HW_SIG_POWER, -1, false, CONF },
    { 23, "VIN", "VIN", HW_SIG_POWER, -1, false, CONF },
    { 18, "GND", "GND", HW_SIG_POWER, -1, false, CONF },

    /* ── LCD connector pin SP (speaker?) ────────────────────────────────── */
    { 40, "SP", "SP", HW_SIG_UNKNOWN, -1, false, UNK },
};

const int hw_net_map_count = (int)(sizeof(hw_net_map) / sizeof(hw_net_map[0]));

/* -------------------------------------------------------------------------
 * Component semantic table
 *
 * component_id matches index in hw_components[] (hw_schematic_data.c).
 * ------------------------------------------------------------------------- */
const HwComponentSemantic hw_component_map[] = {
    /* id  ref    value             kind                  owner           conf */
    { 10, "U1", "DMG CPU",         HW_COMP_CPU,          "gb->cpu, gb->gpu, gb->spu, gb->timer, gb->irq", CONF },
    { 13, "U2", "LH5164LN",        HW_COMP_WRAM,         "gb->iram (bank 0)",   CONF },
    {  0, "U3", "LH5164LN",        HW_COMP_WRAM,         "gb->iram (bank 1)",   CONF },
    {  1, "P1", "GameBoy_Cartridge",HW_COMP_CART,         "gb->cart",            CONF },
    {  4, "X1", "4.194304 MHz",    HW_COMP_TIMER_CLOCK,  "sync / timing",       CONF },
    { 11, "J2", "Conn_01x21",      HW_COMP_PPU_LCD,      "gb->gpu (LCD out)",   CONF },

    /* Decoupling caps and passives — no emulator owner */
    {  2, "C15", "10n",  HW_COMP_POWER, "power decoupling", CONF },
    {  3, "C14", "10n",  HW_COMP_POWER, "power decoupling", CONF },
    {  5, "C21", "27p",  HW_COMP_POWER, "crystal load cap", CONF },
    {  6, "C22", "27p",  HW_COMP_POWER, "crystal load cap", CONF },
    {  7, "C10", "100n", HW_COMP_POWER, "power decoupling", CONF },
    {  9, "C12", "10n",  HW_COMP_POWER, "power decoupling", CONF },
    { 12, "C13", "10n",  HW_COMP_POWER, "power decoupling", CONF },
    {  8, "R7",  "180k", HW_COMP_MISC,  "reset RC network",  PROB },
    { 14, "R8",  "1M",   HW_COMP_MISC,  "reset RC network",  PROB },
};

const int hw_component_map_count =
    (int)(sizeof(hw_component_map) / sizeof(hw_component_map[0]));

/* -------------------------------------------------------------------------
 * Lookup helpers
 * ------------------------------------------------------------------------- */

const HwNetSemantic *hw_map_find_net(int net_id)
{
    if (net_id < 0)
        return NULL;
    for (int i = 0; i < hw_net_map_count; i++)
        if (hw_net_map[i].net_id == net_id)
            return &hw_net_map[i];
    return NULL;
}

const HwComponentSemantic *hw_map_find_component(int component_id)
{
    if (component_id < 0)
        return NULL;
    for (int i = 0; i < hw_component_map_count; i++)
        if (hw_component_map[i].component_id == component_id)
            return &hw_component_map[i];
    return NULL;
}

const char *hw_signal_kind_name(HwSignalKind k)
{
    switch (k) {
        case HW_SIG_ADDR:      return "addr";
        case HW_SIG_DATA:      return "data";
        case HW_SIG_CTRL_RD:   return "ctrl_rd";
        case HW_SIG_CTRL_WR:   return "ctrl_wr";
        case HW_SIG_CTRL_CS:   return "ctrl_cs";
        case HW_SIG_CLOCK:     return "clock";
        case HW_SIG_RESET:     return "reset";
        case HW_SIG_IRQ:       return "irq";
        case HW_SIG_LCD:       return "lcd";
        case HW_SIG_AUDIO:     return "audio";
        case HW_SIG_SERIAL:    return "serial";
        case HW_SIG_JOYPAD:    return "joypad";
        case HW_SIG_POWER:     return "power";
        case HW_SIG_WRAM_ADDR: return "wram_addr";
        case HW_SIG_WRAM_DATA: return "wram_data";
        default:               return "unknown";
    }
}

const char *hw_component_kind_name(HwComponentKind k)
{
    switch (k) {
        case HW_COMP_CPU:         return "cpu";
        case HW_COMP_CART:        return "cart";
        case HW_COMP_WRAM:        return "wram";
        case HW_COMP_VRAM:        return "vram";
        case HW_COMP_PPU_LCD:     return "ppu_lcd";
        case HW_COMP_APU_AUDIO:   return "apu_audio";
        case HW_COMP_TIMER_CLOCK: return "timer_clock";
        case HW_COMP_JOYPAD:      return "joypad";
        case HW_COMP_SERIAL:      return "serial";
        case HW_COMP_POWER:       return "power";
        case HW_COMP_MISC:        return "misc";
        default:                  return "unknown";
    }
}

const char *hw_confidence_name(HwMapConfidence c)
{
    switch (c) {
        case HW_CONF_CONFIRMED: return "confirmed";
        case HW_CONF_PROBABLE:  return "probable";
        case HW_CONF_PROXY:     return "proxy";
        default:                return "unknown";
    }
}
