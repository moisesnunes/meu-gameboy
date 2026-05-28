/* sm83_semantic_map.c -- electrical net → emulator signal mapping */
#include "sm83_semantic_map.h"
#include "sm83_netlist_sim.h"   /* sm83_sim_find_net */
#include "sm83_netlist_data.h"  /* SM83_VCC/GND_NET_HEURISTIC + transistors/instances */
#include "gb.h"
#include <stddef.h>
#include <string.h>
#include <math.h>

/* ─────────────────────────────────────────────────────────────────────────────
 * Static table
 *
 * net_id is -1 until sm83_semantic_map_init() resolves it.
 * Ordering: signal group, then bit ascending.
 *
 * Confidence rationale:
 *   PROBABLE  — net name found in sm83_nets[] via string match.  We cannot
 *               be CONFIRMED until the net is traced back to the actual latch
 *               output transistors (future Etapa E work).
 *   CONFIRMED — power rails only, confirmed by find_power_rails.py analysis
 *               (GND ratio 219:1, VCC ratio 14:1) and sim cross-check.
 * ───────────────────────────────────────────────────────────────────────────── */

#define PROBABLE  SM83_CONF_PROBABLE
#define CONFIRMED SM83_CONF_CONFIRMED
#define UNK       SM83_CONF_UNKNOWN

