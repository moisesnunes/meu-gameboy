#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "gba.h"

/* Cycles from start of line to HBlank start (dot 240) */
#define GBA_CYCLES_HBLANK_START (240 * GBA_CYCLES_PER_DOT) /* 960 */

static void latch_affine_line(struct gba_gpu *gpu)
{
     for (int b = 2; b <= 3; b++)
     {
          struct gba_bg_layer *bg = &gpu->bg[b];
          bg->pa_line = bg->pa;
          bg->pb_line = bg->pb;
          bg->pc_line = bg->pc;
          bg->pd_line = bg->pd;
          bg->ref_x_line = bg->ref_x_latch;
          bg->ref_y_line = bg->ref_y_latch;
     }
}

void gba_gpu_reset(struct gba *gba)
{
     struct gba_gpu *gpu = &gba->gpu;
     memset(gpu, 0, sizeof(*gpu));
     gpu->vcount = 0;
     gpu->hblank_flag = false;
     latch_affine_line(gpu);
     /* First event: HBlank start at dot 240 of line 0 */
     gba_sync_next(gba, GBA_SYNC_GPU, GBA_CYCLES_HBLANK_START);
}

/* -------------------------------------------------------------------------
 * Palette helpers
 * ---------------------------------------------------------------------- */

static uint16_t read_pal16(struct gba *gba, int idx)
{
     idx &= 0xFF;
     return (uint16_t)(gba->pram[idx * 2] | (gba->pram[idx * 2 + 1] << 8));
}

static uint16_t read_obj_pal16(struct gba *gba, int idx)
{
     idx = 256 + (idx & 0xFF);
     return (uint16_t)(gba->pram[idx * 2] | (gba->pram[idx * 2 + 1] << 8));
}

/* -------------------------------------------------------------------------
 * Tile rendering helpers
 * ---------------------------------------------------------------------- */

static uint8_t vram_tile_pixel_256(struct gba *gba, uint32_t tile_addr,
                                   int tx, int ty)
{
     uint32_t addr = tile_addr + (uint32_t)(ty * 8 + tx);
     if (addr >= GBA_VRAM_SIZE)
          return 0;
     return gba->vram[addr];
}

static uint8_t vram_tile_pixel_16(struct gba *gba, uint32_t tile_addr,
                                  int tx, int ty)
{
     uint32_t addr = tile_addr + (uint32_t)(ty * 4 + tx / 2);
     if (addr >= GBA_VRAM_SIZE)
          return 0;
     uint8_t b = gba->vram[addr];
     return (tx & 1) ? (b >> 4) : (b & 0xF);
}

/* -------------------------------------------------------------------------
 * Window helpers
 * ---------------------------------------------------------------------- */

/*
 * Returns the effective enable-bits for a given pixel (x) on a given line.
 * Bits: 0=BG0, 1=BG1, 2=BG2, 3=BG3, 4=OBJ, 5=color-special
 * When no windows are active every bit is 1 (all visible).
 */
static uint8_t window_flags(const struct gba_gpu *gpu, int x, uint8_t line)
{
     bool any_win = gpu->win0_en || gpu->win1_en || gpu->winobj_en;
     if (!any_win)
          return 0x3F;

     /* Check win0 */
     if (gpu->win0_en)
     {
          bool in_x = (gpu->win0_x1 <= gpu->win0_x2)
                          ? (x >= gpu->win0_x1 && x < gpu->win0_x2)
                          : (x >= gpu->win0_x1 || x < gpu->win0_x2);
          bool in_y = (gpu->win0_y1 <= gpu->win0_y2)
                          ? (line >= gpu->win0_y1 && line < gpu->win0_y2)
                          : (line >= gpu->win0_y1 || line < gpu->win0_y2);
          if (in_x && in_y)
               return gpu->winin & 0x3F;
     }

     /* Check win1 */
     if (gpu->win1_en)
     {
          bool in_x = (gpu->win1_x1 <= gpu->win1_x2)
                          ? (x >= gpu->win1_x1 && x < gpu->win1_x2)
                          : (x >= gpu->win1_x1 || x < gpu->win1_x2);
          bool in_y = (gpu->win1_y1 <= gpu->win1_y2)
                          ? (line >= gpu->win1_y1 && line < gpu->win1_y2)
                          : (line >= gpu->win1_y1 || line < gpu->win1_y2);
          if (in_x && in_y)
               return (gpu->winin >> 8) & 0x3F;
     }

     /* Obj-window: pixel must be in an OBJ with gfx_mode==2 */
     if (gpu->winobj_en)
     {
          /* obj_mode_buf is filled by sprite rendering; bit 1 of winout selects obj-win */
          /* We can't check obj_mode_buf here (called before sprites drawn for BG),
             so we defer obj-win to the compositor. Return WINOUT outside flags for now. */
     }

     /* Outside all windows */
     return (gpu->winout) & 0x3F;
}

/* -------------------------------------------------------------------------
 * BG tile mode rendering (modes 0-2)
 * ---------------------------------------------------------------------- */

