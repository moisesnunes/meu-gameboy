#ifndef _GB_GPU_H_
#define _GB_GPU_H_

/* The GPU supports up to 40 sprites concurrently */
#define GB_GPU_MAX_SPRITES 40
/* The hardware only selects up to 10 sprites per scanline */
#define GB_GPU_LINE_SPRITES 10
/* Background/window FIFO depth used by the pixel transfer model */
#define GB_GPU_FIFO_CAPACITY 16

enum gb_color
{
     GB_COL_WHITE,
     GB_COL_LIGHTGREY,
     GB_COL_DARKGREY,
     GB_COL_BLACK
};

/* Resolução nativa do LCD do Game Boy. O fator de escala para a janela
 * do desktop fica no frontend (sdl.c), não aqui. */
#define GB_LCD_WIDTH  160
#define GB_LCD_HEIGHT 144

union gb_gpu_color
{
     /* DMG color: 4 shades */
     enum gb_color dmg_color;
     /* GBC color: xRGB 1555 */
     uint16_t gbc_color;
};

struct gb_gpu_pixel
{
     union gb_gpu_color color;
     /* Cor bruta do tile (0-3) antes de aplicar BGP — válido apenas para
      * pixels de BG/window no DMG.  Guardada no FIFO para que a paleta
      * BGP seja lida no momento do pop, não do fetch. */
     enum gb_color raw;
     bool opaque;
     /* GBC tile attribute bit 7: BG/window pixel has sprite priority. */
     bool bg_priority;
};

/*
 * Decoded OAM entry selected for the current line. It lives in the GPU state
 * because pixel transfer can be suspended and resumed between CPU accesses.
 */
struct gb_sprite
{
     int x;
     int y;
     uint8_t tile_index;
     bool background;
     bool x_flip;
     bool y_flip;
     bool use_obp1;
     bool high_bank;
     uint8_t palette;
};

struct gb_gpu_fetcher
{
     bool window;
     uint8_t step;
     uint8_t ticks;
     uint8_t map_x;
     /* Contador de tiles buscados desde o restart (sem offset SCX). Usado para
      * calcular map_x dinâmico quando SCX muda mid-scanline. */
     uint8_t tile_count;
};

/* Palette used by the GBC */
struct gb_color_palette
{
     /* 8 palettes of 4 colors. Each color is stored as xBGR 1555 */
     uint16_t colors[8][4];
     /* Index of the next write in this palette */
     uint8_t write_index;
     /* If true `write_index` auto-increments after a write */
     bool auto_increment;
};

struct gb_gpu
{
     /* Background scroll X */
     uint8_t scx;
     /* Background scroll Y */
     uint8_t scy;
     /* Line counter interrupt enable */
     bool iten_lyc;
     /* Mode 0 interrupt enable */
     bool iten_mode0;
     /* Mode 1 interrupt enable */
     bool iten_mode1;
     /* Mode 2 interrupt enable */
     bool iten_mode2;
     /* True if the GPU is enabled */
     bool master_enable;
     /* True if the background is enabled */
     bool bg_enable;
     /* True if the window is enabled */
     bool window_enable;
     /* True if the sprites are enabled */
     bool sprite_enable;
     /* If true sprites are 8x16, otherwise 8x8 */
     bool tall_sprites;
     /* If true the background uses the "high" tile map */
     bool bg_use_high_tm;
     /* If true the window uses the "high" tile map */
     bool window_use_high_tm;
     /* If true the background and window use the sprite tile set */
     bool bg_window_use_sprite_ts;
     /* LY register */
     uint8_t ly;
     /* LYC register */
     uint8_t lyc;
     /* Background palette */
     uint8_t bgp;
     /* Sprite palette 0 */
     uint8_t obp0;
     /* Sprite palette 1 */
     uint8_t obp1;
     /* Window position X + 7 */
     uint8_t wx;
     /* Window position Y */
     uint8_t wy;
     /* Current position within the current line */
     uint16_t line_pos;
     /* DMG quirk: after enabling LCD, LY reads 1 just before line 0 ends. */
     bool lcd_enable_ly_quirk;
     /* Current LCD STAT interrupt signal. IF is raised only on low->high. */
     bool stat_irq_line;
     /* Latched STAT coincidence bit. LCD-off handling can preserve it briefly. */
     bool stat_lyc_flag;
     /* Internal window line counter — increments only when the window is
     * actually rendered on a scanline, independent of LY and WY. */
     uint8_t window_line;
     /* STAT Mode 0 IRQ is asserted one T-cycle before HBlank is visible. */
     bool stat_mode0_early_fired;
     /* Pixel FIFO transfer state for the current scanline. */
     bool line_started;
     bool line_complete;
     bool line_sent;
     bool window_active;
     bool window_rendered;
     uint8_t screen_x;
     uint8_t fifo_discard;
     uint8_t fifo_len;
     uint8_t sprite_stall;
     uint16_t mode3_min_end;
     union gb_gpu_color line[GB_LCD_WIDTH];
     struct gb_gpu_pixel fifo[GB_GPU_FIFO_CAPACITY];
     struct gb_gpu_fetcher fetcher;
     struct gb_sprite line_sprites[GB_GPU_LINE_SPRITES + 1];
     bool line_sprite_stalled[GB_GPU_LINE_SPRITES];
     /* Object Attribute Memory (sprite configuration). Each sprite uses 4 bytes
      * for attributes. */
     uint8_t oam[GB_GPU_MAX_SPRITES * 4];
     /* GBC-only: background color palettes */
     struct gb_color_palette bg_palettes;
     /* GBC-only: sprite color palettes */
     struct gb_color_palette sprite_palettes;
     /* GBC-only: OPRI (0xFF6C). Bit 0: 0=GBC priority (OAM index),
      * 1=DMG priority (X coordinate). Written by boot ROM; locked after boot. */
     uint8_t opri;
};

void gb_gpu_reset(struct gb *gb);
void gb_gpu_reset_scanline(struct gb *gb);
uint8_t gb_gpu_get_mode(struct gb *gb);
bool gb_gpu_oam_read_blocked(struct gb *gb);
bool gb_gpu_oam_write_blocked(struct gb *gb);
bool gb_gpu_vram_read_blocked(struct gb *gb);
bool gb_gpu_vram_blocked(struct gb *gb);
void gb_gpu_sync(struct gb *gb);
void gb_gpu_set_lcd_stat(struct gb *gb, uint8_t stat);
void gb_gpu_set_lcdc(struct gb *gb, uint8_t stat);
void gb_gpu_set_lyc(struct gb *gb, uint8_t lyc);
uint8_t gb_gpu_get_lcdc(struct gb *gb);
uint8_t gb_gpu_get_ly(struct gb *gb);
uint8_t gb_gpu_get_lcd_stat(struct gb *gb);

#endif /* _GB_GPU_H_ */