Sm83NetSemanticEntry sm83_semantic_map[] = {
    /* net_id  net_name            signal           bit  conf     note */

    /* ── PC low byte ── */
    { -1, "reg_pcl[0]", SM83_SEM_PCL, 0, PROBABLE, NULL },
    { -1, "reg_pcl[1]", SM83_SEM_PCL, 1, PROBABLE, NULL },
    { -1, "reg_pcl[2]", SM83_SEM_PCL, 2, PROBABLE, NULL },
    { -1, "reg_pcl[3]", SM83_SEM_PCL, 3, PROBABLE, NULL },
    { -1, "reg_pcl[4]", SM83_SEM_PCL, 4, PROBABLE, NULL },
    { -1, "reg_pcl[5]", SM83_SEM_PCL, 5, PROBABLE, NULL },
    { -1, "reg_pcl[6]", SM83_SEM_PCL, 6, PROBABLE, NULL },
    { -1, "reg_pcl[7]", SM83_SEM_PCL, 7, PROBABLE, NULL },

    /* ── PC high byte ── */
    { -1, "reg_pch[0]", SM83_SEM_PCH, 0, PROBABLE, NULL },
    { -1, "reg_pch[1]", SM83_SEM_PCH, 1, PROBABLE, NULL },
    { -1, "reg_pch[2]", SM83_SEM_PCH, 2, PROBABLE, NULL },
    { -1, "reg_pch[3]", SM83_SEM_PCH, 3, PROBABLE, NULL },
    { -1, "reg_pch[4]", SM83_SEM_PCH, 4, PROBABLE, NULL },
    { -1, "reg_pch[5]", SM83_SEM_PCH, 5, PROBABLE, NULL },
    { -1, "reg_pch[6]", SM83_SEM_PCH, 6, PROBABLE, NULL },
    { -1, "reg_pch[7]", SM83_SEM_PCH, 7, PROBABLE, NULL },

    /* ── Accumulator A ── */
    { -1, "reg_a[0]", SM83_SEM_REG_A, 0, PROBABLE, NULL },
    { -1, "reg_a[1]", SM83_SEM_REG_A, 1, PROBABLE, NULL },
    { -1, "reg_a[2]", SM83_SEM_REG_A, 2, PROBABLE, NULL },
    { -1, "reg_a[3]", SM83_SEM_REG_A, 3, PROBABLE, NULL },
    { -1, "reg_a[4]", SM83_SEM_REG_A, 4, PROBABLE, NULL },
    { -1, "reg_a[5]", SM83_SEM_REG_A, 5, PROBABLE, NULL },
    { -1, "reg_a[6]", SM83_SEM_REG_A, 6, PROBABLE, NULL },
    { -1, "reg_a[7]", SM83_SEM_REG_A, 7, PROBABLE, NULL },

    /* ── Register B ── */
    { -1, "reg_b[0]", SM83_SEM_REG_B, 0, PROBABLE, NULL },
    { -1, "reg_b[1]", SM83_SEM_REG_B, 1, PROBABLE, NULL },
    { -1, "reg_b[2]", SM83_SEM_REG_B, 2, PROBABLE, NULL },
    { -1, "reg_b[3]", SM83_SEM_REG_B, 3, PROBABLE, NULL },
    { -1, "reg_b[4]", SM83_SEM_REG_B, 4, PROBABLE, NULL },
    { -1, "reg_b[5]", SM83_SEM_REG_B, 5, PROBABLE, NULL },
    { -1, "reg_b[6]", SM83_SEM_REG_B, 6, PROBABLE, NULL },
    { -1, "reg_b[7]", SM83_SEM_REG_B, 7, PROBABLE, NULL },

    /* ── Register C ── */
    { -1, "reg_c[0]", SM83_SEM_REG_C, 0, PROBABLE, NULL },
    { -1, "reg_c[1]", SM83_SEM_REG_C, 1, PROBABLE, NULL },
    { -1, "reg_c[2]", SM83_SEM_REG_C, 2, PROBABLE, NULL },
    { -1, "reg_c[3]", SM83_SEM_REG_C, 3, PROBABLE, NULL },
    { -1, "reg_c[4]", SM83_SEM_REG_C, 4, PROBABLE, NULL },
    { -1, "reg_c[5]", SM83_SEM_REG_C, 5, PROBABLE, NULL },
    { -1, "reg_c[6]", SM83_SEM_REG_C, 6, PROBABLE, NULL },
    { -1, "reg_c[7]", SM83_SEM_REG_C, 7, PROBABLE, NULL },

    /* ── Register D ── */
    { -1, "reg_d[0]", SM83_SEM_REG_D, 0, PROBABLE, NULL },
    { -1, "reg_d[1]", SM83_SEM_REG_D, 1, PROBABLE, NULL },
    { -1, "reg_d[2]", SM83_SEM_REG_D, 2, PROBABLE, NULL },
    { -1, "reg_d[3]", SM83_SEM_REG_D, 3, PROBABLE, NULL },
    { -1, "reg_d[4]", SM83_SEM_REG_D, 4, PROBABLE, NULL },
    { -1, "reg_d[5]", SM83_SEM_REG_D, 5, PROBABLE, NULL },
    { -1, "reg_d[6]", SM83_SEM_REG_D, 6, PROBABLE, NULL },
    { -1, "reg_d[7]", SM83_SEM_REG_D, 7, PROBABLE, NULL },

    /* ── Register E ── */
    { -1, "reg_e[0]", SM83_SEM_REG_E, 0, PROBABLE, NULL },
    { -1, "reg_e[1]", SM83_SEM_REG_E, 1, PROBABLE, NULL },
    { -1, "reg_e[2]", SM83_SEM_REG_E, 2, PROBABLE, NULL },
    { -1, "reg_e[3]", SM83_SEM_REG_E, 3, PROBABLE, NULL },
    { -1, "reg_e[4]", SM83_SEM_REG_E, 4, PROBABLE, NULL },
    { -1, "reg_e[5]", SM83_SEM_REG_E, 5, PROBABLE, NULL },
    { -1, "reg_e[6]", SM83_SEM_REG_E, 6, PROBABLE, NULL },
    { -1, "reg_e[7]", SM83_SEM_REG_E, 7, PROBABLE, NULL },

    /* ── Register H ── */
    { -1, "reg_h[0]", SM83_SEM_REG_H, 0, PROBABLE, NULL },
    { -1, "reg_h[1]", SM83_SEM_REG_H, 1, PROBABLE, NULL },
    { -1, "reg_h[2]", SM83_SEM_REG_H, 2, PROBABLE, NULL },
    { -1, "reg_h[3]", SM83_SEM_REG_H, 3, PROBABLE, NULL },
    { -1, "reg_h[4]", SM83_SEM_REG_H, 4, PROBABLE, NULL },
    { -1, "reg_h[5]", SM83_SEM_REG_H, 5, PROBABLE, NULL },
    { -1, "reg_h[6]", SM83_SEM_REG_H, 6, PROBABLE, NULL },
    { -1, "reg_h[7]", SM83_SEM_REG_H, 7, PROBABLE, NULL },

    /* ── Register L ── */
    { -1, "reg_l[0]", SM83_SEM_REG_L, 0, PROBABLE, NULL },
    { -1, "reg_l[1]", SM83_SEM_REG_L, 1, PROBABLE, NULL },
    { -1, "reg_l[2]", SM83_SEM_REG_L, 2, PROBABLE, NULL },
    { -1, "reg_l[3]", SM83_SEM_REG_L, 3, PROBABLE, NULL },
    { -1, "reg_l[4]", SM83_SEM_REG_L, 4, PROBABLE, NULL },
    { -1, "reg_l[5]", SM83_SEM_REG_L, 5, PROBABLE, NULL },
    { -1, "reg_l[6]", SM83_SEM_REG_L, 6, PROBABLE, NULL },
    { -1, "reg_l[7]", SM83_SEM_REG_L, 7, PROBABLE, NULL },

    /* ── Stack Pointer low byte ── */
    { -1, "reg_spl[0]", SM83_SEM_SPL, 0, PROBABLE, NULL },
    { -1, "reg_spl[1]", SM83_SEM_SPL, 1, PROBABLE, NULL },
    { -1, "reg_spl[2]", SM83_SEM_SPL, 2, PROBABLE, NULL },
    { -1, "reg_spl[3]", SM83_SEM_SPL, 3, PROBABLE, NULL },
    { -1, "reg_spl[4]", SM83_SEM_SPL, 4, PROBABLE, NULL },
    { -1, "reg_spl[5]", SM83_SEM_SPL, 5, PROBABLE, NULL },
    { -1, "reg_spl[6]", SM83_SEM_SPL, 6, PROBABLE, NULL },
    { -1, "reg_spl[7]", SM83_SEM_SPL, 7, PROBABLE, NULL },

    /* ── Stack Pointer high byte ── */
    { -1, "reg_sph[0]", SM83_SEM_SPH, 0, PROBABLE, NULL },
    { -1, "reg_sph[1]", SM83_SEM_SPH, 1, PROBABLE, NULL },
    { -1, "reg_sph[2]", SM83_SEM_SPH, 2, PROBABLE, NULL },
    { -1, "reg_sph[3]", SM83_SEM_SPH, 3, PROBABLE, NULL },
    { -1, "reg_sph[4]", SM83_SEM_SPH, 4, PROBABLE, NULL },
    { -1, "reg_sph[5]", SM83_SEM_SPH, 5, PROBABLE, NULL },
    { -1, "reg_sph[6]", SM83_SEM_SPH, 6, PROBABLE, NULL },
    { -1, "reg_sph[7]", SM83_SEM_SPH, 7, PROBABLE, NULL },

    /* ── Instruction Register ── */
    { -1, "reg_ir[0]", SM83_SEM_IR, 0, PROBABLE, NULL },
    { -1, "reg_ir[1]", SM83_SEM_IR, 1, PROBABLE, NULL },
    { -1, "reg_ir[2]", SM83_SEM_IR, 2, PROBABLE, NULL },
    { -1, "reg_ir[3]", SM83_SEM_IR, 3, PROBABLE, NULL },
    { -1, "reg_ir[4]", SM83_SEM_IR, 4, PROBABLE, NULL },
    { -1, "reg_ir[5]", SM83_SEM_IR, 5, PROBABLE, NULL },
    { -1, "reg_ir[6]", SM83_SEM_IR, 6, PROBABLE, NULL },
    { -1, "reg_ir[7]", SM83_SEM_IR, 7, PROBABLE, NULL },

    /* ── ALU flags ── */
    { -1, "flag_z", SM83_SEM_FLAG_Z, 0, PROBABLE, NULL },
    { -1, "flag_n", SM83_SEM_FLAG_N, 0, PROBABLE, NULL },
    { -1, "flag_h", SM83_SEM_FLAG_H, 0, PROBABLE, NULL },
    { -1, "flag_c", SM83_SEM_FLAG_C, 0, PROBABLE, NULL },

    /* ── Internal data bus ── */
    { -1, "dbus_bridge[0]", SM83_SEM_DBUS, 0, PROBABLE, NULL },
    { -1, "dbus_bridge[1]", SM83_SEM_DBUS, 1, PROBABLE, NULL },
    { -1, "dbus_bridge[2]", SM83_SEM_DBUS, 2, PROBABLE, NULL },
    { -1, "dbus_bridge[3]", SM83_SEM_DBUS, 3, PROBABLE, NULL },
    { -1, "dbus_bridge[4]", SM83_SEM_DBUS, 4, PROBABLE, NULL },
    { -1, "dbus_bridge[5]", SM83_SEM_DBUS, 5, PROBABLE, NULL },
    { -1, "dbus_bridge[6]", SM83_SEM_DBUS, 6, PROBABLE, NULL },
    { -1, "dbus_bridge[7]", SM83_SEM_DBUS, 7, PROBABLE, NULL },

    /* ── IDU output ── */
    { -1, "idu[0]", SM83_SEM_IDU, 0, PROBABLE, NULL },
    { -1, "idu[1]", SM83_SEM_IDU, 1, PROBABLE, NULL },
    { -1, "idu[2]", SM83_SEM_IDU, 2, PROBABLE, NULL },
    { -1, "idu[3]", SM83_SEM_IDU, 3, PROBABLE, NULL },
    { -1, "idu[4]", SM83_SEM_IDU, 4, PROBABLE, NULL },
    { -1, "idu[5]", SM83_SEM_IDU, 5, PROBABLE, NULL },
    { -1, "idu[6]", SM83_SEM_IDU, 6, PROBABLE, NULL },
    { -1, "idu[7]", SM83_SEM_IDU, 7, PROBABLE, NULL },

    /* ── Power rails — CONFIRMED by heuristic analysis ── */
    { SM83_VCC_NET_HEURISTIC, NULL, SM83_SEM_VCC, 0, CONFIRMED,
      "net@28 p-term:n-term ratio 14:1" },
    { SM83_GND_NET_HEURISTIC, NULL, SM83_SEM_GND, 0, CONFIRMED,
      "net@277 n-term:p-term ratio 219:1" },
};

