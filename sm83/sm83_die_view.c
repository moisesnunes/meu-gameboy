#include "sm83_die_view.h"
#include "sm83_netlist_data.h"
#include <math.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>

static float sm83_die_aspect(void)
{
     float w = (float)(SM83_BBOX_X_MAX - SM83_BBOX_X_MIN);
     float h = (float)(SM83_BBOX_Y_MAX - SM83_BBOX_Y_MIN);
     return h > 0.0f ? w / h : 1.0f;
}

static void sm83_view_metrics(const Sm83ViewTransform *t,
                              float *origin_x, float *origin_y,
                              float *scale_x, float *scale_y)
{
     float canvas_w = t->canvas_w > 1.0f ? t->canvas_w : 1.0f;
     float canvas_h = t->canvas_h > 1.0f ? t->canvas_h : 1.0f;
     float die_aspect = sm83_die_aspect();
     float fit_w = canvas_w;
     float fit_h = fit_w / die_aspect;

     if (fit_h > canvas_h)
     {
          fit_h = canvas_h;
          fit_w = fit_h * die_aspect;
     }

     *scale_x = fit_w * t->zoom;
     *scale_y = fit_h * t->zoom;
     *origin_x = t->canvas_x + (canvas_w - *scale_x) * 0.5f;
     *origin_y = t->canvas_y + (canvas_h - *scale_y) * 0.5f;
}

void sm83_view_cache_update(const Sm83ViewTransform *t, Sm83ViewCache *c)
{
     float origin_x, origin_y, scale_x, scale_y;
     sm83_view_metrics(t, &origin_x, &origin_y, &scale_x, &scale_y);

     /* The pan is baked into the origin so fast transforms don't need it */
     c->origin_x = origin_x - t->pan_x * scale_x;
     c->origin_y = origin_y - t->pan_y * scale_y;
     c->scale_x  = scale_x;
     c->scale_y  = scale_y;

     /* Viewport bounds in normalized die coords */
     float canvas_w = t->canvas_w > 1.0f ? t->canvas_w : 1.0f;
     float canvas_h = t->canvas_h > 1.0f ? t->canvas_h : 1.0f;
     c->vlo_x = (t->canvas_x - c->origin_x) / scale_x;
     c->vlo_y = (t->canvas_y - c->origin_y) / scale_y;
     c->vhi_x = (t->canvas_x + canvas_w - c->origin_x) / scale_x;
     c->vhi_y = (t->canvas_y + canvas_h - c->origin_y) / scale_y;
}

void sm83_view_fit(Sm83ViewTransform *t)
{
     /* Pan centered, zoom so the die fills the canvas */
     t->pan_x = 0.0f;
     t->pan_y = 0.0f;
     t->zoom = 1.0f;
}

void sm83_die_to_screen(const Sm83ViewTransform *t, float nx, float ny,
                        float *sx, float *sy)
{
     float origin_x, origin_y, scale_x, scale_y;
     sm83_view_metrics(t, &origin_x, &origin_y, &scale_x, &scale_y);
     *sx = origin_x + (nx - t->pan_x) * scale_x;
     *sy = origin_y + (ny - t->pan_y) * scale_y;
}

void sm83_screen_to_die(const Sm83ViewTransform *t, float sx, float sy,
                        float *nx, float *ny)
{
     float origin_x, origin_y, scale_x, scale_y;
     sm83_view_metrics(t, &origin_x, &origin_y, &scale_x, &scale_y);
     *nx = (sx - origin_x) / scale_x + t->pan_x;
     *ny = (sy - origin_y) / scale_y + t->pan_y;
}

void sm83_view_screen_scale(const Sm83ViewTransform *t, float *scale_x, float *scale_y)
{
     float origin_x, origin_y;
     sm83_view_metrics(t, &origin_x, &origin_y, scale_x, scale_y);
}