static void render_bg_tile(struct gba *gba, int bg_idx, uint16_t *line_buf,
                           uint8_t *prio_buf, uint8_t line)
{
     struct gba_gpu *gpu = &gba->gpu;
     struct gba_bg_layer *bg = &gpu->bg[bg_idx];

     uint32_t map_base = (uint32_t)bg->map_base * 0x800;
     uint32_t tile_base = (uint32_t)bg->tile_base * 0x4000;
     bool colors256 = bg->colors_256;

     bool any_win = gpu->win0_en || gpu->win1_en || gpu->winobj_en;
     uint8_t bg_bit = (uint8_t)(1 << bg_idx);

     int map_w_tiles, map_h_tiles;
     switch (bg->size)
     {
     case 0:
          map_w_tiles = 32;
          map_h_tiles = 32;
          break;
     case 1:
          map_w_tiles = 64;
          map_h_tiles = 32;
          break;
     case 2:
          map_w_tiles = 32;
          map_h_tiles = 64;
          break;
     case 3:
          map_w_tiles = 64;
          map_h_tiles = 64;
          break;
     default:
          return;
     }

     int mosaic_h = bg->mosaic ? (int)(gpu->bg_mosaic_h + 1) : 1;
     int mosaic_v = bg->mosaic ? (int)(gpu->bg_mosaic_v + 1) : 1;
     int snapped_line = (line / mosaic_v) * mosaic_v;

     int y = (snapped_line + bg->vofs) & ((map_h_tiles * 8) - 1);
     int tile_y = y / 8;
     int py = y % 8;

     for (int x = 0; x < GBA_LCD_W; x++)
     {
          /* Window check */
          if (any_win && !(window_flags(gpu, x, line) & bg_bit))
               continue;

          int sx_x = (x / mosaic_h) * mosaic_h;
          int sx = (sx_x + bg->hofs) & ((map_w_tiles * 8) - 1);
          int tile_x = sx / 8;
          int px = sx % 8;

          int sbb = 0;
          int tx = tile_x, ty = tile_y;
          if (map_w_tiles == 64)
          {
               if (tx >= 32)
               {
                    sbb += 1;
                    tx -= 32;
               }
          }
          if (map_h_tiles == 64)
          {
               if (ty >= 32)
               {
                    sbb += (map_w_tiles == 64) ? 2 : 1;
                    ty -= 32;
               }
          }

          uint32_t map_addr = map_base + (uint32_t)(sbb * 0x800 + ty * 64 + tx * 2);
          if (map_addr + 1 >= GBA_VRAM_SIZE)
               continue;
          uint16_t entry = (uint16_t)(gba->vram[map_addr] | (gba->vram[map_addr + 1] << 8));

          uint16_t tile_num = entry & 0x3FF;
          bool hflip = (entry >> 10) & 1;
          bool vflip = (entry >> 11) & 1;
          uint8_t palette = (entry >> 12) & 0xF;

          int fpx = hflip ? 7 - px : px;
          int fpy = vflip ? 7 - py : py;

          uint8_t color_idx;
          uint16_t color;
          if (colors256)
          {
               uint32_t taddr = tile_base + tile_num * 64;
               color_idx = vram_tile_pixel_256(gba, taddr, fpx, fpy);
               if (color_idx == 0)
                    continue;
               color = read_pal16(gba, color_idx);
          }
          else
          {
               uint32_t taddr = tile_base + tile_num * 32;
               color_idx = vram_tile_pixel_16(gba, taddr, fpx, fpy);
               if (color_idx == 0)
                    continue;
               color = read_pal16(gba, palette * 16 + color_idx);
          }

          if (bg->priority <= prio_buf[x])
          {
               line_buf[x] = color & 0x7FFF;
               prio_buf[x] = bg->priority;
          }
     }
}

/* -------------------------------------------------------------------------
 * BG affine rendering (BG2/BG3 in modes 1-2, or mode 3/4/5)
 * ---------------------------------------------------------------------- */

static void render_bg_affine(struct gba *gba, int bg_idx, uint16_t *line_buf,
                             uint8_t *prio_buf, uint8_t line)
{
     struct gba_gpu *gpu = &gba->gpu;
     struct gba_bg_layer *bg = &gpu->bg[bg_idx];

     uint32_t map_base = (uint32_t)bg->map_base * 0x800;
     uint32_t tile_base = (uint32_t)bg->tile_base * 0x4000;

     bool any_win = gpu->win0_en || gpu->win1_en || gpu->winobj_en;
     uint8_t bg_bit = (uint8_t)(1 << bg_idx);

     int map_size_tiles;
     switch (bg->size)
     {
     case 0:
          map_size_tiles = 16;
          break;
     case 1:
          map_size_tiles = 32;
          break;
     case 2:
          map_size_tiles = 64;
          break;
     case 3:
          map_size_tiles = 128;
          break;
     default:
          return;
     }
     int map_size_px = map_size_tiles * 8;

     int mosaic_h = bg->mosaic ? (int)(gpu->bg_mosaic_h + 1) : 1;
     int mosaic_v = bg->mosaic ? (int)(gpu->bg_mosaic_v + 1) : 1;

     /* For affine BG, mosaic snaps the reference point per-block */
     int16_t pa = bg->pa_line;
     int16_t pb = bg->pb_line;
     int16_t pc = bg->pc_line;
     int16_t pd = bg->pd_line;
     int32_t rx_start = bg->ref_x_line;
     int32_t ry_start = bg->ref_y_line;
     if (bg->mosaic)
     {
          int snapped_line = (line / mosaic_v) * mosaic_v;
          int line_delta = snapped_line - (int)line;
          rx_start += (int32_t)line_delta * pb;
          ry_start += (int32_t)line_delta * pd;
     }
     int32_t rx = rx_start;
     int32_t ry = ry_start;

     for (int x = 0; x < GBA_LCD_W; x++)
     {
          if (any_win && !(window_flags(gpu, x, line) & bg_bit))
          {
               rx += pa;
               ry += pc;
               continue;
          }

          int32_t eff_rx = bg->mosaic ? rx_start + (int32_t)((x / mosaic_h) * mosaic_h) * pa : rx;
          int32_t eff_ry = bg->mosaic ? ry_start + (int32_t)((x / mosaic_h) * mosaic_h) * pc : ry;
          int sx = eff_rx >> 8;
          int sy = eff_ry >> 8;

          if (bg->wrap)
          {
               sx = ((sx % map_size_px) + map_size_px) % map_size_px;
               sy = ((sy % map_size_px) + map_size_px) % map_size_px;
          }
          else if (sx < 0 || sx >= map_size_px || sy < 0 || sy >= map_size_px)
          {
               rx += pa;
               ry += pc;
               continue;
          }

          int tile_x = sx / 8, tile_y = sy / 8;
          int px = sx % 8, py = sy % 8;

          uint32_t map_addr = map_base + (uint32_t)(tile_y * map_size_tiles + tile_x);
          if (map_addr >= GBA_VRAM_SIZE)
          {
               rx += pa;
               ry += pc;
               continue;
          }
          uint8_t tile_num = gba->vram[map_addr];

          uint32_t taddr = tile_base + tile_num * 64;
          uint8_t color_idx = vram_tile_pixel_256(gba, taddr, px, py);
          if (color_idx != 0)
          {
               uint16_t color = read_pal16(gba, color_idx);
               if (bg->priority <= prio_buf[x])
               {
                    line_buf[x] = color & 0x7FFF;
                    prio_buf[x] = bg->priority;
               }
          }

          rx += pa;
          ry += pc;
     }

     bg->ref_x_latch += pb;
     bg->ref_y_latch += pd;
}

