/* sm83_semantic_map.h
 *
 * Maps SM83 netlist net IDs to emulator-visible signals with confidence levels.
 *
 * SEPARATION OF CONCERNS
 * ─────────────────────────────────────────────────────────────────────────────
 * sm83_node_map.h   — VISUAL map: instance name → die position (used for
 *                     overlay rendering; relates to sm83_instances[]).
 *
 * sm83_semantic_map.h — ELECTRICAL map: net_id (index into sm83_nets[]) →
 *                     emulator signal + confidence (used for sim validation
 *                     and mismatch detection; relates to sm83_transistors[]).
 *
 * The two are complementary: the visual map tells you WHERE a bit lives on
 * the die; the semantic map tells you WHICH electrical net carries that bit
 * and how certain that assignment is.
 *
 * NET NAME NOTE
 * ─────────────────────────────────────────────────────────────────────────────
 * sm83_nets[] contains only anonymous names ("net@N").  The net_name field
 * in Sm83NetSemanticEntry stores the INSTANCE name (from sm83_instances[])
 * used to identify the cell whose output transistor drives this net.
 * sm83_semantic_map_init() will NOT find these by sm83_sim_find_net() —
 * net_id must be resolved by transistor tracing (Etapa E).
 * Rails are pre-resolved by SM83_VCC/GND_NET_HEURISTIC and are CONFIRMED.
 *
 * CONFIDENCE LEVELS
 * ─────────────────────────────────────────────────────────────────────────────
 * CONFIRMED   — net_id confirmed by die analysis or heuristic with strong
 *               transistor-terminal ratio (rails only at this stage).
 * PROBABLE    — instance identified in sm83_instances[]; net_id not yet
 *               resolved — requires transistor tracing (Etapa E).
 * PROXY       — net is electrically adjacent to the real signal net (connected
 *               through a buffer/pass transistor); may differ by one inversion.
 * UNKNOWN     — no reliable mapping found yet; net_id is -1 here.
 *
 * All entries with confidence < PROBABLE should be treated as hints only and
 * must NOT be used for emulation decisions.
 */
#ifndef SM83_SEMANTIC_MAP_H
#define SM83_SEMANTIC_MAP_H

#include <stdint.h>
#include "sm83_netlist_sim.h"   /* Sm83NetlistSim — needed by sm83_semantic_audit() */

/* ─────────────────────────────────────────────────────────────────────────────
 * Semantic signal identifiers
 * One entry per bit-width group; individual bits are addressed via the `bit`
 * field in Sm83NetSemanticEntry.
 * ───────────────────────────────────────────────────────────────────────────── */
typedef enum {
    /* Program Counter */
    SM83_SEM_PCL        =  0,   /* PC low byte, bits [0..7] */
    SM83_SEM_PCH        =  1,   /* PC high byte, bits [0..7] */

    /* General-purpose registers */
    SM83_SEM_REG_A      =  2,
    SM83_SEM_REG_B      =  3,
    SM83_SEM_REG_C      =  4,
    SM83_SEM_REG_D      =  5,
    SM83_SEM_REG_E      =  6,
    SM83_SEM_REG_H      =  7,
    SM83_SEM_REG_L      =  8,

    /* Stack Pointer */
    SM83_SEM_SPL        =  9,   /* SP low byte */
    SM83_SEM_SPH        = 10,   /* SP high byte */

    /* Instruction Register */
    SM83_SEM_IR         = 11,   /* Opcode latch, bits [0..7] */

    /* ALU flags — single-bit each */
    SM83_SEM_FLAG_Z     = 12,
    SM83_SEM_FLAG_N     = 13,
    SM83_SEM_FLAG_H     = 14,
    SM83_SEM_FLAG_C     = 15,

    /* Internal buses */
    SM83_SEM_DBUS       = 16,   /* Internal data bus, bits [0..7] */

    /* IDU (Increment/Decrement Unit output) */
    SM83_SEM_IDU        = 17,   /* bits [0..7] */

    /* Power rails — handled separately by the sim, listed here for completeness */
    SM83_SEM_VCC        = 18,
    SM83_SEM_GND        = 19,

    SM83_SEM_COUNT      = 20,
    SM83_SEM_UNKNOWN_SIG = SM83_SEM_COUNT,
} Sm83SemanticSignal;

/* ─────────────────────────────────────────────────────────────────────────────
 * Confidence of a net→signal assignment
 * ───────────────────────────────────────────────────────────────────────────── */
typedef enum {
    SM83_CONF_UNKNOWN   = 0,   /* no reliable mapping */
    SM83_CONF_PROXY     = 1,   /* electrically adjacent, may be inverted */
    SM83_CONF_PROBABLE  = 2,   /* strong heuristic — topology + name */
    SM83_CONF_CONFIRMED = 3,   /* traced or matched by name in netlist */
} Sm83SemanticConfidence;