bool sm83_in_viewport(const Sm83ViewTransform *t, float nx, float ny,
                      float margin_norm)
{
     float x0, y0, x1, y1;
     sm83_screen_to_die(t, t->canvas_x, t->canvas_y, &x0, &y0);
     sm83_screen_to_die(t, t->canvas_x + t->canvas_w, t->canvas_y + t->canvas_h, &x1, &y1);

     float lo_x = (x0 < x1 ? x0 : x1) - margin_norm;
     float lo_y = (y0 < y1 ? y0 : y1) - margin_norm;
     float hi_x = (x0 > x1 ? x0 : x1) + margin_norm;
     float hi_y = (y0 > y1 ? y0 : y1) + margin_norm;
     return nx >= lo_x && nx <= hi_x && ny >= lo_y && ny <= hi_y;
}

bool sm83_arc_in_viewport(const Sm83ViewTransform *t,
                          float nx0, float ny0, float nx1, float ny1)
{
     float vx0, vy0, vx1, vy1;
     sm83_screen_to_die(t, t->canvas_x, t->canvas_y, &vx0, &vy0);
     sm83_screen_to_die(t, t->canvas_x + t->canvas_w, t->canvas_y + t->canvas_h, &vx1, &vy1);

     float lo_x = vx0 < vx1 ? vx0 : vx1;
     float lo_y = vy0 < vy1 ? vy0 : vy1;
     float hi_x = vx0 > vx1 ? vx0 : vx1;
     float hi_y = vy0 > vy1 ? vy0 : vy1;

     float arc_lo_x = nx0 < nx1 ? nx0 : nx1;
     float arc_lo_y = ny0 < ny1 ? ny0 : ny1;
     float arc_hi_x = nx0 > nx1 ? nx0 : nx1;
     float arc_hi_y = ny0 > ny1 ? ny0 : ny1;

     return arc_hi_x >= lo_x && arc_lo_x <= hi_x &&
            arc_hi_y >= lo_y && arc_lo_y <= hi_y;
}

bool sm83_hit_test(const Sm83ViewTransform *t, float sx, float sy,
                   float radius_px, unsigned int layer_mask,
                   Sm83Selection *sel)
{
     float best_dist2 = radius_px * radius_px;
     sel->type = SM83_SEL_NONE;
     sel->index = -1;

     /* Precompute projection and convert the search window to die-norm coords.
      * This lets us skip die_to_screen() for distant elements with a cheap
      * AABB test (2 comparisons) before doing the full distance computation. */
     float origin_x, origin_y, scale_x, scale_y;
     sm83_view_metrics(t, &origin_x, &origin_y, &scale_x, &scale_y);

     float baked_ox = origin_x - t->pan_x * scale_x;
     float baked_oy = origin_y - t->pan_y * scale_y;

     /* mouse in normalized die coords */
     float mnx = (sx - baked_ox) / scale_x;
     float mny = (sy - baked_oy) / scale_y;

     /* search radius in die-norm coords (each axis may differ) */
     float r_nx = radius_px / scale_x;
     float r_ny = radius_px / scale_y;

     /* Check transistors */
     if (layer_mask & (SM83_VIEW_LAYER_NTRANS | SM83_VIEW_LAYER_PTRANS))
     {
          for (int i = 0; i < SM83_TRANSISTOR_COUNT; i++)
          {
               const Sm83Transistor *tr = &sm83_transistors[i];
               unsigned int lbit = 1u << tr->layer;
               if (!(layer_mask & lbit))
                    continue;

               /* Quick AABB reject in die-norm space */
               float dnx = tr->nx - mnx, dny = tr->ny - mny;
               if (dnx < -r_nx || dnx > r_nx || dny < -r_ny || dny > r_ny)
                    continue;

               /* Exact distance in screen pixels */
               float dx = dnx * scale_x, dy = dny * scale_y;
               float d2 = dx * dx + dy * dy;
               if (d2 < best_dist2)
               {
                    best_dist2 = d2;
                    sel->type = SM83_SEL_TRANSISTOR;
                    sel->index = i;
               }
          }
     }

     /* Check geometry nodes */
     for (int i = 0; i < SM83_NODE_COUNT; i++)
     {
          const Sm83Node *n = &sm83_nodes[i];
          unsigned int lbit = 1u << n->layer;
          if (!(layer_mask & lbit))
               continue;

          float dnx = n->nx - mnx, dny = n->ny - mny;
          if (dnx < -r_nx || dnx > r_nx || dny < -r_ny || dny > r_ny)
               continue;

          float dx = dnx * scale_x, dy = dny * scale_y;
          float d2 = dx * dx + dy * dy;
          if (d2 < best_dist2)
          {
               best_dist2 = d2;
               sel->type = SM83_SEL_NODE;
               sel->index = i;
          }
     }

     return sel->type != SM83_SEL_NONE;
}