/* -------------------------------------------------------------------------
 * Bitmap modes
 * ---------------------------------------------------------------------- */

static void render_mode3(struct gba *gba, uint16_t *line_buf, uint8_t line)
{
     struct gba_gpu *gpu = &gba->gpu;
     struct gba_bg_layer *bg = &gpu->bg[2];
     int16_t pa = bg->pa_line;
     int16_t pb = bg->pb_line;
     int16_t pc = bg->pc_line;
     int16_t pd = bg->pd_line;
     int32_t rx = bg->ref_x_line;
     int32_t ry = bg->ref_y_line;

     for (int x = 0; x < GBA_LCD_W; x++)
     {
          int sx = rx >> 8;
          int sy = ry >> 8;

          rx += pa;
          ry += pc;

          if (sx < 0 || sx >= GBA_LCD_W || sy < 0 || sy >= GBA_LCD_H)
               continue;

          uint32_t addr = ((uint32_t)sy * GBA_LCD_W + (uint32_t)sx) * 2;
          if (addr + 1 < GBA_VRAM_SIZE)
          {
               uint16_t c = (uint16_t)(gba->vram[addr] | (gba->vram[addr + 1] << 8));
               line_buf[x] = c & 0x7FFF;
          }
     }

     bg->ref_x_latch += pb;
     bg->ref_y_latch += pd;
}

static void render_mode4(struct gba *gba, uint16_t *line_buf, uint8_t line)
{
     struct gba_gpu *gpu = &gba->gpu;
     uint32_t base = gpu->frame_select ? 0xA000U : 0x0000U;
     base += (uint32_t)(line)*GBA_LCD_W;
     for (int x = 0; x < GBA_LCD_W; x++)
     {
          uint32_t addr = base + (uint32_t)x;
          if (addr < GBA_VRAM_SIZE)
               line_buf[x] = read_pal16(gba, gba->vram[addr]) & 0x7FFF;
     }
}

static void render_mode5(struct gba *gba, uint16_t *line_buf, uint8_t line)
{
     struct gba_gpu *gpu = &gba->gpu;
     if (line >= 128)
          return;
     uint32_t base = gpu->frame_select ? 0xA000U : 0x0000U;
     base += (uint32_t)(line) * 160 * 2;
     for (int x = 0; x < GBA_LCD_W; x++)
     {
          if (x < 160)
          {
               uint32_t addr = base + (uint32_t)(x * 2);
               if (addr + 1 < GBA_VRAM_SIZE)
                    line_buf[x] = (uint16_t)(gba->vram[addr] | (gba->vram[addr + 1] << 8)) & 0x7FFF;
          }
          else
          {
               line_buf[x] = 0;
          }
     }
}

static int32_t sign_extend_bg_ref(uint32_t value)
{
     value &= 0x0FFFFFFFU;
     return (int32_t)(value << 4) >> 4;
}

static void write_bg_ref16(struct gba_bg_layer *bg, bool y_ref,
                           bool high, uint16_t val)
{
     uint32_t cur = (uint32_t)(y_ref ? bg->ref_y : bg->ref_x) & 0x0FFFFFFFU;
     uint32_t next = high ? ((cur & 0x0000FFFFU) | ((uint32_t)val << 16))
                          : ((cur & 0x0FFF0000U) | val);
     int32_t ref = sign_extend_bg_ref(next);

     if (y_ref)
     {
          bg->ref_y = ref;
          bg->ref_y_latch = ref;
     }
     else
     {
          bg->ref_x = ref;
          bg->ref_x_latch = ref;
     }
}

/* -------------------------------------------------------------------------
 * Sprite rendering — fills obj_buf / obj_prio_buf / obj_mode_buf
 * Sprites rendered lowest-priority (127) first so highest (0) wins.
 * ---------------------------------------------------------------------- */

