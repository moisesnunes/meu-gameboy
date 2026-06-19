/* hw_schematic_graph.h — Wire connectivity graph for topological animation.
 *
 * Builds an adjacency list over hw_wires[] so that a "flow" animation can
 * propagate along physically connected wire segments (same net, touching
 * endpoints or junction points) rather than lighting all segments of a net
 * simultaneously.
 *
 * The graph is built once at init time (hw_graph_build) and is read-only
 * thereafter.  All allocations are static; no heap needed.
 *
 * Usage:
 *   hw_graph_build();
 *   hw_graph_reachable(root_wire, net_id, out, n);
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "hw_schematic_data.h"

/* Maximum neighbours per wire segment.  In practice DMG has at most 4-5
 * wires meeting at any junction, so 8 is generous. */
#define HW_GRAPH_MAX_NEIGHBOURS 8

/* Maximum wires reachable in a single BFS (bounded for static allocation). */
#define HW_GRAPH_MAX_REACH 128

typedef struct {
    /* neighbour wire indices (into hw_wires[]), -1 = empty slot */
    int16_t nb[HW_GRAPH_MAX_NEIGHBOURS];
    uint8_t nb_count;
    /* Which endpoint connects to each neighbour: 0 = (nx1,ny1), 1 = (nx2,ny2) */
    uint8_t nb_ep[HW_GRAPH_MAX_NEIGHBOURS];
} HwWireNode;

/* One node per wire in hw_wires[]. */
extern HwWireNode hw_graph_nodes[HW_WIRE_COUNT];
extern bool       hw_graph_ready;

/* Build the connectivity graph.  Safe to call multiple times (idempotent). */
void hw_graph_build(void);

/* BFS: collect all wire indices reachable from start_wire within the same
 * net (net_id).  out[] receives indices; max_out is its capacity.
 * Returns number of wires written to out[].
 * If start_wire < 0 or its net_id doesn't match, returns 0. */
int hw_graph_reachable(int start_wire, int net_id,
                        int16_t *out, int max_out);

/* Return the wire index whose midpoint is closest to (nx, ny) within the
 * given net_id.  Returns -1 if none found within max_dist (normalised). */
int hw_graph_nearest_wire(int net_id, float nx, float ny, float max_dist);
