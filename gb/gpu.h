#ifndef _GB_GPU_H_
#define _GB_GPU_H_

/* A PPU suporta até 40 sprites simultâneos na OAM */
#define GB_GPU_MAX_SPRITES 40
/* O hardware seleciona no máximo 10 sprites por scanline */
#define GB_GPU_LINE_SPRITES 10
/* Profundidade do FIFO de BG/window usado pelo modelo de transferência de pixels */
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
#define GB_LCD_WIDTH 160
#define GB_LCD_HEIGHT 144

enum gb_gpu_fetcher_phase
{
     GB_GPU_FETCH_TILE_ID_0 = 0,
     GB_GPU_FETCH_TILE_ID_1,
     GB_GPU_FETCH_TILE_DATA_LOW_0,
     GB_GPU_FETCH_TILE_DATA_LOW_1,
     GB_GPU_FETCH_TILE_DATA_HIGH_0,
     GB_GPU_FETCH_PUSH,
};

union gb_gpu_color
{
     /* Cor DMG: 4 tons (0=branco … 3=preto) */
     enum gb_color dmg_color;
     /* Cor GBC: formato xBGR 1555 (15 bits de cor) */
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
     /* Atributo GBC bit 7: pixel de BG/window tem prioridade sobre sprites. */
     bool bg_priority;
};

/*
 * Entrada decodificada da OAM selecionada para a scanline atual.
 * Vive no estado da PPU porque a transferência de pixels pode ser suspensa
 * e retomada entre acessos da CPU.
 */
struct gb_sprite
{
     int x;              /* Posição X na tela (com bias removido: OAM_X - 8) */
     int y;              /* Posição Y na tela (com bias removido: OAM_Y - 16) */
     uint8_t tile_index; /* Índice do tile na VRAM (tile set de sprite) */
     bool background;    /* Se true, o sprite fica atrás do BG/window não-branco */
     bool x_flip;        /* Espelha o sprite horizontalmente */
     bool y_flip;        /* Espelha o sprite verticalmente */
     bool use_obp1;      /* DMG: usa paleta OBP1 em vez de OBP0 */
     bool high_bank;     /* GBC: usa o banco alto da VRAM para os dados do tile */
     uint8_t palette;    /* GBC: índice da paleta de sprite (0–7) */
};

struct gb_gpu_fetcher
{
     bool window;
     /* Fase dot-a-dot do fetcher BG/window. O push ainda materializa os 8
      * pixels de uma vez, mas o pipeline já é observável em 6 estados. */
     uint8_t step;
     uint8_t ticks; /* usado como divisor de retry quando o FIFO está cheio */
     uint8_t map_x;
     /* Contador de tiles buscados desde o restart (sem offset SCX). Usado para
      * calcular map_x dinâmico quando SCX muda mid-scanline. */
     uint8_t tile_count;
     uint8_t tile_index;
     uint8_t tile_y;
     uint8_t tile_low;
     uint8_t tile_high;
     uint8_t tile_palette;
     bool tile_use_sprite_ts;
     bool tile_use_high_bank;
     bool tile_x_flip;
     bool tile_priority;
};

/* Paleta de cores usada pelo GBC (registradores BCPS/BCPD e OCPS/OCPD) */
struct gb_color_palette
{
     /* 8 paletas de 4 cores cada. Cada cor em formato xBGR 1555 */
     uint16_t colors[8][4];
     /* Índice da próxima escrita nesta paleta */
     uint8_t write_index;
     /* Se true, write_index é incrementado automaticamente após cada escrita */
     bool auto_increment;
};

