/* hw_schematic_trace.c — Fase D: event-to-net projection layer.
 *
 * Translates gb_hw_trace_event records into per-net and per-component
 * activity intensities (0..1 floats) that decay over time.
 *
 * Projection rules (address range → target component_id):
 *   0x0000-0x7FFF  cart ROM   → comp 1  (P1)
 *   0x8000-0x9FFF  VRAM       → comp 10 (U1, internal VRAM in DMG SoC)
 *   0xA000-0xBFFF  cart RAM   → comp 1  (P1)
 *   0xC000-0xFDFF  WRAM       → comp 13 (U2) or comp 0 (U3) — U2 primary
 *   0xFE00-0xFE9F  OAM        → comp 10 (U1, internal OAM)
 *   0xFF00-0xFFFF  IO/HRAM    → comp 10 (U1, internal IO)
 *
 * Net intensity on read: addr bits HIGH for address, data bits per byte.
 * Net intensity on write: same, plus WR strobe; RD deasserted.
 * Confidence scale: CONFIRMED=1.0, PROBABLE=0.8, PROXY=0.5, UNKNOWN=0.3.
 */
#include "hw_schematic_trace.h"
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Internal: net_id → array index fast lookup table.
 * Built once; avoids O(N²) per event.
 * ------------------------------------------------------------------------- */

/* Maps net_id [0..HW_NET_COUNT-1] → hw_net_map[] index, -1 if not mapped. */
static int8_t s_net_id_to_map[HW_NET_COUNT];
static bool   s_lookup_built = false;

static void build_lookup(void)
{
    if (s_lookup_built) return;
    memset(s_net_id_to_map, -1, sizeof(s_net_id_to_map));
    for (int i = 0; i < hw_net_map_count; i++) {
        int nid = hw_net_map[i].net_id;
        if (nid >= 0 && nid < HW_NET_COUNT)
            s_net_id_to_map[nid] = (int8_t)(i < 127 ? i : 127);
    }
    s_lookup_built = true;
}

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

static float conf_scale(HwMapConfidence c)
{
    switch (c) {
        case HW_CONF_CONFIRMED: return 1.0f;
        case HW_CONF_PROBABLE:  return 0.80f;
        case HW_CONF_PROXY:     return 0.50f;
        default:                return 0.30f;
    }
}

/* Pulse a single net by net_id. intensity is scaled by confidence. */
static void pulse_net(HwSchematicActivityState *st, int net_id,
                      float intensity, uint64_t seq,
                      gb_hw_trace_event_type type, int8_t level)
{
    if (net_id < 0 || net_id >= HW_NET_COUNT) return;
    int mi = (int)(uint8_t)s_net_id_to_map[net_id]; /* may be -1 cast to 255 */
    if ((int8_t)s_net_id_to_map[net_id] < 0) return;
    float scale = conf_scale(hw_net_map[mi].confidence);
    float v = intensity * scale;
    if (v > st->net_fade[net_id])
        st->net_fade[net_id] = v;
    st->net_event_seq[net_id] = seq;
    st->net_last_type[net_id] = type;
    if (level >= 0)
        st->net_level[net_id] = (int8_t)level;
}

/* Pulse a component by component_id. */
static void pulse_comp(HwSchematicActivityState *st, int comp_id,
                       float intensity, uint64_t seq,
                       gb_hw_trace_event_type type)
{
    if (comp_id < 0 || comp_id >= HW_COMPONENT_COUNT) return;
    if (intensity > st->comp_fade[comp_id])
        st->comp_fade[comp_id] = intensity;
    st->comp_event_seq[comp_id] = seq;
    st->comp_last_type[comp_id] = type;
}

/* Address bus: assert bits matching addr, deassert bits that are 0.
 * Net ids for A0..A15 from hw_net_map (confirmed). */
static const int8_t ADDR_NET_IDS[16] = {
     8, 20, 29, 44,  4, 24, 86, 26,  /* A0-A7  */
    73, 66, 30, 83, 22, 19, 47, 85,  /* A8-A15 */
};

/* Data bus: net ids for D0..D7 from hw_net_map (confirmed). */
static const int8_t DATA_NET_IDS[8] = {
    74, 62, 54, 27, 72, 10, 67, 2,  /* D0-D7 */
};

/* Control strobes */
#define NET_nRD  82
#define NET_nWR  80
#define NET_nCS  76
#define NET_nMCS 33