static void render_sprites(struct gba *gba, uint8_t line)
{
     struct gba_gpu *gpu = &gba->gpu;

     /* Sentinel: 0x8000 means no sprite at this pixel */
     for (int i = 0; i < GBA_LCD_W; i++)
     {
          gpu->obj_buf[i] = 0x8000;
          gpu->obj_prio_buf[i] = 4;
          gpu->obj_mode_buf[i] = 0;
     }

     for (int s = 127; s >= 0; s--)
     {
          uint32_t base = (uint32_t)(s * 8);
          uint16_t attr0 = (uint16_t)(gba->oam[base] | (gba->oam[base + 1] << 8));
          uint16_t attr1 = (uint16_t)(gba->oam[base + 2] | (gba->oam[base + 3] << 8));
          uint16_t attr2 = (uint16_t)(gba->oam[base + 4] | (gba->oam[base + 5] << 8));

          uint8_t obj_mode = (attr0 >> 8) & 0x3;
          if (obj_mode == 2)
               continue; /* disabled */

          uint8_t gfx_mode = (attr0 >> 10) & 0x3; /* 0=normal,1=semi-transp,2=obj-win */
          bool affine = (obj_mode == 1 || obj_mode == 3);
          bool dbl_size = (obj_mode == 3);

          int8_t oy = (int8_t)(attr0 & 0xFF);
          int16_t ox = (int16_t)(attr1 & 0x1FF);
          if (ox >= 240)
               ox -= 512;

          uint8_t shape = (attr0 >> 14) & 0x3;
          uint8_t size = (attr1 >> 14) & 0x3;

          static const int8_t sprite_w[3][4] = {
              {8, 16, 32, 64},
              {16, 32, 32, 64},
              {8, 8, 16, 32},
          };
          static const int8_t sprite_h[3][4] = {
              {8, 16, 32, 64},
              {8, 8, 16, 32},
              {16, 32, 32, 64},
          };
          if (shape > 2)
               continue;

          int sw = sprite_w[shape][size];
          int sh = sprite_h[shape][size];
          int render_h = dbl_size ? sh * 2 : sh;
          int render_w = dbl_size ? sw * 2 : sw;

          int sy = (line - (int)oy) & 0xFF;
          if (sy >= render_h)
               continue;

          bool c256 = (attr0 >> 13) & 1;
          uint8_t prio = (attr2 >> 10) & 0x3;
          uint8_t pal = (attr2 >> 12) & 0xF;
          uint16_t tile_num = attr2 & 0x3FF;

          if (affine)
          {
               /* Read 2×2 affine matrix from OAM (4 params, 2 bytes each, stride 8) */
               uint8_t aff_idx = (attr1 >> 9) & 0x1F;
               uint32_t aff_base = (uint32_t)(aff_idx * 32); /* each group = 4 sprites×8B */
               int16_t pa = (int16_t)(gba->oam[aff_base + 6] | (gba->oam[aff_base + 7] << 8));
               int16_t pb = (int16_t)(gba->oam[aff_base + 14] | (gba->oam[aff_base + 15] << 8));
               int16_t pc = (int16_t)(gba->oam[aff_base + 22] | (gba->oam[aff_base + 23] << 8));
               int16_t pd = (int16_t)(gba->oam[aff_base + 30] | (gba->oam[aff_base + 31] << 8));

               /* Centre of rendered bounding box */
               int cx = render_w / 2;
               int cy = render_h / 2;

               /* Texture-space row start for this scanline */
               int32_t tex_x0 = ((sy - cy) * (int32_t)pb + cx * 0x100) + (sw << 7);
               int32_t tex_y0 = ((sy - cy) * (int32_t)pd + cy * 0x100) + (sh << 7);

               for (int x = 0; x < render_w; x++)
               {
                    int dx = ox + x;
                    if (dx < 0 || dx >= GBA_LCD_W)
                    {
                         tex_x0 += pa;
                         tex_y0 += pc;
                         continue;
                    }

                    int fpx = (tex_x0 >> 8);
                    int fpy = (tex_y0 >> 8);
                    tex_x0 += pa;
                    tex_y0 += pc;

                    if (fpx < 0 || fpx >= sw || fpy < 0 || fpy >= sh)
                         continue;

                    uint16_t tn;
                    int tpx = fpx % 8, tpy = fpy % 8;
                    if (gpu->obj_1d)
                    {
                         if (c256)
                              tn = (uint16_t)(tile_num + (fpy / 8) * (sw / 8) * 2 + (fpx / 8) * 2);
                         else
                              tn = (uint16_t)(tile_num + (fpy / 8) * (sw / 8) + (fpx / 8));
                    }
                    else
                    {
                         int cell_x = fpx / 8, cell_y = fpy / 8;
                         tn = (uint16_t)((tile_num & ~0x1F) + cell_y * 32 + (tile_num & 0x1F) + cell_x);
                    }

                    uint32_t tile_vram_base = 0x10000;
                    uint8_t color_idx;
                    uint16_t color;
                    if (c256)
                    {
                         uint32_t taddr = tile_vram_base + tn * 64;
                         color_idx = vram_tile_pixel_256(gba, taddr, tpx, tpy);
                         if (color_idx == 0)
                              continue;
                         color = read_obj_pal16(gba, color_idx) & 0x7FFF;
                    }
                    else
                    {
                         uint32_t taddr = tile_vram_base + tn * 32;
                         color_idx = vram_tile_pixel_16(gba, taddr, tpx, tpy);
                         if (color_idx == 0)
                              continue;
                         color = read_obj_pal16(gba, pal * 16 + color_idx) & 0x7FFF;
                    }

                    if (prio <= gpu->obj_prio_buf[dx])
                    {
                         gpu->obj_buf[dx] = color;
                         gpu->obj_prio_buf[dx] = prio;
                         gpu->obj_mode_buf[dx] = gfx_mode;
                    }
               }
          }
          else
          {
               /* Normal (non-affine) sprite */
               bool mosaic = (attr0 >> 12) & 1;
               bool hflip = (attr1 >> 12) & 1;
               bool vflip = (attr1 >> 13) & 1;
               int mos_h = mosaic ? (int)(gpu->obj_mosaic_h + 1) : 1;
               int mos_v = mosaic ? (int)(gpu->obj_mosaic_v + 1) : 1;
               int snapped_sy = (sy / mos_v) * mos_v;
               int py = vflip ? (sh - 1 - snapped_sy) : snapped_sy;

               for (int x = 0; x < sw; x++)
               {
                    int dx = ox + x;
                    if (dx < 0 || dx >= GBA_LCD_W)
                         continue;

                    int snapped_x = (x / mos_h) * mos_h;
                    int px = hflip ? (sw - 1 - snapped_x) : snapped_x;
                    int tpx = px % 8, tpy = py % 8;

                    uint16_t tn;
                    if (gpu->obj_1d)
                    {
                         if (c256)
                              tn = (uint16_t)(tile_num + (py / 8) * (sw / 8) * 2 + (px / 8) * 2);
                         else
                              tn = (uint16_t)(tile_num + (py / 8) * (sw / 8) + (px / 8));
                    }
                    else
                    {
                         int cell_x = px / 8, cell_y = py / 8;
                         tn = (uint16_t)((tile_num & ~0x1F) + cell_y * 32 + (tile_num & 0x1F) + cell_x);
                    }

                    uint32_t tile_vram_base = 0x10000;
                    uint8_t color_idx;
                    uint16_t color;
                    if (c256)
                    {
                         uint32_t taddr = tile_vram_base + tn * 64;
                         color_idx = vram_tile_pixel_256(gba, taddr, tpx, tpy);
                         if (color_idx == 0)
                              continue;
                         color = read_obj_pal16(gba, color_idx) & 0x7FFF;
                    }
                    else
                    {
                         uint32_t taddr = tile_vram_base + tn * 32;
                         color_idx = vram_tile_pixel_16(gba, taddr, tpx, tpy);
                         if (color_idx == 0)
                              continue;
                         color = read_obj_pal16(gba, pal * 16 + color_idx) & 0x7FFF;
                    }

                    if (prio <= gpu->obj_prio_buf[dx])
                    {
                         gpu->obj_buf[dx] = color;
                         gpu->obj_prio_buf[dx] = prio;
                         gpu->obj_mode_buf[dx] = gfx_mode;
                    }
               }
          }
     }
}

