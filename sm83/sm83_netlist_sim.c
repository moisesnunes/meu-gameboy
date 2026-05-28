/* sm83_netlist_sim.c -- experimental transistor-level netlist simulator */
#include "sm83_netlist_sim.h"
#include "sm83_netlist_data.h"
#include "sm83_node_map.h"
#include "gb.h"
#include <stdlib.h>
#include <string.h>

/* Known power rail names in the SM83 .jelib netlist */
static const char *VCC_NAMES[] = { "vcc", "VCC", "vdd", "VDD", NULL };
static const char *GND_NAMES[] = { "gnd", "GND", "vss", "VSS", NULL };

int sm83_sim_find_net(const char *name)
{
    for (int i = 0; i < SM83_NET_COUNT; i++) {
        if (sm83_nets[i] && strcmp(sm83_nets[i], name) == 0)
            return i;
    }
    return -1;
}

static int find_net_any(const char **names)
{
    for (int i = 0; names[i]; i++) {
        int idx = sm83_sim_find_net(names[i]);
        if (idx >= 0) return idx;
    }
    return -1;
}

void sm83_sim_init(Sm83NetlistSim *sim)
{
    memset(sim, 0, sizeof(*sim));
    sim->net_state  = (uint8_t *)calloc(SM83_NET_COUNT, sizeof(uint8_t));
    sim->net_source = (uint8_t *)calloc(SM83_NET_COUNT, sizeof(uint8_t));

    /* Try named lookup first; fall back to heuristic indices */
    sim->vcc_net = find_net_any(VCC_NAMES);
    sim->gnd_net = find_net_any(GND_NAMES);
    sim->rail_source = SM83_RAILS_MISSING;

    if (sim->vcc_net >= 0 && sim->gnd_net >= 0) {
        sim->rail_source = SM83_RAILS_NAMED;
    } else {
        if (sim->vcc_net < 0) sim->vcc_net = SM83_VCC_NET_HEURISTIC;
        if (sim->gnd_net < 0) sim->gnd_net = SM83_GND_NET_HEURISTIC;
        sim->rail_source = SM83_RAILS_HEURISTIC;
    }

    sim->initialized = (sim->net_state != NULL && sim->net_source != NULL);
    sim->enabled     = false;
    sim->rails_found = (sim->vcc_net >= 0 && sim->gnd_net >= 0);
}

void sm83_sim_shutdown(Sm83NetlistSim *sim)
{
    free(sim->net_state);
    free(sim->net_source);
    sim->net_state   = NULL;
    sim->net_source  = NULL;
    sim->initialized = false;
    sim->enabled     = false;
    sim->rails_found = false;
}

void sm83_sim_reset(Sm83NetlistSim *sim)
{
    if (!sim->net_state) return;
    memset(sim->net_state,  SM83_SIM_UNKNOWN, SM83_NET_COUNT);
    memset(sim->net_source, SM83_SRC_NONE,    SM83_NET_COUNT);
    /* Anchor power rails as strongly driven */
    if (sim->vcc_net >= 0) {
        sim->net_state[sim->vcc_net]  = SM83_SIM_HIGH;
        sim->net_source[sim->vcc_net] = SM83_SRC_RAIL;
    }
    if (sim->gnd_net >= 0) {
        sim->net_state[sim->gnd_net]  = SM83_SIM_LOW;
        sim->net_source[sim->gnd_net] = SM83_SRC_RAIL;
    }
    sim->conflict_count = 0;
}

/* -----------------------------------------------------------------------
 * Seed from emulator state
 * ----------------------------------------------------------------------- */

static void seed_net(Sm83NetlistSim *sim, const char *net_name, bool high)
{
    int idx = sm83_sim_find_net(net_name);
    if (idx < 0) return;
    /* Rails are immutable */
    if (idx == sim->vcc_net || idx == sim->gnd_net) return;
    sim->net_state[idx]  = high ? SM83_SIM_HIGH : SM83_SIM_LOW;
    sim->net_source[idx] = SM83_SRC_SEED;
}