/* Component ids (indices in hw_components[]) for key chips */
#define COMP_U1  10  /* DMG CPU SoC    */
#define COMP_U2  13  /* WRAM primary   */
#define COMP_U3   0  /* WRAM secondary */
#define COMP_P1   1  /* Cartridge      */

static int target_comp(uint16_t addr)
{
    if (addr <= 0x7FFF) return COMP_P1;   /* ROM (cart)       */
    if (addr <= 0x9FFF) return COMP_U1;   /* VRAM (internal)  */
    if (addr <= 0xBFFF) return COMP_P1;   /* ext cart RAM     */
    if (addr <= 0xFDFF) return COMP_U2;   /* WRAM             */
    return COMP_U1;                        /* OAM / IO / HRAM  */
}

static void project_address(HwSchematicActivityState *st, uint16_t addr,
                             uint64_t seq, gb_hw_trace_event_type type)
{
    for (int i = 0; i < 16; i++) {
        int bit = (addr >> i) & 1;
        pulse_net(st, ADDR_NET_IDS[i], 1.0f, seq, type, (int8_t)bit);
    }
}

static void project_data(HwSchematicActivityState *st, uint8_t data,
                         uint64_t seq, gb_hw_trace_event_type type)
{
    for (int i = 0; i < 8; i++) {
        int bit = (data >> i) & 1;
        pulse_net(st, DATA_NET_IDS[i], 0.9f, seq, type, (int8_t)bit);
    }
}

/* -------------------------------------------------------------------------
 * Lifetime
 * ------------------------------------------------------------------------- */

void hw_activity_reset(HwSchematicActivityState *st)
{
    memset(st, 0, sizeof(*st));
    build_lookup();
}

void hw_activity_tick(HwSchematicActivityState *st, float dt, float decay_rate)
{
    float decay = dt * decay_rate;
    for (int i = 0; i < HW_NET_COUNT; i++) {
        st->net_fade[i] -= decay;
        if (st->net_fade[i] < 0.0f) st->net_fade[i] = 0.0f;
    }
    for (int i = 0; i < HW_COMPONENT_COUNT; i++) {
        st->comp_fade[i] -= decay;
        if (st->comp_fade[i] < 0.0f) st->comp_fade[i] = 0.0f;
    }
}

/* -------------------------------------------------------------------------
 * Projection
 * ------------------------------------------------------------------------- */