/* -------------------------------------------------------------------------
 * Alpha blend / brightness
 * ---------------------------------------------------------------------- */

static uint16_t blend_alpha(uint16_t a, uint16_t b, int eva, int evb)
{
     if (eva > 16)
          eva = 16;
     if (evb > 16)
          evb = 16;
     int r = ((a & 0x1F) * eva + (b & 0x1F) * evb) >> 4;
     int g = (((a >> 5) & 0x1F) * eva + ((b >> 5) & 0x1F) * evb) >> 4;
     int bl = (((a >> 10) & 0x1F) * eva + ((b >> 10) & 0x1F) * evb) >> 4;
     if (r > 31)
          r = 31;
     if (g > 31)
          g = 31;
     if (bl > 31)
          bl = 31;
     return (uint16_t)(r | (g << 5) | (bl << 10));
}

static uint16_t blend_bright(uint16_t c, int evy, bool increase)
{
     if (evy > 16)
          evy = 16;
     int r = (c & 0x1F);
     int g = (c >> 5) & 0x1F;
     int bl = (c >> 10) & 0x1F;
     if (increase)
     {
          r += ((31 - r) * evy) >> 4;
          g += ((31 - g) * evy) >> 4;
          bl += ((31 - bl) * evy) >> 4;
     }
     else
     {
          r -= (r * evy) >> 4;
          g -= (g * evy) >> 4;
          bl -= (bl * evy) >> 4;
     }
     return (uint16_t)(r | (g << 5) | (bl << 10));
}

/* -------------------------------------------------------------------------
 * Compositor: merge BG layer + OBJ layer with blending and windows
 * ---------------------------------------------------------------------- */