void sm83_sim_seed_from_gb(Sm83NetlistSim *sim, const struct gb *gb)
{
    if (!sim->net_state || !gb) return;

    sm83_sim_reset(sim);

    uint16_t pc   = gb->cpu.pc;
    uint8_t  a    = gb->cpu.a;
    uint8_t  b    = gb->cpu.b;
    uint8_t  c    = gb->cpu.c;
    uint8_t  d    = gb->cpu.d;
    uint8_t  e    = gb->cpu.e;
    uint8_t  h    = gb->cpu.h;
    uint8_t  l    = gb->cpu.l;
    uint16_t sp   = gb->cpu.sp;
    uint8_t  ir   = gb->debug.cpu_viz.opcode;
    uint8_t  dbus = gb->debug.cpu_viz.data_bus;

    static const char *pcl_nets[8] = {
        "reg_pcl[0]","reg_pcl[1]","reg_pcl[2]","reg_pcl[3]",
        "reg_pcl[4]","reg_pcl[5]","reg_pcl[6]","reg_pcl[7]"
    };
    static const char *pch_nets[8] = {
        "reg_pch[0]","reg_pch[1]","reg_pch[2]","reg_pch[3]",
        "reg_pch[4]","reg_pch[5]","reg_pch[6]","reg_pch[7]"
    };
    static const char *reg_a_nets[8] = {
        "reg_a[0]","reg_a[1]","reg_a[2]","reg_a[3]",
        "reg_a[4]","reg_a[5]","reg_a[6]","reg_a[7]"
    };
    static const char *reg_b_nets[8] = {
        "reg_b[0]","reg_b[1]","reg_b[2]","reg_b[3]",
        "reg_b[4]","reg_b[5]","reg_b[6]","reg_b[7]"
    };
    static const char *reg_c_nets[8] = {
        "reg_c[0]","reg_c[1]","reg_c[2]","reg_c[3]",
        "reg_c[4]","reg_c[5]","reg_c[6]","reg_c[7]"
    };
    static const char *reg_d_nets[8] = {
        "reg_d[0]","reg_d[1]","reg_d[2]","reg_d[3]",
        "reg_d[4]","reg_d[5]","reg_d[6]","reg_d[7]"
    };
    static const char *reg_e_nets[8] = {
        "reg_e[0]","reg_e[1]","reg_e[2]","reg_e[3]",
        "reg_e[4]","reg_e[5]","reg_e[6]","reg_e[7]"
    };
    static const char *reg_h_nets[8] = {
        "reg_h[0]","reg_h[1]","reg_h[2]","reg_h[3]",
        "reg_h[4]","reg_h[5]","reg_h[6]","reg_h[7]"
    };
    static const char *reg_l_nets[8] = {
        "reg_l[0]","reg_l[1]","reg_l[2]","reg_l[3]",
        "reg_l[4]","reg_l[5]","reg_l[6]","reg_l[7]"
    };
    static const char *reg_spl_nets[8] = {
        "reg_spl[0]","reg_spl[1]","reg_spl[2]","reg_spl[3]",
        "reg_spl[4]","reg_spl[5]","reg_spl[6]","reg_spl[7]"
    };
    static const char *reg_sph_nets[8] = {
        "reg_sph[0]","reg_sph[1]","reg_sph[2]","reg_sph[3]",
        "reg_sph[4]","reg_sph[5]","reg_sph[6]","reg_sph[7]"
    };
    static const char *reg_ir_nets[8] = {
        "reg_ir[0]","reg_ir[1]","reg_ir[2]","reg_ir[3]",
        "reg_ir[4]","reg_ir[5]","reg_ir[6]","reg_ir[7]"
    };
    static const char *dbus_nets[8] = {
        "dbus_bridge[0]","dbus_bridge[1]","dbus_bridge[2]","dbus_bridge[3]",
        "dbus_bridge[4]","dbus_bridge[5]","dbus_bridge[6]","dbus_bridge[7]"
    };
    static const char *flag_nets[4] = { "flag_z","flag_n","flag_h","flag_c" };

    for (int i = 0; i < 8; i++) {
        seed_net(sim, pcl_nets[i],     (pc  >> i)       & 1);
        seed_net(sim, pch_nets[i],     (pc  >> (8 + i)) & 1);
        seed_net(sim, reg_a_nets[i],   (a   >> i)       & 1);
        seed_net(sim, reg_b_nets[i],   (b   >> i)       & 1);
        seed_net(sim, reg_c_nets[i],   (c   >> i)       & 1);
        seed_net(sim, reg_d_nets[i],   (d   >> i)       & 1);
        seed_net(sim, reg_e_nets[i],   (e   >> i)       & 1);
        seed_net(sim, reg_h_nets[i],   (h   >> i)       & 1);
        seed_net(sim, reg_l_nets[i],   (l   >> i)       & 1);
        seed_net(sim, reg_spl_nets[i], (sp  >> i)       & 1);
        seed_net(sim, reg_sph_nets[i], (sp  >> (8 + i)) & 1);
        seed_net(sim, reg_ir_nets[i],  (ir  >> i)       & 1);
        seed_net(sim, dbus_nets[i],    (dbus >> i)      & 1);
    }
    seed_net(sim, flag_nets[0], gb->cpu.f_z);
    seed_net(sim, flag_nets[1], gb->cpu.f_n);
    seed_net(sim, flag_nets[2], gb->cpu.f_h);
    seed_net(sim, flag_nets[3], gb->cpu.f_c);
}