void hw_project_event(HwSchematicActivityState *st, const gb_hw_trace_event *ev)
{
    if (!st || !ev) return;
    build_lookup();

    uint64_t seq  = ev->seq;
    uint16_t addr = ev->addr;
    uint8_t  data = ev->data;
    gb_hw_trace_event_type type = ev->type;

    switch (type) {

    case GB_HW_EVT_CPU_FETCH:
        /* Fetch: address bus, data bus (opcode), RD asserted, CPU + target */
        project_address(st, addr, seq, type);
        project_data(st, ev->opcode, seq, type);
        pulse_net(st, NET_nRD, 1.0f, seq, type, 0); /* /RD low = asserted */
        pulse_net(st, NET_nWR, 0.3f, seq, type, 1); /* /WR high = deasserted */
        pulse_comp(st, COMP_U1, 1.0f, seq, type);
        pulse_comp(st, target_comp(addr), 0.9f, seq, type);
        break;

    case GB_HW_EVT_CPU_READ:
        project_address(st, addr, seq, type);
        project_data(st, data, seq, type);
        pulse_net(st, NET_nRD, 1.0f, seq, type, 0);
        pulse_net(st, NET_nWR, 0.3f, seq, type, 1);
        /* CS: cart range only */
        if (addr <= 0x7FFF || (addr >= 0xA000 && addr <= 0xBFFF))
            pulse_net(st, NET_nCS, 1.0f, seq, type, 0);
        else if (addr >= 0xC000 && addr <= 0xFDFF) {
            pulse_net(st, NET_nMCS, 1.0f, seq, type, 0);
        }
        pulse_comp(st, COMP_U1, 1.0f, seq, type);
        pulse_comp(st, target_comp(addr), 0.9f, seq, type);
        break;

    case GB_HW_EVT_CPU_WRITE:
        project_address(st, addr, seq, type);
        project_data(st, data, seq, type);
        pulse_net(st, NET_nWR, 1.0f, seq, type, 0);
        pulse_net(st, NET_nRD, 0.3f, seq, type, 1);
        if (addr <= 0x7FFF || (addr >= 0xA000 && addr <= 0xBFFF))
            pulse_net(st, NET_nCS, 1.0f, seq, type, 0);
        else if (addr >= 0xC000 && addr <= 0xFDFF)
            pulse_net(st, NET_nMCS, 1.0f, seq, type, 0);
        pulse_comp(st, COMP_U1, 1.0f, seq, type);
        pulse_comp(st, target_comp(addr), 0.9f, seq, type);
        break;

    case GB_HW_EVT_IRQ_REQUEST:
    case GB_HW_EVT_IRQ_ACK:
        /* IRQ: just light up the CPU — no dedicated IRQ net in schematic */
        pulse_comp(st, COMP_U1, 0.8f, seq, type);
        break;

    case GB_HW_EVT_DMA_READ:
        /* DMA reads source address */
        project_address(st, addr, seq, type);
        project_data(st, data, seq, type);
        pulse_net(st, NET_nRD, 0.8f, seq, type, 0);
        pulse_comp(st, COMP_U1, 0.7f, seq, type);
        pulse_comp(st, target_comp(addr), 0.6f, seq, type);
        break;

    case GB_HW_EVT_DMA_WRITE:
        /* DMA writes OAM (internal) */
        project_data(st, data, seq, type);
        pulse_net(st, NET_nWR, 0.8f, seq, type, 0);
        pulse_comp(st, COMP_U1, 0.7f, seq, type);
        break;

    case GB_HW_EVT_PPU_MODE:
        /* PPU mode transition: data=new mode (0-3), extra=LY.
         * Mode 3 (drawing): brightest; mode 2 (OAM scan): medium; 0/1: dim. */
        {
            float lcd_int = (ev->data == 3) ? 0.8f :
                            (ev->data == 2) ? 0.5f :
                            (ev->data == 1) ? 0.6f : 0.3f;
            pulse_comp(st, 11, lcd_int, seq, type); /* J2 = LCD connector */
            pulse_comp(st, COMP_U1, 0.4f, seq, type);
        }
        break;

    case GB_HW_EVT_APU_SAMPLE:
        pulse_comp(st, COMP_U1, 0.3f, seq, type);
        break;

    /* ── Fase E ── */

    case GB_HW_EVT_PPU_VBLANK:
        /* VBlank: LCD + CPU bright; data=LY (should be 144) */
        pulse_comp(st, 11, 1.0f, seq, type); /* J2 = LCD */
        pulse_comp(st, COMP_U1, 0.8f, seq, type);
        break;

    case GB_HW_EVT_PPU_HBLANK:
        /* HBlank: LCD dim, CPU dim; fires 144 times per frame */
        pulse_comp(st, 11, 0.4f, seq, type);
        pulse_comp(st, COMP_U1, 0.2f, seq, type);
        break;

    case GB_HW_EVT_OAM_DMA:
        /* OAM DMA: address bus = dest OAM addr, data bus = byte copied */
        project_address(st, ev->addr, seq, type);
        project_data(st, ev->data, seq, type);
        pulse_net(st, NET_nWR, 0.8f, seq, type, 0);
        pulse_comp(st, COMP_U1, 0.7f, seq, type);
        break;

    case GB_HW_EVT_TIMER_OVF:
        /* Timer overflow → IRQ: light CPU; data=TMA reload value */
        pulse_comp(st, COMP_U1, 0.9f, seq, type);
        break;

    case GB_HW_EVT_APU_WRITE:
        /* APU register write: addr=register, data=value */
        project_address(st, ev->addr, seq, type);
        project_data(st, ev->data, seq, type);
        pulse_net(st, NET_nWR, 0.7f, seq, type, 0);
        pulse_comp(st, COMP_U1, 0.6f, seq, type);
        break;

    case GB_HW_EVT_JOYPAD:
        /* Joypad: data=button state byte; write=pressed */
        project_data(st, ev->data, seq, type);
        pulse_comp(st, COMP_U1, 0.5f, seq, type);
        break;

    case GB_HW_EVT_SERIAL_START:
        /* Serial transfer begin: light up link port connector (J1) and CPU.
         * extra=SC byte; no address/data bus activity (serial is off-bus). */
        pulse_comp(st, 12, 0.9f, seq, type); /* J1 = link port connector */
        pulse_comp(st, COMP_U1, 0.6f, seq, type);
        break;

    case GB_HW_EVT_SERIAL_DONE:
        /* Serial transfer complete: link port dims, data=received byte. */
        project_data(st, ev->data, seq, type);
        pulse_comp(st, 12, 0.7f, seq, type); /* J1 = link port */
        pulse_comp(st, COMP_U1, 0.5f, seq, type);
        break;

    case GB_HW_EVT_MBC_SWITCH:
        /* MBC bank switch: cartridge connector active; address bus = write addr.
         * data=new ROM bank low, extra encodes ROM bank MSB + RAM bank. */
        project_address(st, ev->addr, seq, type);
        pulse_net(st, NET_nWR, 0.8f, seq, type, 0);
        pulse_net(st, NET_nCS,  0.8f, seq, type, 0);
        pulse_comp(st, COMP_P1, 1.0f, seq, type); /* cartridge */
        pulse_comp(st, COMP_U1, 0.5f, seq, type);
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------------------
 * Ring buffer consumer
 * ------------------------------------------------------------------------- */

int hw_activity_consume_trace(HwSchematicActivityState *st,
                               const struct gb_hw_trace *trace,
                               uint64_t *last_seq)
{
    if (!st || !trace || !last_seq) return 0;
    if (!trace->enabled || trace->count == 0) return 0;

    build_lookup();

    int projected = 0;
    uint32_t cap   = GB_HW_TRACE_CAP;
    uint32_t count = trace->count < cap ? trace->count : cap;
    uint32_t head  = trace->head; /* next write position */

    /* Iterate oldest → newest.
     * Oldest event is at index (head - count + cap) % cap. */
    uint32_t start = (head + cap - count) % cap;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start + i) % cap;
        const gb_hw_trace_event *ev = &trace->events[idx];
        if (ev->seq <= *last_seq) continue;
        hw_project_event(st, ev);
        if (ev->seq > *last_seq) *last_seq = ev->seq;
        projected++;
    }
    return projected;
}

