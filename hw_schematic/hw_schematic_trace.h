/* hw_schematic_trace.h — Fase D: event projection layer.
 *
 * Translates gb_hw_trace_event records into per-net and per-component
 * activity intensities that the UI renders as wire/component highlights.
 *
 * Design principles:
 *   - No emulation state modified; purely metadata for visualization.
 *   - Hot path (decay tick) is O(nets+comps), called once per frame.
 *   - Projection (event → nets) is O(net_map_count), called per event.
 *   - Confidence is preserved: nets with PROXY/UNKNOWN confidence get
 *     lower intensity than CONFIRMED nets.
 */
#ifndef HW_SCHEMATIC_TRACE_H
#define HW_SCHEMATIC_TRACE_H

#include <stdint.h>
#include <stdbool.h>
#include "hw_schematic_data.h"
#include "hw_schematic_map.h"
#include "../debug.h"

/* -------------------------------------------------------------------------
 * Activity state — one float per net/component, 0..1
 * Allocated statically; safe to zero-init.
 * ------------------------------------------------------------------------- */
typedef struct {
    float    net_fade[HW_NET_COUNT];
    float    comp_fade[HW_COMPONENT_COUNT];

    /* seq of the last event that touched each net/component */
    uint64_t net_event_seq[HW_NET_COUNT];
    uint64_t comp_event_seq[HW_COMPONENT_COUNT];

    /* type of the last event (for colouring) */
    gb_hw_trace_event_type net_last_type[HW_NET_COUNT];
    gb_hw_trace_event_type comp_last_type[HW_COMPONENT_COUNT];

    /* logic level inferred from the last event (0/1/-1=unknown) */
    int8_t   net_level[HW_NET_COUNT];
} HwSchematicActivityState;

/* -------------------------------------------------------------------------
 * Lifetime helpers
 * ------------------------------------------------------------------------- */

/* Zero out all fades and metadata. Call once at init. */
void hw_activity_reset(HwSchematicActivityState *st);

/* Decay all fades by dt seconds. Call once per UI frame.
 * decay_rate: fade decreases at this many units/second (default ~3.0). */
void hw_activity_tick(HwSchematicActivityState *st, float dt, float decay_rate);

/* -------------------------------------------------------------------------
 * Projection — call for each new event coming out of the ring buffer.
 *
 * Projects the event onto nets and components:
 *   - CPU_READ/WRITE/FETCH: address bus bits, data bus bits, RD/WR strobes,
 *     CPU component, target component by address range.
 *   - IRQ_REQUEST/ACK: IRQ net, CPU component.
 *   - DMA_READ/WRITE: address bus, data bus, DMA proxy components.
 *   - PPU_MODE: LCD component.
 *   - APU_SAMPLE: audio component.
 * ------------------------------------------------------------------------- */
void hw_project_event(HwSchematicActivityState *st, const gb_hw_trace_event *ev);

/* -------------------------------------------------------------------------
 * Convenience: consume all new events from the ring buffer since last_seq.
 * Updates *last_seq to the highest seq consumed.
 * Returns number of events projected.
 * ------------------------------------------------------------------------- */
int hw_activity_consume_trace(HwSchematicActivityState *st,
                               const struct gb_hw_trace *trace,
                               uint64_t *last_seq);

/* =========================================================================
 * Fase G — Audit mode
 *
 * hw_audit_trace() scans the ring buffer and classifies anomalies into
 * five categories.  Results are stable across calls (no allocation).
 * ========================================================================= */

#define HW_AUDIT_MAX_FINDINGS 64

typedef enum {
    HW_AUDIT_UNMAPPED_NET = 0, /* net_id referenced but not in hw_net_map     */
    HW_AUDIT_NO_BUS_PROJ,      /* event type touches no net at all            */
    HW_AUDIT_BAD_ADDR,         /* address out of valid DMG range for this type */
    HW_AUDIT_BUS_CONFLICT,     /* READ and WRITE at the same timestamp        */
    HW_AUDIT_SEQ_GAP,          /* seq jumped by more than expected            */
} HwAuditCategory;

static const char *const HW_AUDIT_CAT_NAMES[] = {
    "UNMAPPED_NET",
    "NO_BUS_PROJ",
    "BAD_ADDR",
    "BUS_CONFLICT",
    "SEQ_GAP",
};

typedef struct {
    HwAuditCategory category;
    uint64_t        seq;       /* sequence number of offending event          */
    int32_t         timestamp; /* emulator timestamp                          */
    gb_hw_trace_event_type type; /* event type                               */
    uint16_t        addr;      /* address (if applicable)                     */
    int             net_id;    /* unmapped net_id (HW_AUDIT_UNMAPPED_NET)     */
    char            detail[48];/* human-readable detail string                */
} HwAuditFinding;

typedef struct {
    HwAuditFinding findings[HW_AUDIT_MAX_FINDINGS];
    int            count;
    int            total_events_scanned;
    /* per-category counts (same order as HwAuditCategory) */
    int            cat_count[5];
} HwAuditResult;

/* Scan the ring buffer and populate *out.
 * Pass last_seq_audited to avoid re-auditing old events; updated on return.
 * Pass NULL for last_seq_audited to audit everything. */
void hw_audit_trace(const struct gb_hw_trace *trace,
                    HwAuditResult            *out,
                    uint64_t                 *last_seq_audited);

/* Clear a result struct (zero findings). */
void hw_audit_reset(HwAuditResult *out);

#endif /* HW_SCHEMATIC_TRACE_H */