/* -----------------------------------------------------------------------
 * Propagation
 *
 * Two-pass per iteration:
 *   Pass A — conductivity: decide which transistors conduct based on gate.
 *   Pass B — resolution: for each conducting transistor, merge s1/s2 nets.
 *
 * Merge rules (priority order, highest first):
 *   CONFLICT  — if both sides are driven and disagree → stays CONFLICT
 *   HIGH      — strong driver wins over weak/unknown/float
 *   LOW       — strong driver wins over weak/unknown/float
 *   HIGH_WEAK — weak propagation, overridden by any strong driver
 *   LOW_WEAK  — weak propagation, overridden by any strong driver
 *   FLOAT     — conducting transistor path exists but no logical driver
 *   UNKNOWN   — no path at all
 *
 * Rails (VCC/GND) are re-anchored after each iteration so they can never
 * be overwritten by propagation or conflict.
 * ----------------------------------------------------------------------- */

/* Returns the "winner" when two nets are connected by a conducting transistor.
 * Also sets *conflict if both sides are strongly driven with opposite values. */
static Sm83SimState merge_nets(Sm83SimState a, Sm83SimState b, bool *conflict)
{
    /* CONFLICT is sticky */
    if (a == SM83_SIM_CONFLICT || b == SM83_SIM_CONFLICT) return SM83_SIM_CONFLICT;

    bool a_high = SM83_SIM_IS_HIGH(a);
    bool a_low  = SM83_SIM_IS_LOW(a);
    bool b_high = SM83_SIM_IS_HIGH(b);
    bool b_low  = SM83_SIM_IS_LOW(b);

    /* Detect strong vs strong conflict */
    if (SM83_SIM_IS_DRIVEN(a) && SM83_SIM_IS_DRIVEN(b)) {
        if (a_high && b_low) { *conflict = true; return SM83_SIM_CONFLICT; }
        if (a_low  && b_high){ *conflict = true; return SM83_SIM_CONFLICT; }
        /* Both driven same value — agree */
        return a;
    }

    /* One side is strongly driven — wins */
    if (SM83_SIM_IS_DRIVEN(a)) return a;
    if (SM83_SIM_IS_DRIVEN(b)) return b;

    /* Weak vs weak — take the one that is resolved */
    if (a_high || a_low) {
        /* a is weak, b is not driven strongly */
        if (b == SM83_SIM_UNKNOWN || b == SM83_SIM_FLOAT)
            return a_high ? SM83_SIM_HIGH_WEAK : SM83_SIM_LOW_WEAK;
        /* both weak — if same polarity keep, else conflict is unlikely for weak */
        return a;
    }
    if (b_high || b_low) {
        if (a == SM83_SIM_UNKNOWN || a == SM83_SIM_FLOAT)
            return b_high ? SM83_SIM_HIGH_WEAK : SM83_SIM_LOW_WEAK;
        return b;
    }

    /* Neither side driven — propagate FLOAT */
    if (a == SM83_SIM_FLOAT || b == SM83_SIM_FLOAT) return SM83_SIM_FLOAT;
    return SM83_SIM_UNKNOWN;
}