static void composite_line(struct gba *gba, uint8_t line)
{
     struct gba_gpu *gpu = &gba->gpu;

     uint8_t bld_mode = (gpu->bldcnt >> 6) & 0x3;  /* 0=none,1=alpha,2=bright+,3=bright- */
     uint16_t bld_1st = gpu->bldcnt & 0x3F;        /* which layers are 1st target */
     uint16_t bld_2nd = (gpu->bldcnt >> 8) & 0x3F; /* which layers are 2nd target */
     int eva = gpu->bldalpha_eva;
     int evb = gpu->bldalpha_evb;
     int evy = gpu->bldy;

     bool any_win = gpu->win0_en || gpu->win1_en || gpu->winobj_en;

     for (int x = 0; x < GBA_LCD_W; x++)
     {
          uint8_t wf = any_win ? window_flags(gpu, x, line) : 0x3F;

          /* Obj-window: if any_win and winobj_en, obj pixels with gfx_mode==2
             define a window region — use WINOUT bits 8-13 for those pixels */
          if (gpu->winobj_en && gpu->obj_buf[x] != 0x8000 && gpu->obj_mode_buf[x] == 2)
               wf = (gpu->winout >> 8) & 0x3F;

          bool color_special = (wf >> 5) & 1;

          uint16_t top_color = gpu->line_buf[x]; /* BG result */
          uint8_t top_prio = gpu->prio_buf[x];   /* 0-3 or 4 for backdrop */
          uint8_t top_layer = 5;                 /* 0-3=BG, 4=OBJ, 5=backdrop */

          /* Determine which BG layer produced top_color */
          /* We re-derive: the layer with priority == top_prio and enabled */
          for (int b = 0; b < 4; b++)
          {
               if (gpu->bg_en[b] && gpu->bg[b].priority == top_prio && top_prio < 4)
               {
                    top_layer = (uint8_t)b;
                    break;
               }
          }

          /* Insert OBJ if it has higher or equal priority than BG top */
          bool obj_visible = (gpu->obj_en && (wf & 0x10) &&
                              gpu->obj_buf[x] != 0x8000 &&
                              gpu->obj_mode_buf[x] != 2); /* obj-win pixels invisible */
          uint16_t second_color = 0x8000;                 /* sentinel */
          uint8_t second_layer = 6;

          if (obj_visible && gpu->obj_prio_buf[x] <= top_prio)
          {
               /* OBJ in front of BG: BG becomes second target */
               second_color = top_color;
               second_layer = top_layer;
               top_color = gpu->obj_buf[x];
               top_layer = 4; /* OBJ */
                              /* top_prio not needed further */
          }
          else if (obj_visible)
          {
               /* OBJ behind BG */
               second_color = gpu->obj_buf[x];
               second_layer = 4;
          }

          /* Blend */
          if (color_special && bld_mode != 0)
          {
               bool top_is_1st = (bld_1st >> top_layer) & 1;

               /* Semi-transparent OBJ always alpha-blends with whatever is behind */
               bool semi_transp = (top_layer == 4 && gpu->obj_mode_buf[x] == 1);

               if (semi_transp && second_color != 0x8000)
               {
                    top_color = blend_alpha(top_color, second_color, eva, evb);
               }
               else if (top_is_1st)
               {
                    if (bld_mode == 1)
                    {
                         /* Alpha: need a valid second target */
                         if (second_color != 0x8000 && ((bld_2nd >> second_layer) & 1))
                              top_color = blend_alpha(top_color, second_color, eva, evb);
                    }
                    else if (bld_mode == 2)
                    {
                         top_color = blend_bright(top_color, evy, true);
                    }
                    else if (bld_mode == 3)
                    {
                         top_color = blend_bright(top_color, evy, false);
                    }
               }
          }

          gpu->line_buf[x] = top_color;
     }
}

/* -------------------------------------------------------------------------
 * Scanline rendering
 * ---------------------------------------------------------------------- */

static void render_scanline(struct gba *gba, uint8_t line)
{
     struct gba_gpu *gpu = &gba->gpu;
     uint16_t *buf = gpu->line_buf;

     /* Fill with backdrop (palette index 0); priority 4 = behind everything */
     uint16_t backdrop = read_pal16(gba, 0) & 0x7FFF;
     for (int x = 0; x < GBA_LCD_W; x++)
     {
          buf[x] = backdrop;
          gpu->prio_buf[x] = 4;
     }

     if (gpu->forced_blank)
     {
          for (int x = 0; x < GBA_LCD_W; x++)
               buf[x] = 0x7FFF;
          return;
     }

     /* Render BG layers (back-to-front by priority; lower index = drawn last = wins) */
     switch (gpu->bg_mode)
     {
     case 0:
          if (gpu->bg_en[3])
               render_bg_tile(gba, 3, buf, gpu->prio_buf, line);
          if (gpu->bg_en[2])
               render_bg_tile(gba, 2, buf, gpu->prio_buf, line);
          if (gpu->bg_en[1])
               render_bg_tile(gba, 1, buf, gpu->prio_buf, line);
          if (gpu->bg_en[0])
               render_bg_tile(gba, 0, buf, gpu->prio_buf, line);
          break;
     case 1:
          if (gpu->bg_en[2])
               render_bg_affine(gba, 2, buf, gpu->prio_buf, line);
          if (gpu->bg_en[1])
               render_bg_tile(gba, 1, buf, gpu->prio_buf, line);
          if (gpu->bg_en[0])
               render_bg_tile(gba, 0, buf, gpu->prio_buf, line);
          break;
     case 2:
          if (gpu->bg_en[3])
               render_bg_affine(gba, 3, buf, gpu->prio_buf, line);
          if (gpu->bg_en[2])
               render_bg_affine(gba, 2, buf, gpu->prio_buf, line);
          break;
     case 3:
          render_mode3(gba, buf, line);
          break;
     case 4:
          render_mode4(gba, buf, line);
          break;
     case 5:
          render_mode5(gba, buf, line);
          break;
     default:
          break;
     }

     /* Render sprites into separate OBJ buffers */
     if (gpu->obj_en)
          render_sprites(gba, line);

     /* Composite BG + OBJ with blending and window masking */
     composite_line(gba, line);
}

/* -------------------------------------------------------------------------
 * PPU sync — two events per line:
 *   Event A (hblank_flag=false): dot 0 — start of active display, 960 cycles later → B
 *   Event B (hblank_flag=true):  dot 240 — HBlank start; render line, fire IRQ/DMA;
 *                                  272 cycles later advance vcount → back to A
 * ---------------------------------------------------------------------- */

