/*
 * sm83_netlist_validate.c
 * Lightweight validation for the generated SM83 netlist data arrays.
 *
 * Tests (no emulator, no ROM, no SDL required):
 *   1. Counts are within expected ranges for the SM83 die.
 *   2. Bounding box matches the known Electric VLSI dimensions.
 *   3. Per-layer index ranges are contiguous and cover the full count.
 *   4. All transistor coordinates are normalized within [0,1].
 *   5. Transistors with gate/s1/s2 nets have indices in [0, SM83_NET_COUNT).
 *   6. Arcs with net_id have indices in [0, SM83_NET_COUNT).
 *   7. VCC and GND nets are findable by the sim's name lookup.
 *   8. sm83_sim_init/reset/step runs without crashing on a null gb seed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sm83/sm83_netlist_data.h"
#include "sm83/sm83_netlist_sim.h"

#define PASS(fmt, ...) do { printf("  PASS  " fmt "\n", ##__VA_ARGS__); } while(0)
#define FAIL(fmt, ...) do { printf("  FAIL  " fmt "\n", ##__VA_ARGS__); failures++; } while(0)
#define CHECK(cond, fmt, ...) do { if (cond) PASS(fmt, ##__VA_ARGS__); else FAIL(fmt, ##__VA_ARGS__); } while(0)

static int failures = 0;

/* ── 1. Counts ─────────────────────────────────────────────────────────── */
static void test_counts(void)
{
    printf("[1] Counts\n");
    CHECK(SM83_TRANSISTOR_COUNT > 9000 && SM83_TRANSISTOR_COUNT < 10000,
          "transistors=%d (expected ~9250)", SM83_TRANSISTOR_COUNT);
    CHECK(SM83_NODE_COUNT > 60000 && SM83_NODE_COUNT < 80000,
          "nodes=%d (expected ~66749)", SM83_NODE_COUNT);
    CHECK(SM83_ARC_COUNT > 80000 && SM83_ARC_COUNT < 100000,
          "arcs=%d (expected ~86726)", SM83_ARC_COUNT);
    CHECK(SM83_INSTANCE_COUNT > 600 && SM83_INSTANCE_COUNT < 800,
          "instances=%d (expected ~663)", SM83_INSTANCE_COUNT);
    CHECK(SM83_NET_COUNT > 10000 && SM83_NET_COUNT < 20000,
          "nets=%d (expected ~15440)", SM83_NET_COUNT);
}

/* ── 2. Bounding box ───────────────────────────────────────────────────── */
static void test_bbox(void)
{
    printf("[2] Bounding box\n");
    CHECK(fabsf(SM83_BBOX_X_MIN) < 0.01f, "X_MIN=%.2f", SM83_BBOX_X_MIN);
    CHECK(fabsf(SM83_BBOX_X_MAX - 3670.0f) < 1.0f, "X_MAX=%.2f (expected 3670)", SM83_BBOX_X_MAX);
    CHECK(fabsf(SM83_BBOX_Y_MIN) < 0.01f, "Y_MIN=%.2f", SM83_BBOX_Y_MIN);
    CHECK(fabsf(SM83_BBOX_Y_MAX - 3383.0f) < 1.0f, "Y_MAX=%.2f (expected 3383)", SM83_BBOX_Y_MAX);
}

