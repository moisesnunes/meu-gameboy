/* sm83_netlist_sim.h
 * Experimental transistor-level netlist simulator for the SM83 die.
 *
 * This module is SEPARATE from the emulator core and does NOT affect
 * emulation accuracy, timing, or game compatibility. It runs only when
 * explicitly enabled (sm83_sim_enabled flag) and only while paused or
 * stepping. The authoritative CPU state is always gb->cpu, not this sim.
 *
 * Model (adapted from Perfect6502 / visualz80remix approach):
 *   - One state value per net (SM83_NET_COUNT nets from sm83_netlist_data.h).
 *   - N-transistor: gate HIGH -> connects s1_net <-> s2_net.
 *   - P-transistor: gate LOW  -> connects s1_net <-> s2_net.
 *   - Propagation: iterative relaxation, converges in a few passes.
 *   - Special nets: VCC (always HIGH_STRONG) and GND (always LOW_STRONG).
 *   - CONFLICT detected when HIGH_STRONG and LOW_STRONG meet on same net.
 *
 * Usage:
 *   sm83_sim_init(&sim);             -- allocate and reset
 *   sm83_sim_seed_from_gb(&sim, gb); -- copy known signals from emulator
 *   sm83_sim_step(&sim, 64);         -- run propagation until stable
 *   sm83_sim_net_state(&sim, id);    -- query per-net state for rendering
 *   sm83_sim_net_source(&sim, id);   -- query why a net has that value
 *   sm83_sim_shutdown(&sim);         -- free
 */
#ifndef SM83_NETLIST_SIM_H
#define SM83_NETLIST_SIM_H

#include <stdint.h>
#include <stdbool.h>

struct gb; /* forward — only used in sm83_sim_seed_from_gb */

/* Minimal CPU snapshot for die sim seeding from a historical trace event.
 * Mirrors the snap_* fields of gb_hw_trace_event without requiring debug.h. */
typedef struct {
    uint16_t pc;
    uint8_t  a, b, c, d, e, h, l;
    uint16_t sp;
    uint8_t  flags;   /* (Z<<3)|(N<<2)|(H<<1)|C */
    uint8_t  ir;      /* instruction register */
    uint8_t  dbus;    /* data bus */
    uint8_t  alu_op;  /* GB_VIZ_ALU_* */
    uint8_t  src, dst;/* GB_VIZ_REG_* */
} Sm83CpuSnapshot;

/* -------------------------------------------------------------------------
 * Net logical state
 * Ordered so that (state & SM83_SIM_DRIVEN_MASK) != 0 means "driven".
 * CONFLICT is always shown in the UI as an error.
 * ------------------------------------------------------------------------- */
typedef enum {
    SM83_SIM_UNKNOWN      = 0, /* never resolved */
    SM83_SIM_FLOAT        = 1, /* transistor path exists but no driver */
    SM83_SIM_LOW_WEAK     = 2, /* propagated LOW (not directly driven) */
    SM83_SIM_HIGH_WEAK    = 3, /* propagated HIGH (not directly driven) */
    SM83_SIM_LOW          = 4, /* driven LOW  (seed or rail) */
    SM83_SIM_HIGH         = 5, /* driven HIGH (seed or rail) */
    SM83_SIM_CONFLICT     = 6, /* HIGH and LOW drivers on same net — error */
    SM83_SIM_STATE_COUNT  = 7,
} Sm83SimState;

/* True when state represents a definite logic level (driven or weak) */
#define SM83_SIM_IS_HIGH(s)    ((s) == SM83_SIM_HIGH || (s) == SM83_SIM_HIGH_WEAK)
#define SM83_SIM_IS_LOW(s)     ((s) == SM83_SIM_LOW  || (s) == SM83_SIM_LOW_WEAK)
#define SM83_SIM_IS_DRIVEN(s)  ((s) == SM83_SIM_HIGH || (s) == SM83_SIM_LOW)
#define SM83_SIM_IS_RESOLVED(s) ((s) != SM83_SIM_UNKNOWN && (s) != SM83_SIM_FLOAT)

/* -------------------------------------------------------------------------
 * Net value origin — stored in a parallel array so the UI can explain why.
 * ------------------------------------------------------------------------- */