/* Whether a transistor is conducting given its gate state and type */
static bool transistor_conducts(uint8_t layer, Sm83SimState gate)
{
    if (layer == SM83_LAYER_NTRANS)
        return SM83_SIM_IS_HIGH(gate);   /* N: conducts when gate HIGH */
    else
        return SM83_SIM_IS_LOW(gate);    /* P: conducts when gate LOW  */
}

int sm83_sim_step(Sm83NetlistSim *sim, int max_iters)
{
    if (!sim->net_state || !sim->net_source) return 0;
    if (!sim->rails_found) {
        sim->iterations     = 0;
        sim->conflict_count = 0;
        return 0;
    }

    int iter;
    for (iter = 0; iter < max_iters; iter++) {
        int changed = 0;

        for (int i = 0; i < SM83_TRANSISTOR_COUNT; i++) {
            const Sm83Transistor *tr = &sm83_transistors[i];
            int gate = tr->gate_net;
            int s1   = tr->s1_net;
            int s2   = tr->s2_net;

            if (gate < 0 || s1 < 0 || s2 < 0) continue;
            if (gate >= SM83_NET_COUNT || s1 >= SM83_NET_COUNT || s2 >= SM83_NET_COUNT) continue;

            Sm83SimState gs = (Sm83SimState)sim->net_state[gate];
            if (!transistor_conducts(tr->layer, gs)) continue;

            Sm83SimState ns1 = (Sm83SimState)sim->net_state[s1];
            Sm83SimState ns2 = (Sm83SimState)sim->net_state[s2];

            bool conflict = false;
            Sm83SimState merged = merge_nets(ns1, ns2, &conflict);

            /* Determine source for merged value */
            Sm83NetSource src = SM83_SRC_PROP;
            if (conflict)
                src = SM83_SRC_CONFLICT;
            else if (sim->net_source[s1] == SM83_SRC_RAIL || sim->net_source[s2] == SM83_SRC_RAIL)
                src = SM83_SRC_RAIL;
            else if (sim->net_source[s1] == SM83_SRC_SEED || sim->net_source[s2] == SM83_SRC_SEED)
                src = SM83_SRC_SEED;

            /* Rails are immutable — never overwrite them */
            if (s1 != sim->vcc_net && s1 != sim->gnd_net && merged != ns1) {
                sim->net_state[s1]  = (uint8_t)merged;
                sim->net_source[s1] = (uint8_t)src;
                changed++;
            }
            if (s2 != sim->vcc_net && s2 != sim->gnd_net && merged != ns2) {
                sim->net_state[s2]  = (uint8_t)merged;
                sim->net_source[s2] = (uint8_t)src;
                changed++;
            }
        }

        if (changed == 0) break;
    }

    /* Count conflicts */
    int conflicts = 0;
    for (int i = 0; i < SM83_NET_COUNT; i++)
        if (sim->net_state[i] == SM83_SIM_CONFLICT) conflicts++;

    sim->iterations     = iter;
    sim->conflict_count = conflicts;
    return iter;
}

Sm83SimState sm83_sim_net_state(const Sm83NetlistSim *sim, int net_id)
{
    if (!sim->net_state || net_id < 0 || net_id >= SM83_NET_COUNT)
        return SM83_SIM_UNKNOWN;
    return (Sm83SimState)sim->net_state[net_id];
}

Sm83NetSource sm83_sim_net_source(const Sm83NetlistSim *sim, int net_id)
{
    if (!sim->net_source || net_id < 0 || net_id >= SM83_NET_COUNT)
        return SM83_SRC_NONE;
    return (Sm83NetSource)sim->net_source[net_id];
}