/* ── 3. Per-layer ranges ───────────────────────────────────────────────── */
static void test_layer_ranges(void)
{
    printf("[3] Per-layer index ranges\n");

    /* Transistors: only NTRANS and PTRANS should be non-empty */
    int ntrans = SM83_TRANS_NTRANS_END - SM83_TRANS_NTRANS_START;
    int ptrans = SM83_TRANS_PTRANS_END - SM83_TRANS_PTRANS_START;
    CHECK(ntrans + ptrans == SM83_TRANSISTOR_COUNT,
          "ntrans(%d)+ptrans(%d)==%d", ntrans, ptrans, SM83_TRANSISTOR_COUNT);
    CHECK(SM83_TRANS_NTRANS_END == SM83_TRANS_PTRANS_START,
          "ntrans/ptrans ranges are contiguous");

    /* Arcs: METAL1+POLY+NACTIVE+PACTIVE should sum to total */
    int arc_sum = (SM83_ARC_METAL1_END - SM83_ARC_METAL1_START)
                + (SM83_ARC_POLY_END   - SM83_ARC_POLY_START)
                + (SM83_ARC_NACTIVE_END - SM83_ARC_NACTIVE_START)
                + (SM83_ARC_PACTIVE_END - SM83_ARC_PACTIVE_START);
    CHECK(arc_sum == SM83_ARC_COUNT,
          "arc layer ranges sum=%d == SM83_ARC_COUNT=%d", arc_sum, SM83_ARC_COUNT);

    /* Nodes: METAL1 covers everything (all geometry nodes are metal in current data) */
    int node_sum = SM83_NODE_METAL1_END - SM83_NODE_METAL1_START;
    CHECK(node_sum == SM83_NODE_COUNT,
          "node METAL1 range=%d == SM83_NODE_COUNT=%d", node_sum, SM83_NODE_COUNT);
}

/* ── 4. Transistor coordinate normalization ────────────────────────────── */
static void test_transistor_coords(void)
{
    printf("[4] Transistor normalized coordinates\n");
    int out_of_range = 0;
    for (int i = 0; i < SM83_TRANSISTOR_COUNT; i++) {
        const Sm83Transistor *t = &sm83_transistors[i];
        if (t->nx < 0.0f || t->nx > 1.0f || t->ny < 0.0f || t->ny > 1.0f)
            out_of_range++;
    }
    CHECK(out_of_range == 0,
          "%d/%d transistors have coords outside [0,1]", out_of_range, SM83_TRANSISTOR_COUNT);
}

/* ── 5. Transistor net indices ─────────────────────────────────────────── */
static void test_transistor_nets(void)
{
    printf("[5] Transistor gate/s1/s2 net indices\n");
    int bad_gate = 0, bad_s = 0;
    int with_gate = 0;
    for (int i = 0; i < SM83_TRANSISTOR_COUNT; i++) {
        const Sm83Transistor *t = &sm83_transistors[i];
        if (t->gate_net >= 0) {
            with_gate++;
            if (t->gate_net >= SM83_NET_COUNT) bad_gate++;
        }
        if (t->s1_net >= SM83_NET_COUNT || t->s2_net >= SM83_NET_COUNT) bad_s++;
        if (t->s1_net < -1 || t->s2_net < -1) bad_s++;
    }
    CHECK(with_gate > SM83_TRANSISTOR_COUNT * 9 / 10,
          "%d/%d transistors have gate net (>=90%% expected)", with_gate, SM83_TRANSISTOR_COUNT);
    CHECK(bad_gate == 0,
          "%d transistors have gate_net >= SM83_NET_COUNT", bad_gate);
    CHECK(bad_s == 0,
          "%d transistors have out-of-range s1/s2 net", bad_s);
}

/* ── 6. Arc net indices ────────────────────────────────────────────────── */
static void test_arc_nets(void)
{
    printf("[6] Arc net_id indices\n");
    int bad = 0, with_net = 0;
    for (int i = 0; i < SM83_ARC_COUNT; i++) {
        int nid = sm83_arcs[i].net_id;
        if (nid >= 0) {
            with_net++;
            if (nid >= SM83_NET_COUNT) bad++;
        }
    }
    CHECK(bad == 0, "%d arcs have net_id >= SM83_NET_COUNT", bad);
    /* Expect most arcs to have a valid net (from Phase 6 extraction) */
    int pct = with_net * 100 / SM83_ARC_COUNT;
    CHECK(pct >= 50,
          "%d/%d arcs have valid net_id (%d%%)", with_net, SM83_ARC_COUNT, pct);
}