void gba_gpu_sync(struct gba *gba)
{
     struct gba_gpu *gpu = &gba->gpu;
     gba_sync_resync(gba, GBA_SYNC_GPU);

     if (!gpu->hblank_flag)
     {
          /* ---- Event B: HBlank start (dot 240) ---- */
          gpu->hblank_flag = true;
          gpu->hblank = true;

          if (gpu->vcount < GBA_LCD_H)
          {
               /* Render this line */
               render_scanline(gba, gpu->vcount);

               if (gba->frontend.draw_line)
                    gba->frontend.draw_line(gba->frontend.data, gpu->vcount, gpu->line_buf);

               if (gpu->hblank_irq_en)
                    gba_irq_trigger(gba, GBA_IRQ_HBLANK);

               gba_dma_notify_hblank(gba);
          }

          /* 68 dots until end-of-line */
          gba_sync_next(gba, GBA_SYNC_GPU, 68 * GBA_CYCLES_PER_DOT);
     }
     else
     {
          /* ---- End of line: advance vcount ---- */
          gpu->hblank_flag = false;
          gpu->hblank = false;

          gpu->vcount++;

          if (gpu->vcount == GBA_LCD_H)
          {
               gpu->vblank = true;
               if (gpu->vblank_irq_en)
                    gba_irq_trigger(gba, GBA_IRQ_VBLANK);
               gba_dma_notify_vblank(gba);

               if (gba->frontend.flip)
                    gba->frontend.flip(gba->frontend.data);
               if (gba->frontend.refresh_input)
                    gba->frontend.refresh_input(gba->frontend.data);

               /* Latch affine BG reference points for next frame */
               for (int b = 2; b <= 3; b++)
               {
                    gba->gpu.bg[b].ref_x_latch = gba->gpu.bg[b].ref_x;
                    gba->gpu.bg[b].ref_y_latch = gba->gpu.bg[b].ref_y;
               }
          }
          else if (gpu->vcount == GBA_LCD_TOTAL_LINES - 1)
          {
               gpu->vblank = false;
          }

          if (gpu->vcount >= GBA_LCD_TOTAL_LINES)
          {
               gpu->vcount = 0;
          }

          if (gpu->vcount < GBA_LCD_H)
               latch_affine_line(gpu);

          if (gpu->vcount_irq_en && gpu->vcount == gpu->vcount_trigger)
               gba_irq_trigger(gba, GBA_IRQ_VCOUNT);

          /* 240 dots until next HBlank start */
          gba_sync_next(gba, GBA_SYNC_GPU, GBA_CYCLES_HBLANK_START);
     }
}

/* -------------------------------------------------------------------------
 * Register I/O
 * ---------------------------------------------------------------------- */

uint16_t gba_gpu_read16(struct gba *gba, uint32_t addr)
{
     struct gba_gpu *gpu = &gba->gpu;
     switch (addr)
     {
     case REG_DISPCNT:
          return (uint16_t)(gpu->bg_mode |
                            (gpu->frame_select << 4) |
                            (gpu->hblank_oam_free << 5) |
                            (gpu->obj_1d << 6) |
                            (gpu->forced_blank << 7) |
                            (gpu->bg_en[0] << 8) |
                            (gpu->bg_en[1] << 9) |
                            (gpu->bg_en[2] << 10) |
                            (gpu->bg_en[3] << 11) |
                            (gpu->obj_en << 12) |
                            (gpu->win0_en << 13) |
                            (gpu->win1_en << 14) |
                            (gpu->winobj_en << 15));
     case REG_DISPSTAT:
          return (uint16_t)((gpu->vblank ? 1 : 0) |
                            ((gpu->hblank ? 1 : 0) << 1) |
                            ((gpu->vcount == gpu->vcount_trigger ? 1 : 0) << 2) |
                            (gpu->vblank_irq_en << 3) |
                            (gpu->hblank_irq_en << 4) |
                            (gpu->vcount_irq_en << 5) |
                            (gpu->vcount_trigger << 8));
     case REG_VCOUNT:
          return gpu->vcount;
     default:
          return 0;
     }
}

uint8_t gba_gpu_read8(struct gba *gba, uint32_t addr)
{
     uint16_t v = gba_gpu_read16(gba, addr & ~1U);
     return (addr & 1) ? (v >> 8) : (v & 0xFF);
}

