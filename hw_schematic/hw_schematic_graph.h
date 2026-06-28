/* hw_schematic_graph.h — Wire connectivity graph for topological animation.
 *
 * Builds an adjacency list over a dataset's wires[] so that a "flow"
 * animation can propagate along physically connected wire segments (same
 * net, touching endpoints) rather than lighting all segments at once.
 *
 * The graph is built once per dataset and cached.  Switching datasets
 * invalidates the cache via hw_graph_invalidate().
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "hw_schematic_data.h"
#include "hw_schematic_dataset.h"

/* Maximum neighbours per wire segment. */
#define HW_GRAPH_MAX_NEIGHBOURS 8

/* Maximum wires reachable in a single BFS (bounded for static allocation). */
#define HW_GRAPH_MAX_REACH 512

/* Static capacity: must be >= max wire_count across all datasets.
 * DMG=411  LCD=266 — 512 is safe headroom. */
#define HW_GRAPH_WIRE_CAP 512

typedef struct {
    int16_t nb[HW_GRAPH_MAX_NEIGHBOURS]; /* neighbour wire indices, -1=empty */
    uint8_t nb_count;
    uint8_t nb_ep[HW_GRAPH_MAX_NEIGHBOURS]; /* endpoint: 0=(nx1,ny1) 1=(nx2,ny2) */
} HwWireNode;

extern HwWireNode hw_graph_nodes[HW_GRAPH_WIRE_CAP];
extern bool       hw_graph_ready;

/* Build the connectivity graph for ds.  Safe to call multiple times;
 * re-builds when the dataset pointer changes. */
void hw_graph_build(const HwSchematicDataset *ds);

/* Invalidate cached graph (call when switching datasets). */
void hw_graph_invalidate(void);

/* BFS: collect all wire indices reachable from start_wire within net_id.
 * Returns number of wires written to out[]. */
int hw_graph_reachable(const HwSchematicDataset *ds,
                       int start_wire, int net_id,
                       int16_t *out, int max_out);

/* Return the wire index whose midpoint is closest to (nx,ny) within net_id.
 * Returns -1 if none found within max_dist (normalised). */
int hw_graph_nearest_wire(const HwSchematicDataset *ds,
                          int net_id, float nx, float ny, float max_dist);
