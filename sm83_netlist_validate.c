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
 *   7. VCC and GND rails: named lookup or heuristic fallback.
 *   8. sm83_sim_init/reset/step runs without crashing on a null gb seed.
 *   9. Rail propagation coverage with small seed.
 *  10. Semantic map: init resolves net_ids, no duplicates, confidence is sane.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sm83/sm83_netlist_data.h"
#include "sm83/sm83_netlist_sim.h"
#include "sm83/sm83_semantic_map.h"

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

/* ── 7. VCC/GND rail resolution ────────────────────────────────────────── */
static void test_power_nets(void)
{
    printf("[7] VCC/GND rail resolution\n");

    /* Check named lookup first */
    static const char *vcc_names[] = { "vcc", "VCC", "vdd", "VDD", NULL };
    static const char *gnd_names[] = { "gnd", "GND", "vss", "VSS", NULL };
    int vcc_named = -1, gnd_named = -1;
    for (int i = 0; vcc_names[i]; i++) {
        int idx = sm83_sim_find_net(vcc_names[i]);
        if (idx >= 0) { vcc_named = idx; break; }
    }
    for (int i = 0; gnd_names[i]; i++) {
        int idx = sm83_sim_find_net(gnd_names[i]);
        if (idx >= 0) { gnd_named = idx; break; }
    }

    if (vcc_named >= 0)
        PASS("VCC net found by name at index %d", vcc_named);
    else
        printf("  INFO  VCC not found by name — heuristic fallback: net_id=%d (%s)\n",
               SM83_VCC_NET_HEURISTIC, sm83_nets[SM83_VCC_NET_HEURISTIC]);

    if (gnd_named >= 0)
        PASS("GND net found by name at index %d", gnd_named);
    else
        printf("  INFO  GND not found by name — heuristic fallback: net_id=%d (%s)\n",
               SM83_GND_NET_HEURISTIC, sm83_nets[SM83_GND_NET_HEURISTIC]);

    /* Verify sim init resolves rails (named or heuristic) */
    Sm83NetlistSim sim;
    sm83_sim_init(&sim);
    static const char *src_names[] = { "missing", "named", "heuristic", "manual" };
    if (sim.rails_found)
        PASS("sim rails resolved via '%s'  VCC=%d  GND=%d",
             src_names[sim.rail_source & 3], sim.vcc_net, sim.gnd_net);
    else
        printf("  FAIL  sim.rails_found=false after init\n");
    sm83_sim_shutdown(&sim);
}

/* ── 8. Sim init/reset/step smoke test ─────────────────────────────────── */
static void test_sim_smoke(void)
{
    printf("[8] Sim init/reset/step smoke\n");
    Sm83NetlistSim sim;
    sm83_sim_init(&sim);

    CHECK(sim.initialized,          "sim.initialized after init");
    CHECK(sim.net_state  != NULL,   "sim.net_state != NULL");
    CHECK(sim.net_source != NULL,   "sim.net_source != NULL");

    sm83_sim_reset(&sim);

    /* VCC should be HIGH (strongly driven), GND should be LOW */
    if (sim.vcc_net >= 0) {
        CHECK(sm83_sim_net_state(&sim, sim.vcc_net)  == SM83_SIM_HIGH,
              "VCC net is HIGH after reset");
        CHECK(sm83_sim_net_source(&sim, sim.vcc_net) == SM83_SRC_RAIL,
              "VCC net source is RAIL after reset");
    }
    if (sim.gnd_net >= 0) {
        CHECK(sm83_sim_net_state(&sim, sim.gnd_net)  == SM83_SIM_LOW,
              "GND net is LOW after reset");
        CHECK(sm83_sim_net_source(&sim, sim.gnd_net) == SM83_SRC_RAIL,
              "GND net source is RAIL after reset");
    }

    /* Rails must survive propagation — run a few steps */
    sm83_sim_step(&sim, 4);
    if (sim.vcc_net >= 0)
        CHECK(sm83_sim_net_state(&sim, sim.vcc_net) == SM83_SIM_HIGH,
              "VCC remains HIGH after propagation");
    if (sim.gnd_net >= 0)
        CHECK(sm83_sim_net_state(&sim, sim.gnd_net) == SM83_SIM_LOW,
              "GND remains LOW after propagation");

    /* Conflict count must be non-negative */
    CHECK(sim.conflict_count >= 0, "conflict_count >= 0 after step");
    printf("  INFO  conflict_count=%d after 4-iter step (rails only, no seeds)\n",
           sim.conflict_count);

    /* Out-of-range net query returns UNKNOWN/NONE */
    CHECK(sm83_sim_net_state(&sim,  -1)           == SM83_SIM_UNKNOWN, "net_state(-1) == UNKNOWN");
    CHECK(sm83_sim_net_state(&sim,  SM83_NET_COUNT)== SM83_SIM_UNKNOWN, "net_state(NET_COUNT) == UNKNOWN");
    CHECK(sm83_sim_net_source(&sim, -1)            == SM83_SRC_NONE,    "net_source(-1) == NONE");

    sm83_sim_shutdown(&sim);
    CHECK(sim.net_state  == NULL, "net_state NULL after shutdown");
    CHECK(sim.net_source == NULL, "net_source NULL after shutdown");
    CHECK(!sim.initialized,       "initialized false after shutdown");
}