/* ─────────────────────────────────────────────────────────────────────────────
 * One mapping entry: net_id ↔ (signal, bit)
 * ───────────────────────────────────────────────────────────────────────────── */
typedef struct {
    int                    net_id;       /* index into sm83_nets[]; -1 = not found */
    const char            *net_name;     /* sm83_nets[net_id] if found, else NULL */
    Sm83SemanticSignal     signal;       /* which emulator-visible signal */
    int                    bit;          /* bit within that signal (0 for 1-bit sigs) */
    Sm83SemanticConfidence confidence;
    const char            *note;         /* optional human note (NULL = no note) */
} Sm83NetSemanticEntry;

/* ─────────────────────────────────────────────────────────────────────────────
 * Semantic map table
 *
 * Populated at build time by resolving net names from sm83_nets[].
 * All net_id fields start as -1; sm83_semantic_map_init() fills them in by
 * calling sm83_sim_find_net() for each net_name.
 *
 * Confidence values are set conservatively:
 *   - Named nets found verbatim in sm83_nets[] → PROBABLE
 *     (not CONFIRMED until traced to actual transistor outputs)
 *   - Rails (VCC/GND) → CONFIRMED (resolved by find_power_rails.py analysis)
 *
 * Format: { net_id, net_name, signal, bit, confidence, note }
 * ───────────────────────────────────────────────────────────────────────────── */

/* Initialised by sm83_semantic_map_init() — see below */
extern Sm83NetSemanticEntry sm83_semantic_map[];
extern int                  sm83_semantic_map_count;

/* Fill in all net_id fields using sm83_sim_find_net().
 * Must be called after sm83_sim_init() (which loads the net name table).
 * Safe to call multiple times. */
void sm83_semantic_map_init(void);

/* Look up the semantic entry for a given net index.
 * Returns NULL if net_id is not in the map or confidence < min_confidence. */
const Sm83NetSemanticEntry *sm83_semantic_map_find(int net_id,
                                                    Sm83SemanticConfidence min_confidence);

/* Forward declaration — gb is defined in gb.h */
struct gb;

/* Return the emulator value for a given (signal, bit) pair from gb state.
 * Returns -1 if signal/bit is out of range or gb is NULL. */
int sm83_semantic_emulator_bit(const struct gb *gb,
                                Sm83SemanticSignal signal, int bit);

/* ─────────────────────────────────────────────────────────────────────────────
 * Mismatch record
 *
 * One entry per net where the transistor-level sim disagrees with the
 * emulator.  Produced by sm83_semantic_audit(); consumed by the UI.
 *
 * sim_high == 1  → sim says net is HIGH; emulator says LOW (or vice-versa).
 * emulator_bit   → value from sm83_semantic_emulator_bit() (0 or 1).
 * sim_bit        → 1 if SM83_SIM_IS_HIGH(sim state), 0 if IS_LOW, -1 if unknown.
 * ───────────────────────────────────────────────────────────────────────────── */
typedef struct {
    int                    net_id;
    Sm83SemanticSignal     signal;
    int                    bit;
    Sm83SemanticConfidence confidence;
    int                    emulator_bit; /* 0 or 1 */
    int                    sim_bit;      /* 0 or 1 */
} Sm83DieMismatch;

/* Ring buffer capacity — stores the most recent N mismatches across all
 * sm83_semantic_audit() calls.  Old entries are overwritten. */
#define SM83_MISMATCH_BUF_SIZE 64

/* ─────────────────────────────────────────────────────────────────────────────
 * Mismatch ring buffer (shared, updated by sm83_semantic_audit())
 * ───────────────────────────────────────────────────────────────────────────── */
extern Sm83DieMismatch sm83_mismatch_buf[SM83_MISMATCH_BUF_SIZE];
extern int             sm83_mismatch_count; /* total unique mismatches in last audit */
extern int             sm83_mismatch_head;  /* write head in ring (0..BUF_SIZE-1) */

/* Compare transistor-level sim state against emulator state for every entry
 * in sm83_semantic_map[] with confidence >= min_confidence.
 *
 * For each mismatch the entry is appended to sm83_mismatch_buf[] (ring).
 * sm83_mismatch_count is set to the number of mismatches found this call.
 *
 * Requires:
 *   sm83_semantic_map_init() called (net_ids resolved).
 *   sim step has run (sm83_sim_step() called after seed).
 *   gb != NULL.
 *
 * Returns the number of mismatches found. */
int sm83_semantic_audit(const Sm83NetlistSim        *sim,
                        const struct gb             *gb,
                        Sm83SemanticConfidence       min_confidence);

#endif /* SM83_SEMANTIC_MAP_H */