typedef enum {
    SM83_SRC_NONE       = 0, /* UNKNOWN or FLOAT — no source */
    SM83_SRC_RAIL       = 1, /* anchored by VCC/GND */
    SM83_SRC_SEED       = 2, /* directly seeded from gb->cpu state */
    SM83_SRC_PROP       = 3, /* propagated through conducting transistor */
    SM83_SRC_CONFLICT   = 4, /* conflict: two drivers disagree */
} Sm83NetSource;

/* -------------------------------------------------------------------------
 * How VCC/GND rail indices were resolved
 * ------------------------------------------------------------------------- */
typedef enum {
    SM83_RAILS_MISSING    = 0, /* no rails found — sim cannot propagate */
    SM83_RAILS_NAMED      = 1, /* resolved by net name (vcc/gnd/vdd/vss) */
    SM83_RAILS_HEURISTIC  = 2, /* resolved by connectivity heuristic */
    SM83_RAILS_MANUAL     = 3, /* overridden by user */
} Sm83RailSource;

/* -------------------------------------------------------------------------
 * Simulator state
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t        *net_state;      /* SM83_NET_COUNT entries, Sm83SimState */
    uint8_t        *net_source;     /* SM83_NET_COUNT entries, Sm83NetSource */
    bool            initialized;
    bool            enabled;        /* false = sim dormant, panel shows nothing */
    int             vcc_net;        /* index of VCC net (-1 if not found) */
    int             gnd_net;        /* index of GND net (-1 if not found) */
    int             iterations;     /* propagation passes used in last step */
    int             conflict_count; /* nets in CONFLICT state after last step */
    bool            rails_found;
    Sm83RailSource  rail_source;
} Sm83NetlistSim;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Allocate state arrays and find VCC/GND nets. Call once. */
void sm83_sim_init(Sm83NetlistSim *sim);

/* Free state arrays. */
void sm83_sim_shutdown(Sm83NetlistSim *sim);

/* Reset all nets to UNKNOWN; re-anchor VCC/GND. */
void sm83_sim_reset(Sm83NetlistSim *sim);

/* Seed known signals from the emulator's CPU state.
 * Only sets nets that have a named mapping in sm83_node_map.h.
 * Does not run propagation — call sm83_sim_step() after. */
void sm83_sim_seed_from_gb(Sm83NetlistSim *sim, const struct gb *gb);

/* Seed from a CPU snapshot (built from a historical FETCH trace event).
 * Allows replay: select any event in the die viewer timeline and seed the sim
 * with the CPU register snapshot captured at that instruction boundary. */
void sm83_sim_seed_from_snapshot(Sm83NetlistSim *sim, const Sm83CpuSnapshot *snap);

/* Phase-aware partial seed — does NOT reset; updates only the nets that change
 * during a specific micro-event type.  ev_type is a gb_hw_trace_event_type value:
 *   CPU_READ / CPU_WRITE : seeds DBUS (ev_data8), ABUS (ev_addr)
 *   CPU_ALU              : seeds flag nets (ev_flags_after), snap_a into reg_a nets
 *   CPU_WRITEBACK        : seeds the destination register bits (ev_dst, ev_data8/addr)
 *   CPU_FETCH            : equivalent to seed_from_snapshot (full register seed)
 * All other event types are no-ops.  Call sm83_sim_step() after to propagate. */
void sm83_sim_phase_seed(Sm83NetlistSim *sim,
                         int              ev_type,     /* gb_hw_trace_event_type */
                         uint16_t         ev_addr,
                         uint8_t          ev_data8,
                         uint8_t          ev_extra,    /* dst reg for WRITEBACK */
                         uint8_t          ev_flags,    /* flags_after for ALU */
                         const Sm83CpuSnapshot *snap); /* may be NULL */

/* Run propagation until stable or max_iters reached.
 * Returns number of iterations used. Updates conflict_count. */
int sm83_sim_step(Sm83NetlistSim *sim, int max_iters);

/* Return the logical state of a single net by index. */
Sm83SimState sm83_sim_net_state(const Sm83NetlistSim *sim, int net_id);

/* Return the value origin of a single net by index. */
Sm83NetSource sm83_sim_net_source(const Sm83NetlistSim *sim, int net_id);

/* Find the net index for a given net name string (linear scan — for init only). */
int sm83_sim_find_net(const char *name);

#endif /* SM83_NETLIST_SIM_H */