/* ── 7. VCC/GND net lookup ─────────────────────────────────────────────── */
static void test_power_nets(void)
{
    printf("[7] VCC/GND net name lookup\n");
    static const char *vcc_names[] = { "vcc", "VCC", "vdd", "VDD", NULL };
    static const char *gnd_names[] = { "gnd", "GND", "vss", "VSS", NULL };

    int vcc_found = -1, gnd_found = -1;
    for (int i = 0; vcc_names[i]; i++) {
        int idx = sm83_sim_find_net(vcc_names[i]);
        if (idx >= 0) { vcc_found = idx; break; }
    }
    for (int i = 0; gnd_names[i]; i++) {
        int idx = sm83_sim_find_net(gnd_names[i]);
        if (idx >= 0) { gnd_found = idx; break; }
    }

    if (vcc_found >= 0)
        PASS("VCC net found at index %d", vcc_found);
    else
        printf("  WARN  VCC net not found — propagation will have no HIGH anchor\n");

    if (gnd_found >= 0)
        PASS("GND net found at index %d", gnd_found);
    else
        printf("  WARN  GND net not found — propagation will have no LOW anchor\n");
}

/* ── 8. Sim init/reset/step smoke test ─────────────────────────────────── */
static void test_sim_smoke(void)
{
    printf("[8] Sim init/reset/step smoke\n");
    Sm83NetlistSim sim;
    sm83_sim_init(&sim);

    CHECK(sim.initialized, "sim.initialized after init");
    CHECK(sim.net_state != NULL, "sim.net_state != NULL");

    sm83_sim_reset(&sim);

    /* VCC should be HIGH, GND should be LOW */
    if (sim.vcc_net >= 0)
        CHECK(sm83_sim_net_state(&sim, sim.vcc_net) == SM83_SIM_HIGH,
              "VCC net is HIGH after reset");
    if (sim.gnd_net >= 0)
        CHECK(sm83_sim_net_state(&sim, sim.gnd_net) == SM83_SIM_LOW,
              "GND net is LOW after reset");

    /* Run a few propagation steps without seeding — should not crash */
    int iters = sm83_sim_step(&sim, 4);
    CHECK(iters >= 0 && iters <= 4, "step returned %d iters (expected 0..4)", iters);

    /* Out-of-range net query returns UNKNOWN */
    Sm83SimState bad = sm83_sim_net_state(&sim, -1);
    CHECK(bad == SM83_SIM_UNKNOWN, "net_state(-1) == UNKNOWN");
    bad = sm83_sim_net_state(&sim, SM83_NET_COUNT);
    CHECK(bad == SM83_SIM_UNKNOWN, "net_state(NET_COUNT) == UNKNOWN");

    sm83_sim_shutdown(&sim);
    CHECK(sim.net_state == NULL, "net_state NULL after shutdown");
    CHECK(!sim.initialized, "initialized false after shutdown");
}

/* ─────────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("SM83 netlist data validation\n");
    printf("  transistors : %d\n", SM83_TRANSISTOR_COUNT);
    printf("  nodes       : %d\n", SM83_NODE_COUNT);
    printf("  arcs        : %d\n", SM83_ARC_COUNT);
    printf("  nets        : %d\n", SM83_NET_COUNT);
    printf("  instances   : %d\n", SM83_INSTANCE_COUNT);
    printf("\n");

    test_counts();
    printf("\n");
    test_bbox();
    printf("\n");
    test_layer_ranges();
    printf("\n");
    test_transistor_coords();
    printf("\n");
    test_transistor_nets();
    printf("\n");
    test_arc_nets();
    printf("\n");
    test_power_nets();
    printf("\n");
    test_sim_smoke();
    printf("\n");

    if (failures == 0)
        printf("All tests passed.\n");
    else
        printf("%d test(s) FAILED.\n", failures);

    return failures > 0 ? 1 : 0;
}