/* =========================================================================
 * Fase G — Audit mode
 * ========================================================================= */

/* Net ids referenced in hw_project_event that must exist in hw_net_map.
 * A net_id is "unmapped" when s_net_id_to_map[] == -1 after build_lookup(). */
static bool audit_net_mapped(int net_id)
{
    if (net_id < 0 || net_id >= HW_NET_COUNT) return false;
    return (int8_t)s_net_id_to_map[net_id] >= 0;
}

/* Add a finding if there is still room. */
static void audit_push(HwAuditResult *out, HwAuditCategory cat,
                       const gb_hw_trace_event *ev, int net_id,
                       const char *detail)
{
    if (out->count >= HW_AUDIT_MAX_FINDINGS) return;
    HwAuditFinding *f = &out->findings[out->count++];
    f->category  = cat;
    f->seq       = ev->seq;
    f->timestamp = ev->timestamp;
    f->type      = ev->type;
    f->addr      = ev->addr;
    f->net_id    = net_id;
    /* snprintf is safe: detail[48] is the destination size */
    snprintf(f->detail, sizeof(f->detail), "%s", detail ? detail : "");
    out->cat_count[cat]++;
}

/* ── Address range validation ───────────────────────────────────────────── */

/* Returns true when addr is a plausible target for a CPU read/write.
 * The DMG has 64 KiB flat address space; 0xFEA0–0xFEFF is prohibited but
 * the CPU can still address it (reads return 0xFF or open-bus). */
static bool addr_valid_cpu(uint16_t addr)
{
    (void)addr;
    return true; /* full 16-bit space is reachable by CPU */
}

/* DMA source must be in ROM/SRAM/WRAM/VRAM (not OAM, HRAM, or IO).
 * Allowed: 0x0000–0xDFFF (also Echo 0xE000–0xFDFF mirrors WRAM in DMG). */
static bool addr_valid_dma_src(uint16_t addr)
{
    return addr <= 0xDFFF || (addr >= 0xE000 && addr <= 0xFDFF);
}

/* OAM DMA destination must be FE00–FE9F. */
static bool addr_valid_oam_dst(uint16_t addr)
{
    return addr >= 0xFE00 && addr <= 0xFE9F;
}

/* APU register writes must be in 0xFF10–0xFF3F. */
static bool addr_valid_apu(uint16_t addr)
{
    return addr >= 0xFF10 && addr <= 0xFF3F;
}

/* ── Address nets that hw_project_event writes ───────────────────────────── */
static void audit_check_addr_nets(HwAuditResult *out,
                                   const gb_hw_trace_event *ev)
{
    char buf[48];
    for (int i = 0; i < 16; i++) {
        int nid = ADDR_NET_IDS[i];
        if (!audit_net_mapped(nid)) {
            snprintf(buf, sizeof(buf), "A%d net_id=%d not in map", i, nid);
            audit_push(out, HW_AUDIT_UNMAPPED_NET, ev, nid, buf);
        }
    }
}

