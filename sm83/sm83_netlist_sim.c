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
        if (idx >= 0)
            return idx;
    }
    return -1;
}

void sm83_sim_init(Sm83NetlistSim *sim)
{
    memset(sim, 0, sizeof(*sim));
    sim->net_state = (uint8_t *)calloc(SM83_NET_COUNT, sizeof(uint8_t));
    sim->vcc_net   = find_net_any(VCC_NAMES);
    sim->gnd_net   = find_net_any(GND_NAMES);
    sim->initialized = sim->net_state != NULL;
    sim->enabled     = false;
}

void sm83_sim_shutdown(Sm83NetlistSim *sim)
{
    free(sim->net_state);
    sim->net_state   = NULL;
    sim->initialized = false;
}

void sm83_sim_reset(Sm83NetlistSim *sim)
{
    if (!sim->net_state) return;
    memset(sim->net_state, SM83_SIM_UNKNOWN, SM83_NET_COUNT);
    /* Anchor power rails */
    if (sim->vcc_net >= 0) sim->net_state[sim->vcc_net] = SM83_SIM_HIGH;
    if (sim->gnd_net >= 0) sim->net_state[sim->gnd_net] = SM83_SIM_LOW;
}

/* -----------------------------------------------------------------------
 * Seed from emulator state.
 * We reuse sm83_node_map.h which maps named instances to emulator signals.
 * Here we look up the net names from those instances and seed them.
 * ---------------------------------------------------------------------- */

/* Inject one net by name with a HIGH/LOW value from the emulator */
static void seed_net(Sm83NetlistSim *sim, const char *net_name, bool high)
{
    int idx = sm83_sim_find_net(net_name);
    if (idx >= 0)
        sim->net_state[idx] = high ? SM83_SIM_HIGH : SM83_SIM_LOW;
}

void sm83_sim_seed_from_gb(Sm83NetlistSim *sim, const struct gb *gb)
{
    if (!sim->net_state || !gb) return;

    sm83_sim_reset(sim);

    uint16_t pc = gb->cpu.pc;
    uint8_t  a  = gb->cpu.a;
    uint8_t  b  = gb->cpu.b;
    uint8_t  c  = gb->cpu.c;
    uint8_t  d  = gb->cpu.d;
    uint8_t  e  = gb->cpu.e;
    uint8_t  h  = gb->cpu.h;
    uint8_t  l  = gb->cpu.l;

    /* Net names are from dmg-schematics sm83.jelib, verified against sm83_nets[].
     * PC low byte: "reg_pcl[i]", PC high byte: "reg_pch[i]" (not "pcl[i]"). */
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
    static const char *flag_nets[4] = {
        "flag_z","flag_n","flag_h","flag_c"
    };

    uint16_t sp = gb->cpu.sp;

    for (int i = 0; i < 8; i++) {
        seed_net(sim, pcl_nets[i],   (pc >> i) & 1);
        seed_net(sim, pch_nets[i],   (pc >> (8 + i)) & 1);
        seed_net(sim, reg_a_nets[i], (a >> i) & 1);
        seed_net(sim, reg_b_nets[i], (b >> i) & 1);
        seed_net(sim, reg_c_nets[i], (c >> i) & 1);
        seed_net(sim, reg_d_nets[i], (d >> i) & 1);
        seed_net(sim, reg_e_nets[i], (e >> i) & 1);
        seed_net(sim, reg_h_nets[i], (h >> i) & 1);
        seed_net(sim, reg_l_nets[i], (l >> i) & 1);
        seed_net(sim, reg_spl_nets[i], (sp >> i) & 1);
        seed_net(sim, reg_sph_nets[i], (sp >> (8 + i)) & 1);
    }
    seed_net(sim, flag_nets[0], gb->cpu.f_z);
    seed_net(sim, flag_nets[1], gb->cpu.f_n);
    seed_net(sim, flag_nets[2], gb->cpu.f_h);
    seed_net(sim, flag_nets[3], gb->cpu.f_c);
}

/* -----------------------------------------------------------------------
 * Propagation
 *
 * Algorithm (simplified Perfect6502-style):
 *   For each transistor:
 *     N-type: gate=HIGH -> connect s1 <-> s2 (merge their states)
 *     P-type: gate=LOW  -> connect s1 <-> s2
 *   A connection means: if either side is driven (HIGH/LOW), drive the other.
 *   Repeat until no state changes.
 *
 * This is a relaxation approach — not a proper union-find, but correct for
 * small iteration counts because the SM83 netlist is acyclic at most paths.
 * ---------------------------------------------------------------------- */

static Sm83SimState merge_driven(Sm83SimState a, Sm83SimState b)
{
    /* If one side is driven and the other is unknown/float, propagate */
    if (a == SM83_SIM_HIGH || a == SM83_SIM_LOW) return a;
    if (b == SM83_SIM_HIGH || b == SM83_SIM_LOW) return b;
    return SM83_SIM_FLOAT;
}

int sm83_sim_step(Sm83NetlistSim *sim, int max_iters)
{
    if (!sim->net_state) return 0;

    int iter;
    for (iter = 0; iter < max_iters; iter++) {
        int changed = 0;

        for (int i = 0; i < SM83_TRANSISTOR_COUNT; i++) {
            const Sm83Transistor *tr = &sm83_transistors[i];
            int gate = tr->gate_net;
            int s1   = tr->s1_net;
            int s2   = tr->s2_net;

            if (gate < 0 || s1 < 0 || s2 < 0) continue;
            if (s1 >= SM83_NET_COUNT || s2 >= SM83_NET_COUNT || gate >= SM83_NET_COUNT) continue;

            Sm83SimState gate_state = sim->net_state[gate];
            bool conducting;
            if (tr->layer == SM83_LAYER_NTRANS)
                conducting = (gate_state == SM83_SIM_HIGH);
            else /* PTRANS */
                conducting = (gate_state == SM83_SIM_LOW);

            if (!conducting) continue;

            Sm83SimState ns1 = sim->net_state[s1];
            Sm83SimState ns2 = sim->net_state[s2];
            Sm83SimState merged = merge_driven(ns1, ns2);

            if (merged != SM83_SIM_UNKNOWN && merged != SM83_SIM_FLOAT) {
                if (ns1 != merged) { sim->net_state[s1] = merged; changed++; }
                if (ns2 != merged) { sim->net_state[s2] = merged; changed++; }
            } else if (ns1 == SM83_SIM_FLOAT && ns2 == SM83_SIM_UNKNOWN) {
                sim->net_state[s2] = SM83_SIM_FLOAT; changed++;
            } else if (ns2 == SM83_SIM_FLOAT && ns1 == SM83_SIM_UNKNOWN) {
                sim->net_state[s1] = SM83_SIM_FLOAT; changed++;
            }
        }

        if (changed == 0) break;
    }

    sim->iterations = iter;
    return iter;
}

Sm83SimState sm83_sim_net_state(const Sm83NetlistSim *sim, int net_id)
{
    if (!sim->net_state || net_id < 0 || net_id >= SM83_NET_COUNT)
        return SM83_SIM_UNKNOWN;
    return (Sm83SimState)sim->net_state[net_id];
}