void gba_gpu_write16(struct gba *gba, uint32_t addr, uint16_t val)
{
     struct gba_gpu *gpu = &gba->gpu;
     switch (addr)
     {
     case REG_DISPCNT:
          gpu->bg_mode = val & 0x7;
          gpu->frame_select = (val >> 4) & 1;
          gpu->hblank_oam_free = (val >> 5) & 1;
          gpu->obj_1d = (val >> 6) & 1;
          gpu->forced_blank = (val >> 7) & 1;
          gpu->bg_en[0] = (val >> 8) & 1;
          gpu->bg_en[1] = (val >> 9) & 1;
          gpu->bg_en[2] = (val >> 10) & 1;
          gpu->bg_en[3] = (val >> 11) & 1;
          gpu->obj_en = (val >> 12) & 1;
          gpu->win0_en = (val >> 13) & 1;
          gpu->win1_en = (val >> 14) & 1;
          gpu->winobj_en = (val >> 15) & 1;
          break;
     case REG_DISPSTAT:
          gpu->vblank_irq_en = (val >> 3) & 1;
          gpu->hblank_irq_en = (val >> 4) & 1;
          gpu->vcount_irq_en = (val >> 5) & 1;
          gpu->vcount_trigger = val >> 8;
          break;
     case REG_BG0CNT:
     case REG_BG1CNT:
     case REG_BG2CNT:
     case REG_BG3CNT:
     {
          int n = (addr - REG_BG0CNT) / 2;
          gpu->bg[n].priority = val & 0x3;
          gpu->bg[n].tile_base = (val >> 2) & 0x3;
          gpu->bg[n].mosaic = (val >> 6) & 1;
          gpu->bg[n].colors_256 = (val >> 7) & 1;
          gpu->bg[n].map_base = (val >> 8) & 0x1F;
          gpu->bg[n].wrap = (val >> 13) & 1;
          gpu->bg[n].size = (val >> 14) & 0x3;
          break;
     }
     case REG_BG0HOFS:
          gpu->bg[0].hofs = val & 0x1FF;
          break;
     case REG_BG0VOFS:
          gpu->bg[0].vofs = val & 0x1FF;
          break;
     case REG_BG1HOFS:
          gpu->bg[1].hofs = val & 0x1FF;
          break;
     case REG_BG1VOFS:
          gpu->bg[1].vofs = val & 0x1FF;
          break;
     case REG_BG2HOFS:
          gpu->bg[2].hofs = val & 0x1FF;
          break;
     case REG_BG2VOFS:
          gpu->bg[2].vofs = val & 0x1FF;
          break;
     case REG_BG3HOFS:
          gpu->bg[3].hofs = val & 0x1FF;
          break;
     case REG_BG3VOFS:
          gpu->bg[3].vofs = val & 0x1FF;
          break;
     case REG_BG2PA:
          gpu->bg[2].pa = (int16_t)val;
          break;
     case REG_BG2PB:
          gpu->bg[2].pb = (int16_t)val;
          break;
     case REG_BG2PC:
          gpu->bg[2].pc = (int16_t)val;
          break;
     case REG_BG2PD:
          gpu->bg[2].pd = (int16_t)val;
          break;
     case REG_BG3PA:
          gpu->bg[3].pa = (int16_t)val;
          break;
     case REG_BG3PB:
          gpu->bg[3].pb = (int16_t)val;
          break;
     case REG_BG3PC:
          gpu->bg[3].pc = (int16_t)val;
          break;
     case REG_BG3PD:
          gpu->bg[3].pd = (int16_t)val;
          break;
     case REG_BG2X:
          write_bg_ref16(&gpu->bg[2], false, false, val);
          break;
     case REG_BG2X + 2:
          write_bg_ref16(&gpu->bg[2], false, true, val);
          break;
     case REG_BG2Y:
          write_bg_ref16(&gpu->bg[2], true, false, val);
          break;
     case REG_BG2Y + 2:
          write_bg_ref16(&gpu->bg[2], true, true, val);
          break;
     case REG_BG3X:
          write_bg_ref16(&gpu->bg[3], false, false, val);
          break;
     case REG_BG3X + 2:
          write_bg_ref16(&gpu->bg[3], false, true, val);
          break;
     case REG_BG3Y:
          write_bg_ref16(&gpu->bg[3], true, false, val);
          break;
     case REG_BG3Y + 2:
          write_bg_ref16(&gpu->bg[3], true, true, val);
          break;
     case REG_WIN0H:
          gpu->win0_x2 = val & 0xFF;
          gpu->win0_x1 = val >> 8;
          break;
     case REG_WIN1H:
          gpu->win1_x2 = val & 0xFF;
          gpu->win1_x1 = val >> 8;
          break;
     case REG_WIN0V:
          gpu->win0_y2 = val & 0xFF;
          gpu->win0_y1 = val >> 8;
          break;
     case REG_WIN1V:
          gpu->win1_y2 = val & 0xFF;
          gpu->win1_y1 = val >> 8;
          break;
     case REG_WININ:
          gpu->winin = val;
          break;
     case REG_WINOUT:
          gpu->winout = val;
          break;
     case REG_MOSAIC:
          gpu->bg_mosaic_h = val & 0xF;
          gpu->bg_mosaic_v = (val >> 4) & 0xF;
          gpu->obj_mosaic_h = (val >> 8) & 0xF;
          gpu->obj_mosaic_v = (val >> 12) & 0xF;
          break;
     case REG_BLDCNT:
          gpu->bldcnt = val;
          break;
     case REG_BLDALPHA:
          gpu->bldalpha_eva = val & 0x1F;
          gpu->bldalpha_evb = (val >> 8) & 0x1F;
          break;
     case REG_BLDY:
          gpu->bldy = val & 0x1F;
          break;
     default:
          break;
     }
}

void gba_gpu_write32(struct gba *gba, uint32_t addr, uint32_t val)
{
     switch (addr)
     {
     case REG_BG2X:
          gba->gpu.bg[2].ref_x = sign_extend_bg_ref(val);
          gba->gpu.bg[2].ref_x_latch = gba->gpu.bg[2].ref_x;
          break;
     case REG_BG2Y:
          gba->gpu.bg[2].ref_y = sign_extend_bg_ref(val);
          gba->gpu.bg[2].ref_y_latch = gba->gpu.bg[2].ref_y;
          break;
     case REG_BG3X:
          gba->gpu.bg[3].ref_x = sign_extend_bg_ref(val);
          gba->gpu.bg[3].ref_x_latch = gba->gpu.bg[3].ref_x;
          break;
     case REG_BG3Y:
          gba->gpu.bg[3].ref_y = sign_extend_bg_ref(val);
          gba->gpu.bg[3].ref_y_latch = gba->gpu.bg[3].ref_y;
          break;
     default:
          gba_gpu_write16(gba, addr, (uint16_t)(val & 0xFFFF));
          gba_gpu_write16(gba, addr + 2, (uint16_t)(val >> 16));
          break;
     }
}

void gba_gpu_write8(struct gba *gba, uint32_t addr, uint8_t val)
{
     uint16_t cur = gba_gpu_read16(gba, addr & ~1U);
     if (addr & 1)
          gba_gpu_write16(gba, addr & ~1U, (uint16_t)((cur & 0x00FF) | (val << 8)));
     else
          gba_gpu_write16(gba, addr & ~1U, (uint16_t)((cur & 0xFF00) | val));
}