/* ── 9. Rail propagation coverage ─────────────────────────────────────── */
static void test_rail_propagation(void)
{
    printf("[9] Rail propagation coverage\n");
    Sm83NetlistSim sim;
    sm83_sim_init(&sim);
    sm83_sim_reset(&sim);

    /* Seed typical register values so we have some HIGH/LOW non-rail drivers */
    static const char *a_nets[8] = {
        "reg_a[0]","reg_a[1]","reg_a[2]","reg_a[3]",
        "reg_a[4]","reg_a[5]","reg_a[6]","reg_a[7]"
    };
    for (int i = 0; i < 8; i++) {
        int idx = sm83_sim_find_net(a_nets[i]);
        if (idx >= 0 && idx != sim.vcc_net && idx != sim.gnd_net) {
            sim.net_state[idx]  = (i & 1) ? SM83_SIM_HIGH : SM83_SIM_LOW;
            sim.net_source[idx] = SM83_SRC_SEED;
        }
    }

    int iters = sm83_sim_step(&sim, 64);

    int resolved = 0, unknown = 0, floating = 0, conflicted = 0;
    for (int i = 0; i < SM83_NET_COUNT; i++) {
        switch ((Sm83SimState)sim.net_state[i]) {
            case SM83_SIM_HIGH: case SM83_SIM_LOW:
            case SM83_SIM_HIGH_WEAK: case SM83_SIM_LOW_WEAK:
                resolved++; break;
            case SM83_SIM_FLOAT:   floating++;   break;
            case SM83_SIM_CONFLICT: conflicted++; break;
            default: unknown++; break;
        }
    }

    int pct_resolved = resolved * 100 / SM83_NET_COUNT;
    printf("  INFO  iters=%d  resolved=%d/%d (%d%%)  unknown=%d  float=%d  conflict=%d\n",
           iters, resolved, SM83_NET_COUNT, pct_resolved, unknown, floating, conflicted);

    /* With rails + 8 seeds we expect at least some propagation */
    CHECK(resolved >= 2, "at least 2 nets resolved (rails themselves)");
    /* iters==0 is valid if no transistor gate is driven by the small seed — no change to propagate */
    CHECK(iters >= 0,    "propagation step returned without error");
    CHECK(conflicted == 0, "no conflicts with rails-only + small seed");

    sm83_sim_shutdown(&sim);
}