static void audit_check_data_nets(HwAuditResult *out,
                                   const gb_hw_trace_event *ev)
{
    char buf[48];
    for (int i = 0; i < 8; i++) {
        int nid = DATA_NET_IDS[i];
        if (!audit_net_mapped(nid)) {
            snprintf(buf, sizeof(buf), "D%d net_id=%d not in map", i, nid);
            audit_push(out, HW_AUDIT_UNMAPPED_NET, ev, nid, buf);
        }
    }
}

static void audit_check_ctrl_nets(HwAuditResult *out,
                                   const gb_hw_trace_event *ev)
{
    static const struct { int nid; const char *name; } ctrls[] = {
        { NET_nRD, "nRD" }, { NET_nWR, "nWR" },
        { NET_nCS, "nCS" }, { NET_nMCS, "nMCS" },
    };
    char buf[48];
    for (int i = 0; i < 4; i++) {
        if (!audit_net_mapped(ctrls[i].nid)) {
            snprintf(buf, sizeof(buf), "%s net_id=%d not in map",
                     ctrls[i].name, ctrls[i].nid);
            audit_push(out, HW_AUDIT_UNMAPPED_NET, ev, ctrls[i].nid, buf);
        }
    }
}

/* ── Per-event audit ─────────────────────────────────────────────────────── */

