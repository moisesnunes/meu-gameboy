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
 *   - Propagation: iterative union-find style, converges in a few passes.
 *   - Special nets: VCC (always HIGH) and GND (always LOW) anchored by name.
 *
 * Usage:
 *   sm83_sim_init(&sim);          -- allocate and reset
 *   sm83_sim_seed_from_gb(&sim, gb); -- copy known signals from emulator
 *   sm83_sim_step(&sim);          -- run propagation until stable
 *   sm83_sim_net_state(&sim, id); -- query per-net state for rendering
 *   sm83_sim_shutdown(&sim);      -- free
 */
#ifndef SM83_NETLIST_SIM_H
#define SM83_NETLIST_SIM_H

#include <stdint.h>
#include <stdbool.h>

struct gb; /* forward — only used in sm83_sim_seed_from_gb */

/* Net state values (4 fit in 2 bits, stored as uint8_t for cache friendliness) */
typedef enum {
    SM83_SIM_UNKNOWN = 0, /* not yet resolved */
    SM83_SIM_LOW     = 1, /* logic 0 */
    SM83_SIM_HIGH    = 2, /* logic 1 */
    SM83_SIM_FLOAT   = 3, /* undriven / high-Z */
} Sm83SimState;

typedef struct {
    uint8_t  *net_state;      /* SM83_NET_COUNT entries, Sm83SimState per net */
    bool      initialized;
    bool      enabled;        /* false = sim is dormant, panel shows nothing */
    int       vcc_net;        /* index of VCC net (-1 if not found) */
    int       gnd_net;        /* index of GND net (-1 if not found) */
    int       iterations;     /* propagation passes used in last step */
} Sm83NetlistSim;

/* Allocate state arrays and find VCC/GND nets. Call once. */
void sm83_sim_init(Sm83NetlistSim *sim);

/* Free state arrays. */
void sm83_sim_shutdown(Sm83NetlistSim *sim);

/* Reset all nets to UNKNOWN. */
void sm83_sim_reset(Sm83NetlistSim *sim);

/* Seed known signals from the emulator's CPU state.
 * Only sets nets that have a named mapping in sm83_node_map.h.
 * Does not run propagation — call sm83_sim_step() after. */
void sm83_sim_seed_from_gb(Sm83NetlistSim *sim, const struct gb *gb);

/* Run propagation until stable or max_iters reached.
 * Returns number of iterations used. */
int sm83_sim_step(Sm83NetlistSim *sim, int max_iters);

/* Return the state of a single net by index. */
Sm83SimState sm83_sim_net_state(const Sm83NetlistSim *sim, int net_id);

/* Find the net index for a given net name string (linear scan, slow — for init only). */
int sm83_sim_find_net(const char *name);

#endif /* SM83_NETLIST_SIM_H */
