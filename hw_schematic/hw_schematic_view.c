/* hw_schematic_view.c -- coordinate transform + background texture loader */
#include "hw_schematic_view.h"
#include <stdlib.h>
#include <string.h>
#include <GL/gl.h>

/* Keep stb_image private to this translation unit so other frontends may also
 * define STB_IMAGE_IMPLEMENTATION without link-time symbol collisions. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* nanosvg rasterizer — define implementation once here */
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"
#include "nanosvgrast.h"

unsigned int hw_schematic_bg_texture = 0;

void hw_schematic_view_fit(HwSchematicView *v)
{
     /* zoom is pixels per normalized X unit. Y is scaled by the A4 H/W aspect. */
     v->zoom = v->canvas_w;
     v->pan_x = 0.0f;
     if (v->zoom * HW_SCHEMATIC_ASPECT > v->canvas_h)
     {
          v->zoom = v->canvas_h / HW_SCHEMATIC_ASPECT;
     }
     v->pan_y = 0.0f;
}

static bool hw_schematic_upload_texture(const unsigned char *pixels, int w, int h)
{
     if (!pixels || w <= 0 || h <= 0)
          return false;

     GLint max_tex = 0;
     glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
     if (max_tex > 0 && (w > max_tex || h > max_tex))
          return false;

     hw_schematic_bg_unload();

     glGenTextures(1, &hw_schematic_bg_texture);
     if (!hw_schematic_bg_texture)
          return false;

     glBindTexture(GL_TEXTURE_2D, hw_schematic_bg_texture);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
     glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
     glBindTexture(GL_TEXTURE_2D, 0);

     return true;
}

static bool hw_schematic_bg_load_raster(const char *path)
{
     int w = 0, h = 0, ch = 0;
     unsigned char *pixels = stbi_load(path, &w, &h, &ch, 4);
     if (!pixels)
          return false;

     bool ok = hw_schematic_upload_texture(pixels, w, h);
     stbi_image_free(pixels);
     return ok;
}

static bool hw_schematic_bg_load_svg(const char *path)
{
     NSVGimage *image = nsvgParseFromFile(path, "px", 96.0f);
     if (!image)
          return false;

     int w = (int)(image->width + 0.5f);
     int h = (int)(image->height + 0.5f);
     GLint max_tex = 0;
     glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_tex);
     if (max_tex > 0)
     {
          float scale_limit = 1.0f;
          if (w > max_tex)
               scale_limit = (float)max_tex / (float)w;
          if (h * scale_limit > max_tex)
               scale_limit = (float)max_tex / (float)h;
          if (scale_limit < 1.0f)
          {
               w = (int)(image->width * scale_limit);
               h = (int)(image->height * scale_limit);
          }
     }
     if (w <= 0 || h <= 0)
     {
          nsvgDelete(image);
          return false;
     }

     NSVGrasterizer *rast = nsvgCreateRasterizer();
     if (!rast)
     {
          nsvgDelete(image);
          return false;
     }

     unsigned char *pixels = (unsigned char *)malloc((size_t)(w * h * 4));
     if (!pixels)
     {
          nsvgDeleteRasterizer(rast);
          nsvgDelete(image);
          return false;
     }

     float scale = (float)w / image->width;
     nsvgRasterize(rast, image, 0, 0, scale, pixels, w, h, w * 4);
     nsvgDeleteRasterizer(rast);
     nsvgDelete(image);

     bool ok = hw_schematic_upload_texture(pixels, w, h);
     free(pixels);
     return ok;
}

bool hw_schematic_bg_load(const char *path)
{
     if (hw_schematic_bg_load_raster(path))
          return true;
     return hw_schematic_bg_load_svg(path);
}

void hw_schematic_bg_unload(void)
{
     if (hw_schematic_bg_texture)
     {
          glDeleteTextures(1, &hw_schematic_bg_texture);
          hw_schematic_bg_texture = 0;
     }
}

void hw_schematic_view_cache(const HwSchematicView *v, HwSchematicCache *c)
{
     c->scale_x = v->zoom;
     c->scale_y = v->zoom * HW_SCHEMATIC_ASPECT;
     c->origin_x = v->canvas_x - v->pan_x * c->scale_x;
     c->origin_y = v->canvas_y - v->pan_y * c->scale_y;

     /* Viewport bounds in normalized schematic space */
     c->vp_nx0 = v->pan_x;
     c->vp_ny0 = v->pan_y;
     c->vp_nx1 = v->pan_x + v->canvas_w / c->scale_x;
     c->vp_ny1 = v->pan_y + v->canvas_h / c->scale_y;
}
