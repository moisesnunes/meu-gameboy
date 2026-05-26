#ifndef _GBA_GPU_H_
#define _GBA_GPU_H_

#include <stdint.h>
#include <stdbool.h>

struct gba;

#define GBA_LCD_W   240
#define GBA_LCD_H   160
#define GBA_LCD_DOTS_PER_LINE 308   /* 240 active + 68 HBlank */
#define GBA_LCD_TOTAL_LINES   228   /* 160 active + 68 VBlank */

/* GBA PPU timing (cycles at 16.78MHz) */
#define GBA_CYCLES_PER_DOT    4
#define GBA_CYCLES_PER_LINE   (GBA_LCD_DOTS_PER_LINE * GBA_CYCLES_PER_DOT)  /* 1232 */
#define GBA_CYCLES_PER_FRAME  (GBA_LCD_TOTAL_LINES * GBA_CYCLES_PER_LINE)

struct gba_bg_layer {
    /* BGxCNT */
    uint8_t  priority;
    uint8_t  tile_base;    /* character base block (×16KB) */
    bool     mosaic;
    bool     colors_256;   /* false=16/16 palettes, true=256/1 palette */
    uint8_t  map_base;     /* screen base block (×2KB) */
    bool     wrap;         /* affine wrapping */
    uint8_t  size;         /* 0-3 */
    /* Scroll offsets (tile modes) */
    uint16_t hofs;
    uint16_t vofs;
    /* Affine parameters (BG2/BG3 only) — 8.8 fixed point */
    int16_t  pa, pb, pc, pd;
    int32_t  ref_x, ref_y;   /* 20.8 fixed point, updated each frame */
    int32_t  ref_x_latch, ref_y_latch;
    int16_t  pa_line, pb_line, pc_line, pd_line;
    int32_t  ref_x_line, ref_y_line;
};

struct gba_sprite {
    /* OAM attribute 0 */
    int16_t  y;
    uint8_t  obj_mode;    /* 0=normal,1=affine,2=disable,3=double affine */
    uint8_t  gfx_mode;    /* 0=normal,1=semi-transp,2=obj-window */
    bool     mosaic;
    bool     colors_256;
    uint8_t  shape;       /* 0=square,1=horiz,2=vert */
    /* OAM attribute 1 */
    int16_t  x;
    uint8_t  affine_idx;  /* used when obj_mode==1||3 */
    bool     h_flip;
    bool     v_flip;
    uint8_t  size;
    /* OAM attribute 2 */
    uint16_t tile_idx;
    uint8_t  priority;
    uint8_t  palette;    /* 16-color mode only */
};

struct gba_gpu {
    /* DISPCNT (0x04000000) */
    uint8_t  bg_mode;
    bool     frame_select;     /* mode 4/5 page flip */
    bool     hblank_oam_free;
    bool     obj_1d;           /* false=2D tile mapping, true=1D */
    bool     forced_blank;
    bool     bg_en[4];
    bool     obj_en;
    bool     win0_en, win1_en, winobj_en;

    /* DISPSTAT (0x04000004) */
    bool     vblank_irq_en;
    bool     hblank_irq_en;
    bool     vcount_irq_en;
    uint8_t  vcount_trigger;

    /* VCOUNT (0x04000006) */
    uint8_t  vcount;

    /* Background layers */
    struct gba_bg_layer bg[4];

    /* Window registers */
    uint8_t  win0_x1, win0_x2, win0_y1, win0_y2;
    uint8_t  win1_x1, win1_x2, win1_y1, win1_y2;
    uint8_t  winin;   /* win0/win1 enable bits */
    uint8_t  winout;  /* outside/obj-win enable bits */

    /* Mosaic */
    uint8_t  bg_mosaic_h, bg_mosaic_v;
    uint8_t  obj_mosaic_h, obj_mosaic_v;

    /* Blend control */
    uint16_t bldcnt;
    uint8_t  bldalpha_eva, bldalpha_evb;
    uint8_t  bldy;

    /* Internal timing */
    int32_t  cycles_line;  /* cycles into current line */
    bool     hblank;
    bool     vblank;
    bool     hblank_flag;  /* true = currently in HBlank period (dot 240-307) */

    /* Current scanline buffer (RGB555 → frontend converts) */
    uint16_t line_buf[GBA_LCD_W];
    uint8_t  prio_buf[GBA_LCD_W];  /* per-pixel BG priority (0=highest, 4=backdrop) */

    /* Sprite layer rendered separately so BG+OBJ priority can be resolved */
    uint16_t obj_buf[GBA_LCD_W];   /* RGB555 color, 0x8000 = transparent sentinel */
    uint8_t  obj_prio_buf[GBA_LCD_W]; /* sprite priority (0-3) */
    uint8_t  obj_mode_buf[GBA_LCD_W]; /* 0=normal,1=semi-transp,2=obj-win per pixel */
};

void    gba_gpu_reset(struct gba *gba);
void    gba_gpu_sync(struct gba *gba);
uint8_t gba_gpu_read8(struct gba *gba, uint32_t addr);
uint16_t gba_gpu_read16(struct gba *gba, uint32_t addr);
void    gba_gpu_write8(struct gba *gba, uint32_t addr, uint8_t val);
void    gba_gpu_write16(struct gba *gba, uint32_t addr, uint16_t val);
void    gba_gpu_write32(struct gba *gba, uint32_t addr, uint32_t val);

#endif /* _GBA_GPU_H_ */