int sm83_semantic_map_count =
    (int)(sizeof(sm83_semantic_map) / sizeof(sm83_semantic_map[0]));

/* ─────────────────────────────────────────────────────────────────────────────
 * Spatial tracing helpers
 *
 * sm83_nets[] contains only "net@N" anonymous names — there are no semantic
 * names in the electrical netlist.  We resolve net_ids by finding the
 * transistor s1/s2 terminal that appears most frequently near each named
 * instance in sm83_instances[].
 *
 * The DFF cells that implement registers have their output transistors
 * clustered within ~50 Electric-VLSI units of the cell centre.  The net
 * that appears most as a shared s1/s2 terminal in that neighbourhood is
 * the Q-output of the cell.
 * ───────────────────────────────────────────────────────────────────────────── */

#define TRACE_RADIUS 30.0f   /* Electric VLSI units — one DFF cell, avoids cross-register overlap */
#define TRACE_MIN_HITS 2     /* below this → PROXY confidence */

/* Count how often each net appears as s1/s2 within radius of (cx,cy).
 * Fills pairs[]/n_pairs; returns number of unique nets found. */
typedef struct { int id; int count; } NetPair;

static int count_nets_near(float cx, float cy, NetPair *pairs, int max_pairs)
{
    int n = 0;
    for (int i = 0; i < SM83_TRANSISTOR_COUNT; i++) {
        const Sm83Transistor *t = &sm83_transistors[i];
        if (fabsf(t->x - cx) > TRACE_RADIUS || fabsf(t->y - cy) > TRACE_RADIUS)
            continue;
        int nets[2] = { t->s1_net, t->s2_net };
        for (int k = 0; k < 2; k++) {
            int nid = nets[k];
            if (nid < 0 || nid >= SM83_NET_COUNT) continue;
            int found = 0;
            for (int j = 0; j < n; j++) {
                if (pairs[j].id == nid) { pairs[j].count++; found = 1; break; }
            }
            if (!found && n < max_pairs) {
                pairs[n].id    = nid;
                pairs[n].count = 1;
                n++;
            }
        }
    }
    return n;
}