static void audit_event(HwAuditResult *out, const gb_hw_trace_event *ev,
                         uint64_t prev_seq, int32_t prev_ts,
                         bool *last_was_read, bool *last_was_write,
                         int32_t *last_rw_ts)
{
    char buf[48];

    /* ── SEQ_GAP: seq should increase by 1 each event ── */
    if (prev_seq != 0 && ev->seq > prev_seq + 1) {
        snprintf(buf, sizeof(buf), "seq gap +%llu",
                 (unsigned long long)(ev->seq - prev_seq - 1));
        audit_push(out, HW_AUDIT_SEQ_GAP, ev, -1, buf);
    }

    /* ── BUS_CONFLICT: READ and WRITE at same timestamp ── */
    if (ev->timestamp == *last_rw_ts) {
        bool is_read  = (ev->type == GB_HW_EVT_CPU_READ  ||
                         ev->type == GB_HW_EVT_DMA_READ);
        bool is_write = (ev->type == GB_HW_EVT_CPU_WRITE ||
                         ev->type == GB_HW_EVT_DMA_WRITE ||
                         ev->type == GB_HW_EVT_OAM_DMA);
        if ((is_read && *last_was_write) || (is_write && *last_was_read)) {
            snprintf(buf, sizeof(buf), "RD+WR at T=%d", ev->timestamp);
            audit_push(out, HW_AUDIT_BUS_CONFLICT, ev, -1, buf);
        }
    }

    /* Update last R/W tracking */
    {
        bool is_read  = (ev->type == GB_HW_EVT_CPU_READ  ||
                         ev->type == GB_HW_EVT_DMA_READ);
        bool is_write = (ev->type == GB_HW_EVT_CPU_WRITE ||
                         ev->type == GB_HW_EVT_DMA_WRITE ||
                         ev->type == GB_HW_EVT_OAM_DMA);
        if (is_read || is_write) {
            if (ev->timestamp != *last_rw_ts) {
                *last_was_read  = false;
                *last_was_write = false;
                *last_rw_ts     = ev->timestamp;
            }
            if (is_read)  *last_was_read  = true;
            if (is_write) *last_was_write = true;
        }
    }

    (void)prev_ts;

    /* ── Per-type checks ── */
    switch (ev->type) {

    case GB_HW_EVT_CPU_FETCH:
        audit_check_addr_nets(out, ev);
        audit_check_data_nets(out, ev);
        audit_check_ctrl_nets(out, ev);
        if (!addr_valid_cpu(ev->addr)) {
            snprintf(buf, sizeof(buf), "FETCH addr %04X out of range", ev->addr);
            audit_push(out, HW_AUDIT_BAD_ADDR, ev, -1, buf);
        }
        break;

    case GB_HW_EVT_CPU_READ:
        audit_check_addr_nets(out, ev);
        audit_check_data_nets(out, ev);
        audit_check_ctrl_nets(out, ev);
        break;

    case GB_HW_EVT_CPU_WRITE:
        audit_check_addr_nets(out, ev);
        audit_check_data_nets(out, ev);
        audit_check_ctrl_nets(out, ev);
        break;

    case GB_HW_EVT_DMA_READ:
        audit_check_addr_nets(out, ev);
        audit_check_data_nets(out, ev);
        if (!addr_valid_dma_src(ev->addr)) {
            snprintf(buf, sizeof(buf), "DMA src %04X not in allowed range", ev->addr);
            audit_push(out, HW_AUDIT_BAD_ADDR, ev, -1, buf);
        }
        break;

    case GB_HW_EVT_OAM_DMA:
        audit_check_addr_nets(out, ev);
        audit_check_data_nets(out, ev);
        if (!addr_valid_oam_dst(ev->addr)) {
            snprintf(buf, sizeof(buf), "OAM dst %04X not in FE00-FE9F", ev->addr);
            audit_push(out, HW_AUDIT_BAD_ADDR, ev, -1, buf);
        }
        break;

    case GB_HW_EVT_APU_WRITE:
        audit_check_addr_nets(out, ev);
        audit_check_data_nets(out, ev);
        if (!addr_valid_apu(ev->addr)) {
            snprintf(buf, sizeof(buf), "APU reg %04X not in FF10-FF3F", ev->addr);
            audit_push(out, HW_AUDIT_BAD_ADDR, ev, -1, buf);
        }
        break;

    case GB_HW_EVT_IRQ_REQUEST:
    case GB_HW_EVT_IRQ_ACK:
        /* These only pulse COMP_U1 — no net projection, which is by design.
         * Flag only if no component mapping exists for COMP_U1 (index 10). */
        if (10 >= HW_COMPONENT_COUNT) {
            snprintf(buf, sizeof(buf), "IRQ: COMP_U1 (10) out of range");
            audit_push(out, HW_AUDIT_NO_BUS_PROJ, ev, -1, buf);
        }
        break;

    case GB_HW_EVT_PPU_VBLANK:
    case GB_HW_EVT_PPU_HBLANK:
    case GB_HW_EVT_PPU_MODE:
        /* Only pulse LCD comp (11) and U1 (10). Check both. */
        if (11 >= HW_COMPONENT_COUNT) {
            snprintf(buf, sizeof(buf), "PPU: LCD comp (11) out of range");
            audit_push(out, HW_AUDIT_NO_BUS_PROJ, ev, -1, buf);
        }
        break;

    case GB_HW_EVT_TIMER_OVF:
    case GB_HW_EVT_APU_SAMPLE:
    case GB_HW_EVT_JOYPAD:
        /* Intentionally no bus projection — just comp pulses. Not an error. */
        break;

    case GB_HW_EVT_DMA_WRITE:
        audit_check_data_nets(out, ev);
        break;

    default:
        /* Unknown event type — no projection possible */
        snprintf(buf, sizeof(buf), "unknown event type %d", (int)ev->type);
        audit_push(out, HW_AUDIT_NO_BUS_PROJ, ev, -1, buf);
        break;
    }
}

void hw_audit_reset(HwAuditResult *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
}

void hw_audit_trace(const struct gb_hw_trace *trace,
                    HwAuditResult            *out,
                    uint64_t                 *last_seq_audited)
{
    if (!trace || !out) return;
    build_lookup();

    hw_audit_reset(out);

    if (!trace->enabled || trace->count == 0) return;

    uint32_t cap   = GB_HW_TRACE_CAP;
    uint32_t count = trace->count < cap ? trace->count : cap;
    uint32_t head  = trace->head;
    uint32_t start = (head + cap - count) % cap;

    uint64_t prev_seq  = 0;
    int32_t  prev_ts   = 0;
    bool     last_was_read  = false;
    bool     last_was_write = false;
    int32_t  last_rw_ts     = -1;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start + i) % cap;
        const gb_hw_trace_event *ev = &trace->events[idx];

        if (last_seq_audited && ev->seq <= *last_seq_audited) {
            prev_seq = ev->seq;
            prev_ts  = ev->timestamp;
            continue;
        }

        audit_event(out, ev, prev_seq, prev_ts,
                    &last_was_read, &last_was_write, &last_rw_ts);
        out->total_events_scanned++;

        if (last_seq_audited && ev->seq > *last_seq_audited)
            *last_seq_audited = ev->seq;

        prev_seq = ev->seq;
        prev_ts  = ev->timestamp;
    }
}