struct gb_gpu
{
     /* Rolagem horizontal do fundo (registrador SCX, 0xFF43) */
     uint8_t scx;
     /* Rolagem vertical do fundo (registrador SCY, 0xFF42) */
     uint8_t scy;
     /* Habilita IRQ de LCD STAT quando LY == LYC */
     bool iten_lyc;
     /* Habilita IRQ de LCD STAT no início do Modo 0 (HBlank) */
     bool iten_mode0;
     /* Habilita IRQ de LCD STAT no início do Modo 1 (VBlank) */
     bool iten_mode1;
     /* Habilita IRQ de LCD STAT no início do Modo 2 (OAM scan) */
     bool iten_mode2;
     /* LCDC bit 7: liga/desliga toda a PPU */
     bool master_enable;
     /* LCDC bit 0: habilita o fundo (DMG) / prioridade BG sobre sprites (GBC) */
     bool bg_enable;
     /* LCDC bit 5: habilita a camada de janela (window) */
     bool window_enable;
     /* LCDC bit 1: habilita os sprites */
     bool sprite_enable;
     /* LCDC bit 2: false=sprites 8×8, true=sprites 8×16 */
     bool tall_sprites;
     /* LCDC bit 3: false=tile map do fundo em 0x9800, true=em 0x9C00 */
     bool bg_use_high_tm;
     /* LCDC bit 6: false=tile map da window em 0x9800, true=em 0x9C00 */
     bool window_use_high_tm;
     /* LCDC bit 4: false=tile set de BG (0x8800, signed), true=tile set de sprite (0x8000) */
     bool bg_window_use_sprite_ts;
     /* Registrador LY (0xFF44): scanline atual (0–153) */
     uint8_t ly;
     /* Registrador LYC (0xFF45): valor de comparação com LY para IRQ */
     uint8_t lyc;
     /* Registrador BGP (0xFF47): paleta do fundo no DMG */
     uint8_t bgp;
     /* Registrador OBP0 (0xFF48): paleta de sprite 0 no DMG */
     uint8_t obp0;
     /* Registrador OBP1 (0xFF49): paleta de sprite 1 no DMG */
     uint8_t obp1;
     /* Registrador WX (0xFF4B): posição X da window + 7 */
     uint8_t wx;
     /* Registrador WY (0xFF4A): posição Y da window */
     uint8_t wy;
     /* Posição atual dentro da scanline em T-cycles (0–455) */
     uint16_t line_pos;
     /* DMG quirk: após habilitar o LCD, LY lê 1 um pouco antes do fim da linha 0. */
     bool lcd_enable_ly_quirk;
     /* Nível atual do sinal de IRQ do LCD STAT. A IRQ só é disparada na borda 0→1. */
     bool stat_irq_line;
     /* Bit de coincidência LY==LYC travado. Pode ser preservado brevemente ao desligar o LCD. */
     bool stat_lyc_flag;
     /* Contador interno de linhas da window — incrementa apenas quando a window
      * é efetivamente renderizada numa scanline, independente de LY e WY. */
     uint8_t window_line;
     /* A IRQ de Modo 0 do STAT é disparada um T-cycle antes do HBlank ser visível. */
     bool stat_mode0_early_fired;

     /* ── Estado da transferência de pixels (Pixel FIFO) para a scanline atual ── */
     bool line_started;      /* true após iniciar a transferência da linha atual */
     bool line_complete;     /* true após os 160 pixels serem emitidos */
     bool line_sent;         /* true após enviar a linha ao frontend via draw_line_* */
     bool window_active;     /* true enquanto o fetcher está buscando tiles da window */
     bool window_rendered;   /* true se a window foi renderizada em algum pixel desta linha */
     uint8_t screen_x;       /* Próxima coluna a ser escrita (0–159) */
     uint8_t fifo_discard;   /* Pixels a descartar no início da linha por causa do SCX & 7 */
     uint8_t fifo_len;       /* Quantidade de pixels atualmente no FIFO */
     uint8_t sprite_stall;   /* T-cycles restantes de penalidade por fetch de sprite */
     uint16_t mode3_min_end; /* Posição mínima dentro da scanline para sair do Modo 3 */

     union gb_gpu_color line[GB_LCD_WIDTH];                  /* Buffer da scanline atual */
     struct gb_gpu_pixel fifo[GB_GPU_FIFO_CAPACITY];         /* FIFO de pixels BG/window */
     struct gb_gpu_fetcher fetcher;                          /* Estado do tile fetcher */
     struct gb_sprite line_sprites[GB_GPU_LINE_SPRITES + 1]; /* Sprites da scanline (+1 sentinela) */
     bool line_sprite_stalled[GB_GPU_LINE_SPRITES];          /* Marca sprites já processados pelo stall */

     /* OAM — Object Attribute Memory: configuração dos 40 sprites (4 bytes cada) */
     uint8_t oam[GB_GPU_MAX_SPRITES * 4];
     /* GBC: paletas de cor do fundo (registradores BCPS/BCPD, 0xFF68/0xFF69) */
     struct gb_color_palette bg_palettes;
     /* GBC: paletas de cor dos sprites (registradores OCPS/OCPD, 0xFF6A/0xFF6B) */
     struct gb_color_palette sprite_palettes;
     /* GBC: OPRI (0xFF6C). Bit 0: 0=prioridade por índice OAM (GBC), 1=prioridade por X (DMG).
      * Escrito pelo boot ROM; travado após o boot. */
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
const char *gb_gpu_fetcher_phase_name(uint8_t phase);
void gb_gpu_set_lcd_stat(struct gb *gb, uint8_t stat);
void gb_gpu_set_lcdc(struct gb *gb, uint8_t stat);
void gb_gpu_set_lyc(struct gb *gb, uint8_t lyc);
uint8_t gb_gpu_get_lcdc(struct gb *gb);
uint8_t gb_gpu_get_ly(struct gb *gb);
uint8_t gb_gpu_get_lcd_stat(struct gb *gb);

#endif /* _GB_GPU_H_ */
