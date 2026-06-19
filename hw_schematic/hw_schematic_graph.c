/* hw_schematic_graph.c — Wire connectivity graph.
 *
 * Two wire segments are considered connected when:
 *   1. They share the same net_id (or both net_id == -1 for unconnected stubs,
 *      which are never animated), AND
 *   2. At least one endpoint of each wire coincides within SNAP_DIST.
 *
 * This matches KiCad's connectivity rule: wires connect only at endpoints
 * or explicit junction points.  Crossing wires without a junction do NOT
 * connect.
 *
 * SNAP_DIST is set to half a 1.27mm KiCad grid unit in normalised space
 * (1.27mm / 297mm ≈ 0.00428).  We use 0.006 for a little slack.
 */
#include "hw_schematic_graph.h"
#include <string.h>
#include <math.h>

#define SNAP_DIST 0.006f

HwWireNode hw_graph_nodes[HW_WIRE_COUNT];
bool       hw_graph_ready = false;

static inline float dist2(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

void hw_graph_build(void)
{
    if (hw_graph_ready) return;
    memset(hw_graph_nodes, 0xFF, sizeof(hw_graph_nodes)); /* fill -1 */
    for (int i = 0; i < HW_WIRE_COUNT; i++)
        hw_graph_nodes[i].nb_count = 0;

    float snap2 = SNAP_DIST * SNAP_DIST;

    for (int i = 0; i < HW_WIRE_COUNT; i++) {
        const HwWire *wi = &hw_wires[i];
        if (wi->net_id < 0) continue; /* unconnected stub — skip */

        /* Endpoints of wire i */
        float ix[2] = { wi->nx1, wi->nx2 };
        float iy[2] = { wi->ny1, wi->ny2 };

        for (int j = i + 1; j < HW_WIRE_COUNT; j++) {
            const HwWire *wj = &hw_wires[j];
            if (wj->net_id != wi->net_id) continue; /* different net */

            float jx[2] = { wj->nx1, wj->nx2 };
            float jy[2] = { wj->ny1, wj->ny2 };

            /* Test all 4 endpoint pairs */
            for (int ei = 0; ei < 2; ei++) {
                for (int ej = 0; ej < 2; ej++) {
                    if (dist2(ix[ei], iy[ei], jx[ej], jy[ej]) > snap2)
                        continue;
                    /* Connected: add i→j and j→i if room */
                    HwWireNode *ni = &hw_graph_nodes[i];
                    HwWireNode *nj = &hw_graph_nodes[j];
                    if (ni->nb_count < HW_GRAPH_MAX_NEIGHBOURS) {
                        ni->nb[ni->nb_count]    = (int16_t)j;
                        ni->nb_ep[ni->nb_count] = (uint8_t)ei;
                        ni->nb_count++;
                    }
                    if (nj->nb_count < HW_GRAPH_MAX_NEIGHBOURS) {
                        nj->nb[nj->nb_count]    = (int16_t)i;
                        nj->nb_ep[nj->nb_count] = (uint8_t)ej;
                        nj->nb_count++;
                    }
                    goto next_pair; /* at most one match per (i,j) pair */
                }
            }
            next_pair:;
        }
    }
    hw_graph_ready = true;
}

/* BFS over the wire graph within a single net. */
int hw_graph_reachable(int start_wire, int net_id,
                        int16_t *out, int max_out)
{
    if (!hw_graph_ready) hw_graph_build();
    if (start_wire < 0 || start_wire >= HW_WIRE_COUNT) return 0;
    if (hw_wires[start_wire].net_id != net_id)          return 0;
    if (max_out <= 0)                                    return 0;

    /* Visited bitset — 411 wires → 52 bytes */
    uint8_t visited[(HW_WIRE_COUNT + 7) / 8];
    memset(visited, 0, sizeof(visited));

    /* Static BFS queue */
    static int16_t queue[HW_WIRE_COUNT];
    int qhead = 0, qtail = 0;

    visited[start_wire >> 3] |= (uint8_t)(1u << (start_wire & 7));
    queue[qtail++] = (int16_t)start_wire;

    int count = 0;
    while (qhead < qtail && count < max_out) {
        int16_t cur = queue[qhead++];
        out[count++] = cur;

        const HwWireNode *nd = &hw_graph_nodes[cur];
        for (int k = 0; k < nd->nb_count; k++) {
            int16_t nb = nd->nb[k];
            if (nb < 0) break;
            if (visited[nb >> 3] & (1u << (nb & 7))) continue;
            if (hw_wires[nb].net_id != net_id)        continue;
            visited[nb >> 3] |= (uint8_t)(1u << (nb & 7));
            if (qtail < HW_WIRE_COUNT)
                queue[qtail++] = nb;
        }
    }
    return count;
}

int hw_graph_nearest_wire(int net_id, float nx, float ny, float max_dist)
{
    if (!hw_graph_ready) hw_graph_build();
    float best2 = max_dist * max_dist;
    int   best  = -1;
    for (int i = 0; i < HW_WIRE_COUNT; i++) {
        if (hw_wires[i].net_id != net_id) continue;
        float mx = (hw_wires[i].nx1 + hw_wires[i].nx2) * 0.5f;
        float my = (hw_wires[i].ny1 + hw_wires[i].ny2) * 0.5f;
        float d2 = dist2(mx, my, nx, ny);
        if (d2 < best2) { best2 = d2; best = i; }
    }
    return best;
}