/* Returns the most frequent net_id and its hit count near (cx,cy).
 * Excludes net_ids in the exclude[] array of length n_excl (to break ties
 * caused by shared bus nets that appear in multiple adjacent cells). */
static int spatial_best_net(float cx, float cy, int *out_hits)
{
    NetPair pairs[128];
    int n = count_nets_near(cx, cy, pairs, 128);

    int best_id   = -1;
    int best_hits = 0;
    for (int j = 0; j < n; j++) {
        if (pairs[j].count > best_hits) {
            best_hits = pairs[j].count;
            best_id   = pairs[j].id;
        }
    }

    if (out_hits) *out_hits = best_hits;
    return best_id;
}

/* Same as spatial_best_net but skips any net in already_used[] (length nu).
 * Used for single-bit signals (flags) where two adjacent cells share the
 * most-common net. */
static int spatial_best_net_excl(float cx, float cy,
                                  const int *excl, int n_excl, int *out_hits)
{
    NetPair pairs[128];
    int n = count_nets_near(cx, cy, pairs, 128);

    int best_id   = -1;
    int best_hits = 0;
    for (int j = 0; j < n; j++) {
        /* Skip excluded nets */
        int skip = 0;
        for (int k = 0; k < n_excl; k++) {
            if (pairs[j].id == excl[k]) { skip = 1; break; }
        }
        if (skip) continue;
        if (pairs[j].count > best_hits) {
            best_hits = pairs[j].count;
            best_id   = pairs[j].id;
        }
    }

    if (out_hits) *out_hits = best_hits;
    return best_id;
}

