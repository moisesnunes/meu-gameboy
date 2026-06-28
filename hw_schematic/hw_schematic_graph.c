/* hw_schematic_graph.c — Wire connectivity graph.
 *
 * Two wire segments are considered connected when:
 *   1. They share the same net_id, AND
 *   2. At least one endpoint of each wire coincides within SNAP_DIST.
 *
 * SNAP_DIST is set to half a 1.27mm KiCad grid unit in normalised space
 * (1.27mm / 297mm ≈ 0.00428).  We use 0.006 for a little slack.
 */
#include "hw_schematic_graph.h"
#include <string.h>
#include <math.h>

#define SNAP_DIST 0.006f

HwWireNode hw_graph_nodes[HW_GRAPH_WIRE_CAP];
bool       hw_graph_ready = false;

/* Track which dataset the graph was built for. */
static const HwSchematicDataset *s_built_for = NULL;

static int graph_wire_count(const HwSchematicDataset *ds)
{
    if (!ds || !ds->wires || ds->wire_count <= 0)
        return 0;
    return ds->wire_count > HW_GRAPH_WIRE_CAP ? HW_GRAPH_WIRE_CAP : ds->wire_count;
}

static inline float dist2(float ax, float ay, float bx, float by)
{
    float dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy;
}

void hw_graph_invalidate(void)
{
    hw_graph_ready = false;
    s_built_for    = NULL;
}

void hw_graph_build(const HwSchematicDataset *ds)
{
    if (hw_graph_ready && s_built_for == ds) return;

    int n = graph_wire_count(ds);

    memset(hw_graph_nodes, 0xFF, sizeof(hw_graph_nodes));
    if (n == 0) {
        s_built_for    = ds;
        hw_graph_ready = true;
        return;
    }
    for (int i = 0; i < n; i++)
        hw_graph_nodes[i].nb_count = 0;

    float snap2 = SNAP_DIST * SNAP_DIST;
    const HwWire *wires = ds->wires;

    for (int i = 0; i < n; i++) {
        const HwWire *wi = &wires[i];
        if (wi->net_id < 0) continue;

        float ix[2] = { wi->nx1, wi->nx2 };
        float iy[2] = { wi->ny1, wi->ny2 };

        for (int j = i + 1; j < n; j++) {
            const HwWire *wj = &wires[j];
            if (wj->net_id != wi->net_id) continue;

            float jx[2] = { wj->nx1, wj->nx2 };
            float jy[2] = { wj->ny1, wj->ny2 };

            for (int ei = 0; ei < 2; ei++) {
                for (int ej = 0; ej < 2; ej++) {
                    if (dist2(ix[ei], iy[ei], jx[ej], jy[ej]) > snap2)
                        continue;
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
                    goto next_pair;
                }
            }
            next_pair:;
        }
    }
    s_built_for    = ds;
    hw_graph_ready = true;
}

int hw_graph_reachable(const HwSchematicDataset *ds,
                       int start_wire, int net_id,
                       int16_t *out, int max_out)
{
    hw_graph_build(ds);
    int n = graph_wire_count(ds);
    if (start_wire < 0 || start_wire >= n)              return 0;
    if (ds->wires[start_wire].net_id != net_id)         return 0;
    if (max_out <= 0)                                    return 0;

    uint8_t visited[(HW_GRAPH_WIRE_CAP + 7) / 8];
    memset(visited, 0, sizeof(visited));

    static int16_t queue[HW_GRAPH_WIRE_CAP];
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
            if (ds->wires[nb].net_id != net_id)       continue;
            visited[nb >> 3] |= (uint8_t)(1u << (nb & 7));
            if (qtail < HW_GRAPH_WIRE_CAP)
                queue[qtail++] = nb;
        }
    }
    return count;
}

int hw_graph_nearest_wire(const HwSchematicDataset *ds,
                          int net_id, float nx, float ny, float max_dist)
{
    hw_graph_build(ds);
    float best2 = max_dist * max_dist;
    int   best  = -1;
    int   n     = graph_wire_count(ds);
    if (n == 0) return -1;
    const HwWire *wires = ds->wires;
    for (int i = 0; i < n; i++) {
        if (wires[i].net_id != net_id) continue;
        float mx = (wires[i].nx1 + wires[i].nx2) * 0.5f;
        float my = (wires[i].ny1 + wires[i].ny2) * 0.5f;
        float d2 = dist2(mx, my, nx, ny);
        if (d2 < best2) { best2 = d2; best = i; }
    }
    return best;
}
