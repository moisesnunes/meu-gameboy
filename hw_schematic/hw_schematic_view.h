/* hw_schematic_view.h -- KiCad schematic viewer: coordinate transform + texture */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Normalized coordinate space: [0,1] x [0,1] from KiCad A4 paper (297x210mm).
 * nx = kicad_x / 297,  ny = kicad_y / 210 */

#define HW_SCHEMATIC_PAPER_W_MM 297.0f
#define HW_SCHEMATIC_PAPER_H_MM 210.0f
#define HW_SCHEMATIC_ASPECT (HW_SCHEMATIC_PAPER_H_MM / HW_SCHEMATIC_PAPER_W_MM)

typedef struct
{
     float canvas_x, canvas_y; /* ImGui canvas top-left (screen px) */
     float canvas_w, canvas_h; /* ImGui canvas size (screen px) */
     float zoom;               /* pixels per normalized unit; default = canvas_w */
     float pan_x, pan_y;       /* pan offset in normalized units */
} HwSchematicView;

typedef struct
{
     float origin_x, origin_y; /* screen px where normalized (0,0) maps */
     float scale_x, scale_y;   /* screen px per normalized unit */
     /* viewport bounds in normalized space (for culling) */
     float vp_nx0, vp_ny0, vp_nx1, vp_ny1;
} HwSchematicCache;

/* OpenGL texture handle for the background schematic/board image.
 * 0 = not loaded. */
extern unsigned int hw_schematic_bg_texture;

/* Initialize a view to fit the schematic in the canvas. */
void hw_schematic_view_fit(HwSchematicView *v);

/* Load (or reload) a JPG/PNG/BMP/TGA or fallback SVG background as an OpenGL texture.
 * path = filesystem path. Returns true on success. */
bool hw_schematic_bg_load(const char *path);

/* Delete the current OpenGL texture, if any. */
void hw_schematic_bg_unload(void);

/* Precompute cache from current view state. Call once per frame before drawing. */
void hw_schematic_view_cache(const HwSchematicView *v, HwSchematicCache *c);

/* Convert normalized schematic coords to screen pixels. */
static inline float hw_sch_to_screen_x(const HwSchematicCache *c, float nx)
{
     return c->origin_x + nx * c->scale_x;
}
static inline float hw_sch_to_screen_y(const HwSchematicCache *c, float ny)
{
     return c->origin_y + ny * c->scale_y;
}

/* Is a normalized point inside the current viewport? */
static inline bool hw_sch_in_viewport(const HwSchematicCache *c, float nx, float ny)
{
     return nx >= c->vp_nx0 && nx <= c->vp_nx1 &&
            ny >= c->vp_ny0 && ny <= c->vp_ny1;
}

/* Is a normalized line segment at all inside the viewport? */
static inline bool hw_sch_line_in_viewport(const HwSchematicCache *c,
                                           float nx1, float ny1,
                                           float nx2, float ny2)
{
     float xmin = nx1 < nx2 ? nx1 : nx2;
     float xmax = nx1 > nx2 ? nx1 : nx2;
     float ymin = ny1 < ny2 ? ny1 : ny2;
     float ymax = ny1 > ny2 ? ny1 : ny2;
     return xmax >= c->vp_nx0 && xmin <= c->vp_nx1 &&
            ymax >= c->vp_ny0 && ymin <= c->vp_ny1;
}