/* Find the sm83_instances[] entry whose name matches inst_name exactly.
 * Returns the raw (x, y) coordinates.  Returns false if not found. */
static int find_instance_pos(const char *inst_name, float *ox, float *oy)
{
    for (int i = 0; i < SM83_INSTANCE_COUNT; i++) {
        if (sm83_instances[i].name && strcmp(sm83_instances[i].name, inst_name) == 0) {
            *ox = sm83_instances[i].x;
            *oy = sm83_instances[i].y;
            return 1;
        }
    }
    return 0;
}

void sm83_semantic_map_init(void)
{
    /* Track which net_ids have been assigned to which signals so far.
     * Used to break ties for adjacent single-bit cells (flags, IR bits)
     * that share a common bus net. */
    int assigned_nets[128];
    int assigned_sigs[128];
    int n_assigned = 0;

    for (int i = 0; i < sm83_semantic_map_count; i++) {
        Sm83NetSemanticEntry *e = &sm83_semantic_map[i];

        /* Rails are pre-baked with known net_ids — just fill in the name string */
        if (e->signal == SM83_SEM_VCC || e->signal == SM83_SEM_GND) {
            if (e->net_id >= 0 && e->net_id < SM83_NET_COUNT)
                e->net_name = sm83_nets[e->net_id];
            continue;
        }

        if (e->net_name == NULL) continue;

        /* Resolve via spatial tracing: find the instance, then the best net */
        float ix, iy;
        if (!find_instance_pos(e->net_name, &ix, &iy)) {
            if (e->confidence > SM83_CONF_PROXY)
                e->confidence = SM83_CONF_PROXY;
            continue;
        }

        /* Collect nets already assigned to *different* signals — exclude them
         * so cross-register bus nets don't dominate the result. */
        int excl[128];
        int n_excl = 0;
        for (int j = 0; j < n_assigned && n_excl < 127; j++) {
            if (assigned_sigs[j] != (int)e->signal)
                excl[n_excl++] = assigned_nets[j];
        }

        int hits = 0;
        int nid;
        if (n_excl > 0)
            nid = spatial_best_net_excl(ix, iy, excl, n_excl, &hits);
        else
            nid = spatial_best_net(ix, iy, &hits);

        /* If exclusion left us with nothing, fall back to unrestricted search */
        if (nid < 0)
            nid = spatial_best_net(ix, iy, &hits);

        e->net_id = nid;
        if (nid < 0) {
            e->confidence = SM83_CONF_UNKNOWN;
        } else if (hits < TRACE_MIN_HITS) {
            e->confidence = SM83_CONF_PROXY;
        }
        /* else keep PROBABLE from table initialisation */

        /* Record assignment */
        if (nid >= 0 && n_assigned < 128) {
            assigned_nets[n_assigned] = nid;
            assigned_sigs[n_assigned] = (int)e->signal;
            n_assigned++;
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────────
 * Mismatch ring buffer
 * ───────────────────────────────────────────────────────────────────────────── */

Sm83DieMismatch sm83_mismatch_buf[SM83_MISMATCH_BUF_SIZE];
int             sm83_mismatch_count = 0;
int             sm83_mismatch_head  = 0;

int sm83_semantic_audit(const Sm83NetlistSim        *sim,
                        const struct gb             *gb,
                        Sm83SemanticConfidence       min_confidence)
{
    if (!sim || !sim->initialized || !gb) return 0;

    int found = 0;
    sm83_mismatch_count = 0;

    for (int i = 0; i < sm83_semantic_map_count; i++) {
        const Sm83NetSemanticEntry *e = &sm83_semantic_map[i];

        if (e->confidence < min_confidence) continue;
        if (e->net_id < 0 || e->net_id >= SM83_NET_COUNT) continue;

        int emu_bit = sm83_semantic_emulator_bit(gb, e->signal, e->bit);
        if (emu_bit < 0) continue; /* IDU or unmapped — skip */

        Sm83SimState st = sm83_sim_net_state(sim, e->net_id);
        int sim_bit;
        if (SM83_SIM_IS_HIGH(st))
            sim_bit = 1;
        else if (SM83_SIM_IS_LOW(st))
            sim_bit = 0;
        else
            continue; /* UNKNOWN/FLOAT — sim has no opinion, skip */

        if (sim_bit == emu_bit) continue; /* match — no mismatch */

        /* Record mismatch */
        Sm83DieMismatch *m = &sm83_mismatch_buf[sm83_mismatch_head];
        m->net_id       = e->net_id;
        m->signal       = e->signal;
        m->bit          = e->bit;
        m->confidence   = e->confidence;
        m->emulator_bit = emu_bit;
        m->sim_bit      = sim_bit;

        sm83_mismatch_head = (sm83_mismatch_head + 1) & (SM83_MISMATCH_BUF_SIZE - 1);
        found++;
    }

    sm83_mismatch_count = found;
    return found;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * sm83_semantic_map_find
 * ───────────────────────────────────────────────────────────────────────────── */
const Sm83NetSemanticEntry *sm83_semantic_map_find(int net_id,
                                                    Sm83SemanticConfidence min_confidence)
{
    if (net_id < 0) return NULL;
    for (int i = 0; i < sm83_semantic_map_count; i++) {
        const Sm83NetSemanticEntry *e = &sm83_semantic_map[i];
        if (e->net_id == net_id && e->confidence >= min_confidence)
            return e;
    }
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────────────────
 * sm83_semantic_emulator_bit
 * Returns the current emulator value (0 or 1) for (signal, bit).
 * Returns -1 on invalid input.
 * ───────────────────────────────────────────────────────────────────────────── */
int sm83_semantic_emulator_bit(const struct gb *gb,
                                Sm83SemanticSignal signal, int bit)
{
    if (!gb || bit < 0) return -1;

    switch (signal) {
    case SM83_SEM_PCL:    if (bit > 7) return -1; return (gb->cpu.pc >> bit)       & 1;
    case SM83_SEM_PCH:    if (bit > 7) return -1; return (gb->cpu.pc >> (8 + bit)) & 1;
    case SM83_SEM_REG_A:  if (bit > 7) return -1; return (gb->cpu.a  >> bit)       & 1;
    case SM83_SEM_REG_B:  if (bit > 7) return -1; return (gb->cpu.b  >> bit)       & 1;
    case SM83_SEM_REG_C:  if (bit > 7) return -1; return (gb->cpu.c  >> bit)       & 1;
    case SM83_SEM_REG_D:  if (bit > 7) return -1; return (gb->cpu.d  >> bit)       & 1;
    case SM83_SEM_REG_E:  if (bit > 7) return -1; return (gb->cpu.e  >> bit)       & 1;
    case SM83_SEM_REG_H:  if (bit > 7) return -1; return (gb->cpu.h  >> bit)       & 1;
    case SM83_SEM_REG_L:  if (bit > 7) return -1; return (gb->cpu.l  >> bit)       & 1;
    case SM83_SEM_SPL:    if (bit > 7) return -1; return (gb->cpu.sp >> bit)       & 1;
    case SM83_SEM_SPH:    if (bit > 7) return -1; return (gb->cpu.sp >> (8 + bit)) & 1;
    case SM83_SEM_IR:     if (bit > 7) return -1; return (gb->debug.cpu_viz.opcode >> bit) & 1;
    case SM83_SEM_DBUS:   if (bit > 7) return -1; return (gb->debug.cpu_viz.data_bus >> bit) & 1;
    case SM83_SEM_FLAG_Z: if (bit != 0) return -1; return gb->cpu.f_z ? 1 : 0;
    case SM83_SEM_FLAG_N: if (bit != 0) return -1; return gb->cpu.f_n ? 1 : 0;
    case SM83_SEM_FLAG_H: if (bit != 0) return -1; return gb->cpu.f_h ? 1 : 0;
    case SM83_SEM_FLAG_C: if (bit != 0) return -1; return gb->cpu.f_c ? 1 : 0;
    case SM83_SEM_VCC:    return 1;
    case SM83_SEM_GND:    return 0;
    case SM83_SEM_IDU:    return -1; /* IDU output not directly in gb->cpu */
    default:              return -1;
    }
}