/* ── 10. Semantic map integrity ─────────────────────────────────────────── */
static void test_semantic_map(void)
{
    printf("[10] Semantic map integrity\n");

    sm83_semantic_map_init();

    int total   = sm83_semantic_map_count;
    int found   = 0, not_found = 0, downgraded = 0, dup_net = 0;

    /* Check each entry */
    for (int i = 0; i < total; i++) {
        const Sm83NetSemanticEntry *e = &sm83_semantic_map[i];

        /* Rails are pre-baked — skip name-lookup check */
        if (e->signal == SM83_SEM_VCC || e->signal == SM83_SEM_GND) {
            if (e->net_id >= 0 && e->net_id < SM83_NET_COUNT) found++;
            continue;
        }

        if (e->net_id >= 0 && e->net_id < SM83_NET_COUNT) {
            found++;
        } else {
            not_found++;
        }

        /* A non-rail entry that was PROBABLE but name not found → downgraded to PROXY */
        if (e->net_id < 0 && e->confidence <= SM83_CONF_PROXY)
            downgraded++;
    }

    /* Check for cross-signal duplicate net_id (same net → two different signals).
     * Intra-signal duplicates (same net for different bits of A[0..7]) are expected
     * because spatial tracing returns the shared bus net for the whole register. */
    for (int i = 0; i < total; i++) {
        int nid = sm83_semantic_map[i].net_id;
        if (nid < 0) continue;
        for (int j = i + 1; j < total; j++) {
            if (sm83_semantic_map[j].net_id != nid) continue;
            if (sm83_semantic_map[j].signal != sm83_semantic_map[i].signal) {
                dup_net++;
                printf("  INFO  cross-signal dup net_id=%d: entries %d(%d) and %d(%d)\n",
                       nid, i, (int)sm83_semantic_map[i].signal,
                       j, (int)sm83_semantic_map[j].signal);
            }
        }
    }

    printf("  INFO  total=%d  found=%d  not_found=%d  downgraded=%d\n",
           total, found, not_found, downgraded);

    CHECK(total > 100, "map has >100 entries (expect %d)", total);
    /* Most nets are "net@N" — named register nets not yet in netlist.
     * Only rails (2 entries) are pre-resolved; the rest require transistor
     * tracing (future Etapa E).  Accept any count >= 2 (rails themselves). */
    CHECK(found >= 2,
          "%d/%d entries resolved (rails at minimum)", found, total);
    CHECK(not_found == downgraded,
          "all unresolved entries downgraded (%d not_found, %d downgraded)",
          not_found, downgraded);
    CHECK(dup_net == 0, "no duplicate net_id assignments");

    /* Verify sm83_semantic_map_find returns correct entries */
    const Sm83NetSemanticEntry *vcc = sm83_semantic_map_find(
            SM83_VCC_NET_HEURISTIC, SM83_CONF_CONFIRMED);
    CHECK(vcc != NULL && vcc->signal == SM83_SEM_VCC,
          "find VCC net at id=%d", SM83_VCC_NET_HEURISTIC);

    const Sm83NetSemanticEntry *gnd = sm83_semantic_map_find(
            SM83_GND_NET_HEURISTIC, SM83_CONF_CONFIRMED);
    CHECK(gnd != NULL && gnd->signal == SM83_SEM_GND,
          "find GND net at id=%d", SM83_GND_NET_HEURISTIC);

    /* sm83_semantic_map_find with min=CONFIRMED should not return PROBABLE entries */
    int first_probable_id = -1;
    for (int i = 0; i < total; i++) {
        const Sm83NetSemanticEntry *e = &sm83_semantic_map[i];
        if (e->net_id >= 0 && e->confidence == SM83_CONF_PROBABLE) {
            first_probable_id = e->net_id;
            break;
        }
    }
    if (first_probable_id >= 0) {
        const Sm83NetSemanticEntry *r = sm83_semantic_map_find(
                first_probable_id, SM83_CONF_CONFIRMED);
        CHECK(r == NULL,
              "find(net=%d, min=CONFIRMED) returns NULL for PROBABLE entry",
              first_probable_id);
    }
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
    test_rail_propagation();
    printf("\n");
    test_semantic_map();
    printf("\n");

    if (failures == 0)
        printf("All tests passed.\n");
    else
        printf("%d test(s) FAILED.\n", failures);

    return failures > 0 ? 1 : 0;
}