int sm83_node_arcs(int node_idx, int *out_arcs, int max)
{
     int count = 0;
     for (int i = 0; i < SM83_ARC_COUNT && count < max; i++)
     {
          const Sm83Arc *a = &sm83_arcs[i];
          if (a->tail_node == node_idx || a->head_node == node_idx)
               out_arcs[count++] = i;
     }
     return count;
}

/* -----------------------------------------------------------------------
 * Adjacency index for O(degree) BFS instead of O(N_arcs) per node.
 * Built once on first call to sm83_net_flood().
 * adj_head[node] = first index into adj_list[] for that node (-1 = none).
 * adj_list[k] = { arc_index, next_k } singly-linked list.
 * ---------------------------------------------------------------------- */
typedef struct
{
     int arc;
     int next;
} AdjEntry;
static int *s_adj_head = NULL;      /* SM83_NODE_COUNT entries */
static AdjEntry *s_adj_list = NULL; /* 2 * SM83_ARC_COUNT entries (each arc appears twice) */
static int s_adj_built = 0;

static void build_adjacency(void)
{
     if (s_adj_built)
          return;

     s_adj_head = (int *)malloc(SM83_NODE_COUNT * sizeof(int));
     s_adj_list = (AdjEntry *)malloc(2 * SM83_ARC_COUNT * sizeof(AdjEntry));
     if (!s_adj_head || !s_adj_list)
          return;

     memset(s_adj_head, -1, SM83_NODE_COUNT * sizeof(int));

     int slot = 0;
     for (int i = 0; i < SM83_ARC_COUNT; i++)
     {
          const Sm83Arc *a = &sm83_arcs[i];
          if (a->tail_node >= 0 && a->tail_node < SM83_NODE_COUNT)
          {
               s_adj_list[slot].arc = i;
               s_adj_list[slot].next = s_adj_head[a->tail_node];
               s_adj_head[a->tail_node] = slot++;
          }
          if (a->head_node >= 0 && a->head_node < SM83_NODE_COUNT && a->head_node != a->tail_node)
          {
               s_adj_list[slot].arc = i;
               s_adj_list[slot].next = s_adj_head[a->head_node];
               s_adj_head[a->head_node] = slot++;
          }
     }
     s_adj_built = 1;
}

int sm83_net_flood(int start_node, uint8_t *arc_flags, uint8_t *node_flags)
{
     if (start_node < 0 || start_node >= SM83_NODE_COUNT)
          return 0;

     build_adjacency();
     if (!s_adj_head || !s_adj_list)
          return 0;

     /* BFS queue — maximum frontier is SM83_NODE_COUNT */
     int *queue = (int *)malloc(SM83_NODE_COUNT * sizeof(int));
     if (!queue)
          return 0;

     int arc_count = 0;
     int head = 0, tail = 0;

     node_flags[start_node] = 1;
     queue[tail++] = start_node;

     while (head < tail)
     {
          int node = queue[head++];
          for (int k = s_adj_head[node]; k != -1; k = s_adj_list[k].next)
          {
               int arc_idx = s_adj_list[k].arc;
               if (!arc_flags[arc_idx])
               {
                    arc_flags[arc_idx] = 1;
                    arc_count++;
               }
               const Sm83Arc *a = &sm83_arcs[arc_idx];
               int neighbor = (a->tail_node == node) ? a->head_node : a->tail_node;
               if (neighbor >= 0 && neighbor < SM83_NODE_COUNT && !node_flags[neighbor])
               {
                    node_flags[neighbor] = 1;
                    queue[tail++] = neighbor;
               }
          }
     }

     free(queue);
     return arc_count;
}
