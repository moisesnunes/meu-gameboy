#ifndef SM83_DIE_VIEW_H
#define SM83_DIE_VIEW_H

#include <stdbool.h>
#include <stdint.h>

/* sm83_die_view.h
 * Visual model for the SM83 transistor-level die viewer.
 * Provides coordinate transforms, layer visibility, viewport culling,
 * and hit-test for the ImGui panel in debug_ui_panels.cpp.
 */

/* -----------------------------------------------------------------------
 * Layer flags (bit positions match SM83_LAYER_* in sm83_netlist_data.h)
 * ---------------------------------------------------------------------- */
#define SM83_VIEW_LAYER_METAL1 (1u << 0)
#define SM83_VIEW_LAYER_POLY (1u << 1)
#define SM83_VIEW_LAYER_NACTIVE (1u << 2)
#define SM83_VIEW_LAYER_PACTIVE (1u << 3)
#define SM83_VIEW_LAYER_NTRANS (1u << 4)
#define SM83_VIEW_LAYER_PTRANS (1u << 5)
#define SM83_VIEW_LAYER_OVERLAY (1u << 6)
#define SM83_VIEW_LAYER_ALL (0x7Fu)

/* -----------------------------------------------------------------------
 * View transform: maps normalized die coords [0,1]x[0,1] -> screen pixels.
 * The mapping preserves the physical SM83 bounding-box aspect ratio.
 * ---------------------------------------------------------------------- */
typedef struct
{
     float canvas_x, canvas_y; /* top-left of canvas in screen pixels */
     float canvas_w, canvas_h; /* canvas size */
     float zoom;               /* pixels per die-unit (1.0 = canvas fill) */
     float pan_x, pan_y;       /* offset in die-normalized units */
} Sm83ViewTransform;

/* -----------------------------------------------------------------------
 * Selection / hover state
 * ---------------------------------------------------------------------- */
typedef enum
{
     SM83_SEL_NONE = 0,
     SM83_SEL_TRANSISTOR = 1,
     SM83_SEL_NODE = 2,
     SM83_SEL_INSTANCE = 3,
} Sm83SelType;

typedef struct
{
     Sm83SelType type;
     int index; /* index into sm83_transistors[] / sm83_nodes[] / sm83_instances[] */
} Sm83Selection;

/* -----------------------------------------------------------------------
 * Cached projection: precomputed once per frame to avoid repeating the
 * aspect-fit calculation inside every die_to_screen / in_viewport call.
 * ---------------------------------------------------------------------- */
typedef struct
{
     float origin_x, origin_y; /* canvas-local top-left of fitted die rect */
     float scale_x, scale_y;   /* pixels per normalized die unit */
     /* viewport bounds in normalized die coords (for culling) */
     float vlo_x, vlo_y, vhi_x, vhi_y;
} Sm83ViewCache;

/* Compute the cache from a transform. Call once per frame before drawing. */
void sm83_view_cache_update(const Sm83ViewTransform *t, Sm83ViewCache *c);

/* Fast (no recompute) die -> screen using precomputed cache */
static inline void sm83_die_to_screen_fast(const Sm83ViewCache *c,
                                           float nx, float ny,
                                           float *sx, float *sy)
{
     *sx = c->origin_x + nx * c->scale_x;
     *sy = c->origin_y + ny * c->scale_y;
}

/* Fast viewport cull for a point */
static inline bool sm83_in_viewport_fast(const Sm83ViewCache *c,
                                         float nx, float ny, float margin)
{
     return nx >= c->vlo_x - margin && nx <= c->vhi_x + margin &&
            ny >= c->vlo_y - margin && ny <= c->vhi_y + margin;
}

/* Fast viewport cull for a line segment (conservative AABB) */
static inline bool sm83_arc_in_viewport_fast(const Sm83ViewCache *c,
                                             float nx0, float ny0,
                                             float nx1, float ny1)
{
     float lo_x = nx0 < nx1 ? nx0 : nx1;
     float lo_y = ny0 < ny1 ? ny0 : ny1;
     float hi_x = nx0 > nx1 ? nx0 : nx1;
     float hi_y = ny0 > ny1 ? ny0 : ny1;
     return hi_x >= c->vlo_x && lo_x <= c->vhi_x &&
            hi_y >= c->vlo_y && lo_y <= c->vhi_y;
}

/* -----------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/* Initialize default transform so the die fits the canvas */
void sm83_view_fit(Sm83ViewTransform *t);

/* Convert die normalized coordinates to canvas-local screen coordinates */
void sm83_die_to_screen(const Sm83ViewTransform *t, float nx, float ny,
                        float *sx, float *sy);

/* Convert screen coordinates (canvas-local) to die normalized coordinates */
void sm83_screen_to_die(const Sm83ViewTransform *t, float sx, float sy,
                        float *nx, float *ny);

/* Current pixel scale for one normalized die unit, after aspect fitting. */
void sm83_view_screen_scale(const Sm83ViewTransform *t, float *scale_x, float *scale_y);

/* Returns true if the point (nx, ny) maps to inside the current viewport */
bool sm83_in_viewport(const Sm83ViewTransform *t, float nx, float ny,
                      float margin_norm);

/* Returns true if the line segment from (nx0,ny0) to (nx1,ny1) is
 * at least partially inside the viewport (conservative AABB check) */
bool sm83_arc_in_viewport(const Sm83ViewTransform *t,
                          float nx0, float ny0, float nx1, float ny1);

/* Hit-test: find the closest transistor/node under screen position (sx,sy).
 * radius_px is the maximum hit distance in screen pixels.
 * Populates *sel; returns false if nothing was hit. */
bool sm83_hit_test(const Sm83ViewTransform *t, float sx, float sy,
                   float radius_px, unsigned int layer_mask,
                   Sm83Selection *sel);

/* Fill out_arcs[] with indices of arcs whose tail_node or head_node equals
 * node_idx (index into sm83_nodes[]). Returns the count found (up to max).
 * Only arcs with valid connectivity (tail_node != -1 or head_node != -1) are
 * considered. */
int sm83_node_arcs(int node_idx, int *out_arcs, int max);

/* BFS flood-fill from start_node across arc connectivity.
 * Marks every arc reachable by following tail_node/head_node links into
 * arc_flags[] (one byte per arc, set to 1 if in net).
 * Also marks visited nodes into node_flags[] (one byte per node, set to 1).
 * arc_flags must have SM83_ARC_COUNT bytes; node_flags SM83_NODE_COUNT bytes.
 * Both arrays must be zeroed by the caller before the first call.
 * Returns the number of arcs marked. */
int sm83_net_flood(int start_node, uint8_t *arc_flags, uint8_t *node_flags);

/* Electrical net highlight: mark all arcs whose net_id == net_id into arc_flags[],
 * and all transistors with gate/s1/s2 == net_id into trans_flags[] (may be NULL).
 * arc_flags must have SM83_ARC_COUNT bytes; trans_flags SM83_TRANSISTOR_COUNT bytes.
 * Both must be zeroed by caller. Returns number of arcs marked. */
int sm83_net_highlight_by_netid(int net_id,
                                uint8_t *arc_flags,
                                uint8_t *trans_flags);

#endif /* SM83_DIE_VIEW_H */
