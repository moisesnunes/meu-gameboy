/*
 * gpu.c — Emulação da GPU (PPU) do Game Boy
 *
 * O Game Boy não tem uma GPU no sentido moderno. O chip responsável pelo vídeo
 * é chamado de PPU (Picture Processing Unit) e funciona de forma completamente
 * diferente de uma GPU 3D: ele é orientado a scanlines e baseado em tiles.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * CONCEITO FUNDAMENTAL: TILES
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Toda imagem no Game Boy é formada por "tiles" (blocos) de 8×8 pixels.
 * Cada pixel usa 2 bits de cor, portanto há 4 tons possíveis:
 *   00 = Branco (transparente nos sprites)
 *   01 = Cinza claro
 *   10 = Cinza escuro
 *   11 = Preto
 *
 * Um tile ocupa 16 bytes na VRAM (8 linhas × 2 bytes por linha).
 * Os 2 bits de cada pixel estão divididos em dois bytes consecutivos:
 *   byte 0 (linha 0): bit menos significativo de cada pixel
 *   byte 1 (linha 0): bit mais significativo de cada pixel
 * O pixel mais à esquerda está no bit 7 (MSB) de cada byte.
 *
 * Exemplo — tile com borda preta e interior branco:
 *   Linha 0: LSB=0xFF, MSB=0xFF → todos os pixels são 11 (preto)
 *   Linha 1: LSB=0x81, MSB=0x81 → pixels das bordas são 11, centro é 00
 *   ...
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * TILE SETS E TILE MAPS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * A VRAM (0x8000–0x9FFF) armazena dois tipos de dados:
 *
 *   Tile Set (dados dos tiles):
 *     0x8000–0x97FF — "Tile Set de Sprite" (256 tiles, sempre unsigned)
 *     0x8800–0x97FF — Sobreposto ao anterior: 2ª metade também usável como
 *                     "Tile Set de Fundo", com índice signed (–128..127)
 *
 *   Tile Map (mapa de qual tile vai em cada posição):
 *     0x9800–0x9BFF — Tile Map "baixo" (32×32 tiles = 256×256 pixels)
 *     0x9C00–0x9FFF — Tile Map "alto"  (32×32 tiles = 256×256 pixels)
 *
 * O Tile Map contém um índice de 1 byte por posição, indicando qual tile
 * deve ser desenhado naquela célula. O jogo escolhe qual dos dois tile maps
 * usar via registrador LCDC.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * AS TRÊS CAMADAS DE VÍDEO
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * O Game Boy renderiza a tela em três camadas, compostas nesta ordem:
 *
 *   1. BACKGROUND (fundo)
 *      Mapa de 256×256 pixels formado por tiles, rolável via SCX/SCY.
 *      A tela (160×144) é uma "janela" que mostra uma parte desse mapa.
 *      A rolagem envolve (wraps around) nos bordos.
 *
 *   2. WINDOW (janela)
 *      Uma segunda camada de tiles que "flutua" sobre o background.
 *      Diferente do fundo, a janela NÃO rola — ela é posicionada em
 *      coordenadas fixas (WX, WY) na tela. É usada para HUDs, pontuação,
 *      menus, etc. Pixels da window substituem os do background.
 *
 *   3. SPRITES (objetos / OAM)
 *      Até 40 sprites simultâneos, max. 10 por scanline.
 *      Cada sprite tem posição X/Y, índice de tile e atributos (flip, paleta,
 *      prioridade). Pixels transparentes (cor 00) não são desenhados.
 *      Sprites com flag "background" ficam atrás do BG/window não-brancos.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * TEMPORIZAÇÃO: MODOS DA PPU
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * A PPU roda em sincronia com a CPU (4.194304 MHz).
 * Para cada linha (scanline) são gastos exatamente 456 ciclos:
 *
 *   | Modo 2: 80 ciclos  | Modo 3: 172+ ciclos | Modo 0: restante |
 *
 *   Modo 2 — OAM Scan: a PPU lê a OAM para decidir quais sprites aparecem
 *             nesta linha. A CPU não pode acessar a OAM neste período.
 *
 *   Modo 3 — Drawing: a PPU renderiza a linha atual. A CPU não pode acessar
 *             OAM nem VRAM. A transferência usa um fetcher e uma fila de
 *             pixels; sprites e janela podem alongar a duração do modo.
 *
 *   Modo 0 — HBlank: período de pausa entre linhas. A CPU pode acessar
 *             OAM e VRAM. Jogos usam isto para atualizar gráficos sem glitches.
 *
 * A tela tem 144 linhas ativas (0–143), seguidas de:
 *
 *   Modo 1 — VBlank: 10 linhas "virtuais" (144–153), totalizando 70224 ciclos
 *             por quadro (~59,7 fps). A CPU pode acessar tudo. É aqui que o
 *             jogo tipicamente atualiza sprites, tiles e lógica de jogo.
 *
 * A linha é produzida progressivamente durante o Modo 3 por um Pixel FIFO.
 * O frontend ainda recebe a scanline inteira no início do HBlank.
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * REGISTRADORES DE CONTROLE (LCDC — 0xFF40)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   Bit 7 — LCD/PPU enable: desliga toda a tela quando 0
 *   Bit 6 — Window tile map: 0=usa 0x9800, 1=usa 0x9C00
 *   Bit 5 — Window enable
 *   Bit 4 — BG/Window tile set: 0=usa 0x8800 (signed), 1=usa 0x8000 (unsigned)
 *   Bit 3 — BG tile map: 0=usa 0x9800, 1=usa 0x9C00
 *   Bit 2 — Sprite size: 0=8×8, 1=8×16
 *   Bit 1 — Sprite enable
 *   Bit 0 — BG/Window enable (no DMG); prioridade BG no GBC
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * INTERRUPÇÕES DA PPU (LCD STAT — 0xFF41)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 *   Bit 6 — LYC=LY interrupt: dispara quando LY == LYC (linha configurável)
 *   Bit 5 — Modo 2 interrupt: dispara no início de cada nova scanline
 *   Bit 4 — Modo 1 interrupt: dispara no início do VBlank
 *   Bit 3 — Modo 0 interrupt: dispara no início do HBlank
 *   Bits 1–0 — Modo atual (somente leitura)
 *
 * ═══════════════════════════════════════════════════════════════════════════
 * MECANISMO DE SINCRONIZAÇÃO PREGUIÇOSA (LAZY SYNC)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Esta implementação não chama a PPU em todo ciclo de CPU. Em vez disso, usa
 * sincronização preguiçosa:
 *
 *   1. A CPU avança livremente, acumulando ciclos em `gb->timestamp`.
 *   2. Quando algo precisa do estado atual da PPU (leitura de LY, escrita
 *      em LCDC, etc.), `gb_gpu_sync()` é chamada.
 *   3. `gb_gpu_sync()` calcula quantos ciclos passaram desde a última sync.
 *      Em OAM/HBlank/VBlank ele avança por blocos; no Modo 3 ele avança a
 *      transferência por dots, mantendo o fetcher/FIFO entre sincronizações.
 *   4. Após sincronizar, agenda o próximo evento via `gb_sync_next()`.
 *
 * Isso evita chamar a PPU em todo ciclo do host, mas preserva os eventos mais
 * sensíveis dentro da scanline, como bloqueio de VRAM/OAM e entrada em HBlank.
 */

#include <stdio.h>
#include "gb.h"

/*
 * Temporização da PPU por scanline (total: 456 ciclos por linha).
 *
 *   Modo 2 (OAM Scan):  80 ciclos  — leitura dos atributos de sprite
 *   Modo 3 (Drawing):  mínimo de 172 ciclos — transferência de pixels
 *   Modo 0 (HBlank):   restante da linha — CPU pode acessar VRAM/OAM
 *
 * O Modo 3 pode se alongar quando o FIFO reinicia na window ou quando sprites
 * visíveis inserem atraso de fetch. O valor abaixo é o piso usado antes de
 * liberar o HBlank.
 */
#define MODE_2_CYCLES 80U
#define MODE_3_CYCLES 172U
#define MODE_3_END (MODE_2_CYCLES + MODE_3_CYCLES)
#define MODE_0_CYCLES 204U
/* Total de ciclos por scanline */
#define HTOTAL (MODE_2_CYCLES + MODE_3_CYCLES + MODE_0_CYCLES)
/* The first enabled line completes one dot early on DMG/SGB. */
#define LCD_ENABLE_FIRST_HTOTAL (HTOTAL - 1U)
/* LY reads as 1 slightly before the first enabled scanline completes. */
#define LCD_ENABLE_LY_QUIRK_DOT 452U

/* Primeiro scanline após LCD enable (DMG): 77T em mode 0, depois mode 3.
 * SameBoy: 76 + 2 = 78 T-cycles, mas o mode 3 começa 1 cycle depois da
 * transição (SameBoy adiciona 2 mais e depois vai ao goto mode_3_start). */
#define LCD_FIRST_MODE3_START 77U
#define LCD_FIRST_MODE3_END   (LCD_FIRST_MODE3_START + MODE_3_CYCLES)

/* Linha onde começa o VBlank (após as 144 linhas ativas) */
#define VSYNC_START 144U
/* Quantidade de linhas do VBlank */
#define VSYNC_LINES 10U
/* Total de linhas incluindo VBlank (0–153) */
#define VTOTAL (VSYNC_START + VSYNC_LINES)

static void gb_gpu_reset_line_state(struct gb_gpu *gpu)
{
     unsigned i;

     gpu->line_started = false;
     gpu->line_complete = false;
     gpu->line_sent = false;
     gpu->window_active = false;
     gpu->window_rendered = false;
     gpu->stat_mode0_early_fired = false;
     gpu->screen_x = 0;
     gpu->fifo_discard = 0;
     gpu->fifo_len = 0;
     gpu->sprite_stall = 0;
     gpu->mode3_min_end = MODE_3_END;
     gpu->fetcher.window = false;
     gpu->fetcher.step = 0;
     gpu->fetcher.ticks = 0;
     gpu->fetcher.map_x = 0;
     gpu->line_sprites[0].x = GB_LCD_WIDTH * 2;

     for (i = 0; i < GB_GPU_LINE_SPRITES; i++)
     {
          gpu->line_sprite_stalled[i] = false;
     }
}

static uint16_t gb_gpu_line_total(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     if (gpu->lcd_enable_ly_quirk && gpu->ly == 0)
     {
          return LCD_ENABLE_FIRST_HTOTAL;
     }

     return HTOTAL;
}

static uint8_t gb_gpu_visible_ly(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     if (gpu->lcd_enable_ly_quirk && gpu->ly == 0 &&
         gpu->line_pos >= LCD_ENABLE_LY_QUIRK_DOT)
     {
          return 1;
     }

     if (gpu->ly > 0 && gpu->ly < VSYNC_START && gpu->line_pos >= 451)
     {
          return (uint8_t)(gpu->ly + 1);
     }

     return gpu->ly;
}

static bool gb_gpu_stat_lyc_match(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     if (gpu->lcd_enable_ly_quirk && gpu->ly == 0 &&
         gpu->line_pos >= LCD_ENABLE_LY_QUIRK_DOT)
     {
          return false;
     }

     return gb_gpu_visible_ly(gb) == gpu->lyc;
}

static bool gb_gpu_oam_read_tail_blocked(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     if (gpu->lcd_enable_ly_quirk && gpu->ly == 0 &&
         gpu->line_pos >= LCD_ENABLE_LY_QUIRK_DOT)
     {
          return true;
     }

     return gpu->ly > 0 && gpu->ly < VSYNC_START && gpu->line_pos >= 453;
}

bool gb_gpu_vram_read_blocked(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     if (!gpu->master_enable)
     {
          return false;
     }

     if (gpu->ly < VSYNC_START && gpu->line_pos >= LCD_FIRST_MODE3_START &&
         (!gpu->line_complete || gpu->line_pos < MODE_3_END))
     {
          return true;
     }

     return false;
}

/*
 * gb_gpu_reset — reinicia todos os registradores da PPU para o estado inicial.
 *
 * No hardware real, após a boot ROM o LCD já está ativo com configurações
 * padrão. Aqui iniciamos com master_enable=true mas bg_enable=false para
 * que jogos precisem configurar explicitamente o que querem exibir.
 */
void gb_gpu_reset(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     unsigned i;

     gpu->scx = 0;         /* Sem rolagem horizontal */
     gpu->scy = 0;         /* Sem rolagem vertical */
     gpu->iten_lyc = false;
     gpu->iten_mode0 = false;
     gpu->iten_mode1 = false;
     gpu->iten_mode2 = false;
     gpu->master_enable = true;
     gpu->bg_enable = true;
     gpu->window_enable = false;
     gpu->sprite_enable = false;
     gpu->tall_sprites = false;           /* Sprites 8×8 por padrão */
     gpu->bg_use_high_tm = false;
     gpu->window_use_high_tm = false;
     gpu->bg_window_use_sprite_ts = true;
     gpu->ly = 0;           /* Scanline atual */
     gpu->lyc = 0;          /* Valor de comparação com LY */
     gpu->bgp = 0xfc;       /* Paleta do fundo (DMG) */
     gpu->obp0 = 0xff;      /* Paleta de sprite 0 (DMG) */
     gpu->obp1 = 0xff;      /* Paleta de sprite 1 (DMG) */
     gpu->wx = 0;           /* Posição X da janela (+7 do valor real) */
     gpu->wy = 0;           /* Posição Y da janela */
     gpu->line_pos = 0;     /* Posição dentro da scanline atual */
     gpu->lcd_enable_ly_quirk = false;
     gpu->stat_irq_line = false;
     gpu->stat_lyc_flag = true;
     gpu->window_line = 0;  /* Contador interno de linhas da window */
     gb_gpu_reset_line_state(gpu);

     for (i = 0; i < sizeof(gpu->oam); i++)
     {
          gpu->oam[i] = 0;
     }

     /* GBC: inicializa paletas com branco (0x7FFF = xBGR branco) e opri=0.
      * O boot ROM CGB faz isso antes de entregar o controle ao jogo; sem boot
      * ROM precisamos garantir um estado determinístico para evitar lixo. */
     for (i = 0; i < 8; i++)
     {
          unsigned c;
          for (c = 0; c < 4; c++)
          {
               gpu->bg_palettes.colors[i][c]     = 0x7fff;
               gpu->sprite_palettes.colors[i][c] = 0x7fff;
          }
     }
     gpu->bg_palettes.write_index     = 0;
     gpu->bg_palettes.auto_increment  = false;
     gpu->sprite_palettes.write_index    = 0;
     gpu->sprite_palettes.auto_increment = false;
     gpu->opri = 0;
}

void gb_gpu_reset_scanline(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     gpu->ly = 0;
     gpu->line_pos = 0;
     gpu->lcd_enable_ly_quirk = false;
     gpu->stat_irq_line = false;
     gpu->stat_lyc_flag = gpu->ly == gpu->lyc;
     gpu->window_line = 0;
     gb_gpu_reset_line_state(gpu);
}

static void gb_gpu_update_lyc_flag(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     gpu->stat_lyc_flag = gpu->ly == gpu->lyc;
}

static uint8_t gb_gpu_get_stat_mode(struct gb *gb)
{
     return gb_gpu_get_mode(gb);
}

static bool gb_gpu_stat_signal(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     if (!gpu->master_enable)
     {
          return false;
     }

     if (gpu->iten_lyc && gpu->stat_lyc_flag)
     {
          return true;
     }

     if (gpu->iten_mode0 && gpu->stat_mode0_early_fired)
     {
          return true;
     }

     switch (gb_gpu_get_mode(gb))
     {
     case 0:
          return gpu->iten_mode0;
     case 1:
          return gpu->iten_mode1;
     case 2:
          return gpu->iten_mode2;
     default:
          return false;
     }
}

static void gb_gpu_update_stat_irq(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     bool signal = gb_gpu_stat_signal(gb);

     if (signal && !gpu->stat_irq_line)
     {
          gb_irq_trigger(gb, GB_IRQ_LCD_STAT);
     }

     gpu->stat_irq_line = signal;
}

static void gb_gpu_maybe_fire_mode0_stat_early(struct gb *gb,
                                               uint8_t prev_mode,
                                               int32_t step)
{
     struct gb_gpu *gpu = &gb->gpu;
     uint16_t visible_hblank = gpu->mode3_min_end;

     if (prev_mode != 3 || gpu->ly >= VSYNC_START ||
         gpu->stat_mode0_early_fired || !gpu->iten_mode0)
     {
          return;
     }

     if (gpu->lcd_enable_ly_quirk && gpu->ly == 0)
     {
          visible_hblank = LCD_FIRST_MODE3_END;
     }

     if (visible_hblank == 0)
     {
          return;
     }

     if (gpu->line_pos <= visible_hblank - 1 &&
         gpu->line_pos + step >= visible_hblank - 1)
     {
          gpu->stat_mode0_early_fired = true;
          gb_gpu_update_stat_irq(gb);
     }
}

/*
 * gb_gpu_get_mode — retorna o modo atual da PPU (0, 1, 2 ou 3).
 *
 * O modo é determinado pela scanline atual (LY) e pela posição dentro
 * da scanline (line_pos):
 *
 *   LY >= 144            → Modo 1 (VBlank)
 *   line_pos < 80        → Modo 2 (OAM Scan)
 *   line_pos < 252       → Modo 3 (Drawing)
 *   line_pos < 456       → Modo 0 (HBlank)
 */
uint8_t gb_gpu_get_mode(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     if (gpu->ly >= VSYNC_START)
     {
          /* Modo 1: VBlank — a PPU não está desenhando nada */
          return 1;
     }

     /* Primeiro scanline após LCD enable (DMG): sem mode 2; começa em mode 0
      * por 72T, depois mode 3 por 172T, depois mode 0 pelo restante. */
     if (gpu->lcd_enable_ly_quirk && gpu->ly == 0)
     {
          if (gpu->line_pos < LCD_FIRST_MODE3_START)
               return 0;
          if (!gpu->line_complete || gpu->line_pos < LCD_FIRST_MODE3_END)
               return 3;
          return 0;
     }

     if (gpu->line_pos < MODE_2_CYCLES)
     {
          /* Modo 2: a PPU varre a OAM buscando sprites para esta linha */
          return 2;
     }

     if (!gpu->line_complete || gpu->line_pos < gpu->mode3_min_end)
     {
          /* Modo 3: a PPU está renderizando pixels — VRAM e OAM bloqueadas */
          return 3;
     }

     /* Modo 0: HBlank — linha concluída, CPU pode acessar VRAM e OAM */
     return 0;
}

bool gb_gpu_oam_read_blocked(struct gb *gb)
{
     if (!gb->gpu.master_enable)
     {
          return false;
     }

     if (gb_gpu_oam_read_tail_blocked(gb))
     {
          return true;
     }

     return gb_gpu_get_mode(gb) >= 2;
}

bool gb_gpu_oam_write_blocked(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     uint8_t mode;

     if (!gpu->master_enable)
     {
          return false;
     }

     mode = gb_gpu_get_mode(gb);

     if (mode == 3)
     {
          return true;
     }

     if (mode != 2)
     {
          return false;
     }

     if (gb->gbc)
     {
          return true;
     }

     return gpu->line_pos < 76;
}

bool gb_gpu_vram_blocked(struct gb *gb)
{
     return gb->gpu.master_enable && gb_gpu_get_mode(gb) == 3;
}

/*
 * gb_gpu_get_tile_color — lê a cor de um pixel específico dentro de um tile.
 *
 * Formato de um tile na VRAM (2bpp, 8×8 pixels, 16 bytes):
 *
 *   Para cada linha Y (0–7):
 *     byte[Y*2 + 0] = bits LSB de cada pixel da linha
 *     byte[Y*2 + 1] = bits MSB de cada pixel da linha
 *
 *   Pixel X da linha Y:
 *     lsb = (byte[Y*2+0] >> (7-X)) & 1
 *     msb = (byte[Y*2+1] >> (7-X)) & 1
 *     cor = (msb << 1) | lsb   → 0=branco, 1=cinza claro, 2=cinza escuro, 3=preto
 *
 * O pixel mais à esquerda (X=0) está no bit 7 — por isso invertemos X.
 *
 * Endereçamento do tile set:
 *   use_sprite_ts=true  → tile_addr = tile_index * 16         (0x0000+)
 *   use_sprite_ts=false → tile_addr = 0x1000 + (int8_t)tile_index * 16
 *                         O índice é SIGNED: 0x80–0xFF apontam para a
 *                         segunda metade do tile set de sprite (efeito de
 *                         compartilhamento entre os dois tile sets).
 *
 * @tile_index:    índice do tile (0–255)
 * @x, y:         coordenadas do pixel dentro do tile (0–7)
 * @use_sprite_ts: true=tile set de sprite (0x8000), false=tile set de BG (0x8800)
 * @use_high_bank: GBC only — usa o 2º banco de VRAM (0x2000 de offset)
 */
static enum gb_color gb_gpu_get_tile_color(struct gb *gb,
                                           uint8_t tile_index,
                                           uint8_t x, uint8_t y,
                                           bool use_sprite_ts,
                                           bool use_high_bank)
{
     unsigned tile_addr;
     /*
      * Cada tile é 8×8 pixels com 2 bits por pixel = 16 bytes por tile.
      * Layout: para cada linha, 2 bytes consecutivos (LSB plane + MSB plane).
      */
     const unsigned tile_size = 16;
     unsigned lsb;
     unsigned msb;

     if (use_sprite_ts)
     {
          /* Tile set de sprite começa no início da VRAM (0x8000 no espaço GB) */
          tile_addr = tile_index * tile_size;
     }
     else
     {
          /*
           * Tile set de fundo/janela começa em 0x9000 (offset 0x1000 na VRAM).
           * O índice é interpretado como SIGNED (int8_t):
           *   0x00–0x7F → tiles 0–127 (após o ponto base em 0x9000)
           *   0x80–0xFF → tiles –128 a –1 → aponta para 0x8800–0x8FFF
           *               (segunda metade do tile set de sprite)
           * Este truque permite compartilhar tiles entre o tile set de sprite
           * e o de fundo sem duplicar dados.
           */
          tile_addr = 0x1000 + (int8_t)tile_index * tile_size;
     }

     /* GBC only: banco alto da VRAM (adiciona 0x2000 de offset no array vram[]) */
     if (use_high_bank)
     {
          tile_addr += 0x2000;
     }

     /*
      * O pixel mais à esquerda (X=0) está no bit 7 de cada byte.
      * Invertemos X para que o bit corresponda ao pixel correto.
      */
     x = 7 - x;

     /* Lê o bit LSB e MSB do pixel e combina em um valor de 2 bits (0–3) */
     lsb = (gb->vram[tile_addr + y * 2 + 0] >> x) & 1;
     msb = (gb->vram[tile_addr + y * 2 + 1] >> x) & 1;

     return (msb << 1) | lsb;
}

/*
 * gb_gpu_palette_transform — aplica a paleta DMG a uma cor bruta de tile.
 *
 * No DMG, cada cor bruta (0–3) é remapeada através de um registrador de paleta
 * de 8 bits que contém 4 pares de bits (um para cada cor):
 *
 *   Bits [1:0] → mapeamento da cor 0 (branco)
 *   Bits [3:2] → mapeamento da cor 1 (cinza claro)
 *   Bits [5:4] → mapeamento da cor 2 (cinza escuro)
 *   Bits [7:6] → mapeamento da cor 3 (preto)
 *
 * Isso permite que jogos invertam as cores, criem efeitos de "flash", etc.
 * O registrador BGP (0xFF47) faz isso para o fundo; OBP0/OBP1 para sprites.
 *
 * @color:   cor bruta do tile (0–3)
 * @palette: byte do registrador de paleta (BGP, OBP0 ou OBP1)
 */
static enum gb_color gb_gpu_palette_transform(enum gb_color color,
                                              uint8_t palette)
{
     unsigned off = 2 * color;  /* Cada cor ocupa 2 bits no registrador */

     return (palette >> off) & 3;
}

static bool gb_gpu_cgb_dmg_compat(const struct gb *gb)
{
     return gb->gbc && gb->cart.rom && gb->cart.rom_length > 0x143 &&
            !(gb->cart.rom[0x143] & 0x80);
}

static uint16_t gb_gpu_dmg_color_to_gbc(enum gb_color color)
{
     static const uint16_t map[4] = {
          [GB_COL_WHITE]     = 0x7fff,
          [GB_COL_LIGHTGREY] = 0x56b5,
          [GB_COL_DARKGREY]  = 0x294a,
          [GB_COL_BLACK]     = 0x0000,
     };

     return map[color & 3];
}

/*
 * gb_gpu_get_bg_win_pixel — obtém o pixel do fundo ou da janela em (x, y).
 *
 * O fundo e a janela compartilham a mesma lógica de tile map:
 *   1. Divide (x, y) em coordenada de tile (tile_map_x, tile_map_y)
 *      e coordenada dentro do tile (tile_x, tile_y).
 *   2. Lê o índice do tile no tile map (mapa 32×32 na VRAM).
 *   3. Busca os dados do tile no tile set.
 *   4. No GBC: lê atributos do 2º banco de VRAM (flip, paleta, prioridade).
 *   5. Aplica a paleta (DMG) ou seleciona a paleta GBC.
 *
 * O tile map tem 32×32 entradas de 1 byte, cobrindo 256×256 pixels.
 * No VRAM: tile map baixo em 0x1800, tile map alto em 0x1C00.
 *
 * @x, y:        coordenadas no espaço do mapa (não na tela)
 * @use_high_tm: true=usa tile map em 0x9C00, false=0x9800
 */
static struct gb_gpu_pixel gb_gpu_get_bg_win_pixel(struct gb *gb,
                                                   uint8_t x, uint8_t y,
                                                   bool use_high_tm)
{
     struct gb_gpu *gpu = &gb->gpu;

     /* Posição do tile no mapa (cada tile cobre 8×8 pixels) */
     unsigned tile_map_x = x / 8;
     unsigned tile_map_y = y / 8;
     /* Posição do pixel dentro do tile */
     unsigned tile_x = x % 8;
     unsigned tile_y = y % 8;
     /* Offset base do tile map na VRAM */
     unsigned tm_addr;
     uint8_t tile_index;
     struct gb_gpu_pixel pix;
     bool use_sprite_ts = gpu->bg_window_use_sprite_ts;

     /*
      * O Game Boy tem dois tile maps independentes, selecionados via LCDC.
      * Tile map baixo: 0x9800 (offset 0x1800 na VRAM)
      * Tile map alto:  0x9C00 (offset 0x1C00 na VRAM)
      */
     if (use_high_tm)
     {
          tm_addr = 0x1c00;
     }
     else
     {
          tm_addr = 0x1800;
     }

     /*
      * O tile map é uma grade de 32×32 bytes (32 tiles por linha).
      * Cada byte é o índice do tile a ser desenhado naquela célula.
      */
     tm_addr += tile_map_y * 32 + tile_map_x;

     tile_index = gb->vram[tm_addr];

     if (gb->gbc && !gb_gpu_cgb_dmg_compat(gb))
     {
          /*
           * No GBC, o 2º banco de VRAM (offset +0x2000 no array vram[])
           * armazena atributos adicionais para cada célula do tile map:
           *
           *   Bit 7 — prioridade: tile fica na frente dos sprites
           *   Bit 6 — flip vertical
           *   Bit 5 — flip horizontal
           *   Bit 3 — usa banco alto da VRAM para os dados do tile
           *   Bits 2–0 — paleta de cor (0–7, das 8 paletas GBC do fundo)
           */
          uint8_t attrs = gb->vram[tm_addr + 0x2000];
          bool priority = attrs & 0x80;
          bool y_flip = attrs & 0x40;
          bool x_flip = attrs & 0x20;
          bool high_bank = attrs & 0x08;
          uint8_t palette = attrs & 0x07;
          enum gb_color col;

          if (x_flip)
          {
               tile_x = 7 - tile_x;
          }

          if (y_flip)
          {
               tile_y = 7 - tile_y;
          }

          col = gb_gpu_get_tile_color(gb, tile_index,
                                      tile_x, tile_y,
                                      use_sprite_ts,
                                      high_bank);

          /* No GBC, cor 0 é transparente (para fins de prioridade com sprites) */
          pix.opaque = col != GB_COL_WHITE;
          pix.bg_priority = priority;

          /* Lookup na paleta GBC (8 paletas × 4 cores, formato xBGR 1555) */
          pix.color.gbc_color = gpu->bg_palettes.colors[palette][col];
     }
     else
     {
          /* DMG: armazena apenas o índice bruto (0-3) no FIFO; BGP é aplicado
           * na saída (pop) para que mudanças mid-scanline em BGP sejam refletidas
           * imediatamente, como no hardware real. */
          pix.raw = gb_gpu_get_tile_color(gb, tile_index,
                                          tile_x, tile_y,
                                          use_sprite_ts,
                                          false);
          pix.opaque = pix.raw != GB_COL_WHITE;
          pix.bg_priority = false;
          pix.color.dmg_color = GB_COL_WHITE; /* preenchido no pop */
     }

     return pix;
}

/*
 * gb_get_oam_sprite — decodifica a entrada da OAM para um sprite.
 *
 * A OAM é um array de 160 bytes (40 sprites × 4 bytes).
 * Os offsets Y e X são armazenados com bias para permitir clipagem parcial:
 *   Y armazenado = Y real + 16   (sprite com Y=0 ainda aparece 16px acima)
 *   X armazenado = X real + 8    (sprite com X=0 ainda aparece 8px à esquerda)
 *
 * @index: índice do sprite na OAM (0–39)
 */
static struct gb_sprite gb_get_oam_sprite(struct gb *gb, unsigned index)
{
     struct gb_gpu *gpu = &gb->gpu;
     struct gb_sprite s;
     unsigned oam_off = index * 4;  /* Cada sprite ocupa 4 bytes na OAM */
     uint8_t flags;

     s.y = (int)gpu->oam[oam_off] - 16;      /* Remove bias de Y */
     s.x = (int)gpu->oam[oam_off + 1] - 8;   /* Remove bias de X */
     s.tile_index = gpu->oam[oam_off + 2];

     flags = gpu->oam[oam_off + 3];

     s.use_obp1  = flags & 0x10;
     s.x_flip    = flags & 0x20;
     s.y_flip    = flags & 0x40;
     s.background = flags & 0x80;

     if (gb->gbc)
     {
          s.high_bank = flags & 0x08;
          s.palette   = flags & 0x07;
     }
     else
     {
          s.high_bank = false;
          s.palette   = 0;
     }

     return s;
}

/*
 * Limite de sprites por scanline.
 *
 * O hardware do Game Boy só consegue processar 10 sprites por linha.
 * Se houver mais de 10 sprites visíveis em uma linha, os com maior índice
 * na OAM (índices maiores = menor prioridade) são ignorados.
 * Jogos usam esse comportamento para fazer sprites "piscarem" alternando
 * quais estão nos primeiros índices da OAM a cada frame.
 */
/*
 * gb_gpu_get_line_sprites — coleta e ordena os sprites visíveis na scanline.
 *
 * Processo:
 *   1. Itera sobre todos os 40 sprites na OAM.
 *   2. Verifica se o sprite intercepta a scanline atual (LY).
 *   3. Adiciona à lista até o máximo de 10 sprites.
 *   4. Ordena por X usando insertion sort estável — sprites com X menor
 *      são desenhados por cima de sprites com X maior. Para sprites com o
 *      mesmo X, o de menor índice na OAM tem prioridade (sort estável
 *      mantém a ordem de inserção = ordem da OAM).
 *
 * O último elemento da lista é marcado com x=GB_LCD_WIDTH*2 como sentinela
 * para simplificar o loop de renderização sem verificar limites.
 *
 * @ly:      scanline atual
 * @sprites: array de saída (tamanho GB_GPU_LINE_SPRITES + 1 para a sentinela)
 */
static unsigned gb_gpu_get_line_sprites(
    struct gb *gb,
    unsigned ly,
    struct gb_sprite sprites[GB_GPU_LINE_SPRITES + 1])
{

     struct gb_gpu *gpu = &gb->gpu;
     int i;
     unsigned n_sprites;
     unsigned sprite_height;

     /* Sprites 8×8 ou 8×16 conforme o bit 2 do LCDC */
     if (gpu->tall_sprites)
     {
          sprite_height = 16;
     }
     else
     {
          sprite_height = 8;
     }

     n_sprites = 0;
     for (i = 0; i < GB_GPU_MAX_SPRITES; i++)
     {
          struct gb_sprite s = gb_get_oam_sprite(gb, i);

          /* O sprite está na scanline se ly ∈ [s.y, s.y + sprite_height) */
          if ((int)ly < s.y || (int)ly >= (s.y + (int)sprite_height))
          {
               /* Sprite não intercepta esta linha */
               continue;
          }

          sprites[n_sprites] = s;
          n_sprites++;
          if (n_sprites >= GB_GPU_LINE_SPRITES)
          {
               /* Limite de 10 sprites por linha atingido */
               break;
          }
     }

     /* Sentinela: sprite fora da tela para encerrar o loop de renderização */
     sprites[n_sprites].x = GB_LCD_WIDTH * 2;

     /*
      * DMG: ordena por X com insertion sort ESTÁVEL.
      * Sprites com X menor aparecem na frente; empates são resolvidos pelo
      * índice na OAM (menor índice = maior prioridade), preservados pela
      * estabilidade do sort porque iteramos a OAM em ordem crescente acima.
      *
      * GBC: prioridade é puramente pelo índice na OAM — o sprite de menor
      * índice sempre vence, independente da posição X. Como já iteramos a OAM
      * em ordem, a lista já está na ordem correta e não precisa de sort.
      *
      * OPRI (FF6C) bit 0 = 1: força prioridade por X mesmo no GBC (usado pelo
      * boot ROM CGB para jogos DMG em modo de compatibilidade).
      */
     if (!gb->gbc || (gb->gpu.opri & 0x01))
     {
          for (i = 1; i < (int)n_sprites; i++)
          {
               struct gb_sprite cur = sprites[i];
               int j;

               for (j = i - 1; j >= 0; j--)
               {
                    if (sprites[j].x <= cur.x)
                    {
                         break;
                    }

                    sprites[j + 1] = sprites[j];
               }

               sprites[j + 1] = cur;
          }
     }

     return n_sprites;
}

/*
 * gb_gpu_get_sprite_col — amostra a cor de um sprite na posição (x, y) da tela.
 *
 * Retorna true e atualiza `p` se o sprite contribui com um pixel visível.
 * Retorna false se:
 *   - O sprite está atrás do fundo E o pixel de fundo é opaco (não-branco).
 *   - O pixel do sprite nessa posição é transparente (cor bruta == 0).
 *
 * Para sprites 8×16:
 *   O tile é composto por dois tiles consecutivos na VRAM.
 *   O LSB do índice é forçado a 0 para que o tile top seja sempre par.
 *   A parte de baixo usa o tile com índice+1.
 *
 * @sprite: dados do sprite decodificados da OAM
 * @x, y:  coordenadas atuais na tela
 * @p:     pixel atual (pode ser modificado com a cor do sprite)
 */
static bool gb_gpu_get_sprite_col(struct gb *gb,
                                  const struct gb_sprite *sprite,
                                  unsigned x,
                                  unsigned y,
                                  struct gb_gpu_pixel *p)
{
     struct gb_gpu *gpu = &gb->gpu;
     unsigned sprite_x;
     unsigned sprite_y;
     unsigned sprite_flip_height;
     uint8_t tile_index;
     enum gb_color col;

     if (sprite->background && p->opaque && (!gb->gbc || gpu->bg_enable))
     {
          /*
           * Sprite tem flag "behind background": só aparece sobre pixels
           * brancos do fundo/janela. Como p->opaque é true (cor não-branca),
           * o fundo "tampa" o sprite — retorna sem modificar o pixel.
           */
          return false;
     }

     /* Converte coordenada de tela em coordenada local do sprite */
     sprite_x = (int)x - sprite->x;
     sprite_y = (int)y - sprite->y;

     if (gpu->tall_sprites)
     {
          /*
           * Sprites 8×16: dois tiles consecutivos formam um único sprite.
           * O LSB do tile_index é mascarado a 0 — o tile de cima usa sempre
           * o índice par, e o de baixo usa o ímpar seguinte automaticamente
           * pela posição Y (sprite_y 8–15 vai para o segundo tile via
           * gb_gpu_get_tile_color, que usa y*2 como offset no tile).
           */
          tile_index = sprite->tile_index & 0xfe;
          sprite_flip_height = 15;  /* Para flip vertical: 15 - sprite_y */
     }
     else
     {
          tile_index = sprite->tile_index;
          sprite_flip_height = 7;
     }

     /* Aplica flip horizontal: espelha a coordenada X dentro do tile */
     if (sprite->x_flip)
     {
          sprite_x = 7 - sprite_x;
     }

     /* Aplica flip vertical: espelha a coordenada Y dentro do sprite */
     if (sprite->y_flip)
     {
          sprite_y = sprite_flip_height - sprite_y;
     }

     /* Sprites sempre usam o tile set de sprite (0x8000), índice unsigned */
     col = gb_gpu_get_tile_color(gb, tile_index,
                                 sprite_x, sprite_y,
                                 true, sprite->high_bank);

     /* Cor 0 (branco pré-paleta) = pixel transparente no sprite — não desenhado */
     if (col == GB_COL_WHITE)
     {
          return false;
     }

     /* Seleciona a paleta correta e aplica */
     if (gb->gbc && !gb_gpu_cgb_dmg_compat(gb))
     {
          /* GBC: 8 paletas independentes de 4 cores para sprites */
          p->color.gbc_color =
              gpu->sprite_palettes.colors[sprite->palette][col];
     }
     else
     {
          /* DMG: dois registradores de paleta (OBP0 e OBP1), selecionados por flag */
          uint8_t palette;

          if (sprite->use_obp1)
          {
               palette = gpu->obp1;
          }
          else
          {
               palette = gpu->obp0;
          }

          if (gb->gbc)
          {
               p->color.gbc_color =
                    gb_gpu_dmg_color_to_gbc(gb_gpu_palette_transform(col, palette));
          }
          else
          {
               p->color.dmg_color = gb_gpu_palette_transform(col, palette);
          }
     }

     return true;
}

static void gb_gpu_fifo_clear(struct gb_gpu *gpu)
{
     gpu->fifo_len = 0;
}

static bool gb_gpu_fifo_push(struct gb_gpu *gpu, struct gb_gpu_pixel p)
{
     if (gpu->fifo_len >= GB_GPU_FIFO_CAPACITY)
     {
          return false;
     }

     gpu->fifo[gpu->fifo_len++] = p;
     return true;
}

static bool gb_gpu_fifo_pop(struct gb_gpu *gpu, struct gb_gpu_pixel *p)
{
     unsigned i;

     if (gpu->fifo_len == 0)
     {
          return false;
     }

     *p = gpu->fifo[0];

     for (i = 1; i < gpu->fifo_len; i++)
     {
          gpu->fifo[i - 1] = gpu->fifo[i];
     }

     gpu->fifo_len--;
     return true;
}

static struct gb_gpu_pixel gb_gpu_white_pixel(void)
{
     struct gb_gpu_pixel p;

     p.color.dmg_color = GB_COL_WHITE;
     p.raw = GB_COL_WHITE;
     p.opaque = false;
     p.bg_priority = false;
     return p;
}

static void gb_gpu_fetcher_restart(struct gb *gb, bool window)
{
     struct gb_gpu *gpu = &gb->gpu;

     gb_gpu_fifo_clear(gpu);
     gpu->fetcher.window = window;
     gpu->fetcher.step = 0;
     gpu->fetcher.ticks = 0;
     gpu->fetcher.tile_count = 0;
     gpu->fetcher.map_x = window ? 0 : (gpu->scx / 8);
     gpu->window_active = window;
}

static bool gb_gpu_fetcher_push_tile(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     struct gb_gpu_pixel pixels[8];
     uint8_t pixel_y;
     uint8_t pixel_x;
     bool use_high_tm;
     unsigned i;

     if (gpu->fifo_len > GB_GPU_FIFO_CAPACITY - 8)
     {
          return false;
     }

     if (!gb->gbc && !gpu->bg_enable)
     {
          for (i = 0; i < 8; i++)
          {
               pixels[i] = gb_gpu_white_pixel();
          }
     }
     else if (gpu->fetcher.window)
     {
          pixel_y = gpu->window_line;
          pixel_x = gpu->fetcher.map_x * 8;
          use_high_tm = gpu->window_use_high_tm;

          for (i = 0; i < 8; i++)
          {
               pixels[i] = gb_gpu_get_bg_win_pixel(gb, pixel_x + i,
                                                   pixel_y, use_high_tm);
          }
     }
     else
     {
          /* SCY é relido a cada tile para capturar mudanças mid-scanline.
           * SCX high bits: map_x é recalculado como (scx/8 + tile_count) & 31
           * para que mudanças em SCX[7:3] durante o Mode 3 afetem os próximos
           * tiles buscados. */
          pixel_y = (gpu->ly + gpu->scy) & 0xff;
          gpu->fetcher.map_x = ((gpu->scx / 8) + gpu->fetcher.tile_count) & 31;
          pixel_x = gpu->fetcher.map_x * 8;
          use_high_tm = gpu->bg_use_high_tm;

          for (i = 0; i < 8; i++)
          {
               pixels[i] = gb_gpu_get_bg_win_pixel(gb,
                                                   (pixel_x + i) & 0xff,
                                                   pixel_y, use_high_tm);
          }
     }

     for (i = 0; i < 8; i++)
     {
          gb_gpu_fifo_push(gpu, pixels[i]);
     }

     gpu->fetcher.tile_count++;
     gpu->fetcher.map_x = (gpu->fetcher.map_x + 1) & 31;
     gb->debug.sys_viz.fade_ppu_vram = 1.0f;
     return true;
}

static void gb_gpu_fetcher_step(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     /*
      * O fetcher real tem 3 etapas de 2 T-cycles cada:
      *   Passo 0: busca o índice do tile no tile map    (2 dots)
      *   Passo 1: lê o byte low dos dados do tile       (2 dots)
      *   Passo 2: lê o byte high e empurra ao FIFO      (2 dots)
      *
      * O push acontece ao final do passo 2, sem custo extra.
      * Se o FIFO não tiver espaço (> 8 pixels), o fetcher re-tenta a cada
      * 2 dots enquanto aguarda, mantendo o passo em 2.
      */
     gpu->fetcher.ticks++;
     if (gpu->fetcher.ticks < 2)
     {
          return;
     }

     gpu->fetcher.ticks = 0;

     if (gpu->fetcher.step < 2)
     {
          gpu->fetcher.step++;
          return;
     }

     /* Passo 2 completo: tenta empurrar 8 pixels ao FIFO */
     if (gb_gpu_fetcher_push_tile(gb))
     {
          gpu->fetcher.step = 0;
     }
     /* Se FIFO cheio (> 8 pixels), mantém step=2 e retry nos próximos 2 dots */
}

static unsigned gb_gpu_sprite_penalty(unsigned oam_x, uint8_t scx)
{
     unsigned phase = (oam_x + scx) & 7;

     if (phase < 2)
     {
          return 10;
     }

     return 6;
}

static unsigned gb_gpu_sprite_same_x_count(struct gb_gpu *gpu, int x)
{
     unsigned i;
     unsigned count = 0;

     for (i = 0; i < GB_GPU_LINE_SPRITES; i++)
     {
          if (gpu->line_sprites[i].x == GB_LCD_WIDTH * 2)
          {
               break;
          }

          if (gpu->line_sprites[i].x == x)
          {
               count++;
          }
     }

     return count;
}

static unsigned gb_gpu_line_sprite_count(struct gb_gpu *gpu)
{
     unsigned i;

     for (i = 0; i < GB_GPU_LINE_SPRITES; i++)
     {
          if (gpu->line_sprites[i].x == GB_LCD_WIDTH * 2)
          {
               break;
          }
     }

     return i;
}

static unsigned gb_gpu_left_edge_sprites_before(struct gb_gpu *gpu,
                                                unsigned sprite_index)
{
     unsigned i;
     unsigned count = 0;

     for (i = 0; i < sprite_index; i++)
     {
          if (gpu->line_sprites[i].x < 0)
          {
               count++;
          }
     }

     return count;
}

static bool gb_gpu_sprite_x_repeated_before(struct gb_gpu *gpu,
                                            unsigned sprite_index,
                                            bool runtime_stall)
{
     unsigned i;
     int x = gpu->line_sprites[sprite_index].x;

     for (i = 0; i < sprite_index; i++)
     {
          if (runtime_stall && !gpu->line_sprite_stalled[i])
          {
               continue;
          }

          if (gpu->line_sprites[i].x == x)
          {
               return true;
          }
     }

     return false;
}

static bool gb_gpu_has_visible_sprite_after(struct gb_gpu *gpu,
                                            unsigned sprite_index)
{
     unsigned i;

     for (i = sprite_index + 1; i < GB_GPU_LINE_SPRITES; i++)
     {
          if (gpu->line_sprites[i].x == GB_LCD_WIDTH * 2)
          {
               break;
          }

          if (gpu->line_sprites[i].x >= 0 &&
              gpu->line_sprites[i].x < GB_LCD_WIDTH)
          {
               return true;
          }
     }

     return false;
}

static unsigned gb_gpu_obj_fetch_penalty(struct gb_gpu *gpu,
                                         unsigned sprite_index,
                                         bool runtime_stall)
{
     struct gb_sprite *s = &gpu->line_sprites[sprite_index];
     unsigned left_edge_before = gb_gpu_left_edge_sprites_before(gpu,
                                                                  sprite_index);
     unsigned same_x_count = gb_gpu_sprite_same_x_count(gpu, s->x);
     unsigned oam_x = (unsigned)(s->x + 8);
     unsigned phase = (oam_x + gpu->scx) & 7;
     unsigned penalty = gb_gpu_sprite_penalty(oam_x, gpu->scx);

	     if (!runtime_stall && s->x < 0 &&
	         gb_gpu_line_sprite_count(gpu) > 2 &&
	         sprite_index + 1 < GB_GPU_LINE_SPRITES &&
	         gpu->line_sprites[sprite_index + 1].x == s->x + 8 &&
	         (!gpu->sprite_enable || s->x == -8))
	     {
	          return gpu->sprite_enable ? 32 : 48;
	     }

	     if (!runtime_stall && gpu->sprite_enable && s->x < 0 &&
	         gb_gpu_line_sprite_count(gpu) > 2 &&
	         sprite_index + 1 < GB_GPU_LINE_SPRITES &&
	         gpu->line_sprites[sprite_index + 1].x == s->x + 8)
	     {
	          return 24;
	     }

	     if (s->x == -8)
	     {
	          return left_edge_before == 0 ? 8 : 6;
	     }

     if (gb_gpu_sprite_x_repeated_before(gpu, sprite_index, runtime_stall))
     {
          return 6;
     }

     if (!runtime_stall && gb_gpu_line_sprite_count(gpu) <= 2 &&
         gb_gpu_has_visible_sprite_after(gpu, sprite_index))
     {
          if (s->x >= 0 && phase < 2)
          {
               return 12;
          }

          return 8;
     }

     if (!runtime_stall && left_edge_before > 0 && s->x <= 1)
     {
          return 12;
     }

     if (phase < 4 && same_x_count == 1)
     {
          return 8;
     }

     if (same_x_count == 1)
     {
          return runtime_stall ? 0 : 3;
     }

     if (phase >= 2 && phase <= 3 && same_x_count < GB_GPU_LINE_SPRITES)
     {
          return 8;
     }

     return penalty;
}

static void gb_gpu_begin_pixel_transfer(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     unsigned i;

     if (gpu->line_started)
     {
          return;
     }

     gpu->line_started = true;
     gpu->line_complete = false;
     gpu->line_sent = false;
     gpu->screen_x = 0;
     gpu->fifo_discard = gpu->scx & 7;
     gpu->sprite_stall = 0;
     gpu->window_rendered = false;

     for (i = 0; i < GB_LCD_WIDTH; i++)
     {
          gpu->line[i].dmg_color = GB_COL_WHITE;
     }

     for (i = 0; i < GB_GPU_LINE_SPRITES; i++)
     {
          gpu->line_sprite_stalled[i] = false;
     }

     {
          unsigned n = gb_gpu_get_line_sprites(gb, gpu->ly, gpu->line_sprites);
          unsigned i;
          uint16_t sprite_penalty = 0;

          for (i = 0; i < n; i++)
          {
               if (gpu->line_sprites[i].x == -8 ||
                   (gpu->line_sprites[i].x < GB_LCD_WIDTH &&
                    gpu->line_sprites[i].x + 8 > 0))
               {
                    sprite_penalty += gb_gpu_obj_fetch_penalty(gpu, i, false);
               }
          }

          uint8_t scx_penalty = gpu->scx & 7;

          if (scx_penalty)
          {
               scx_penalty += 3;
          }

          gpu->mode3_min_end = MODE_3_END + scx_penalty + sprite_penalty;
     }
	     gb_gpu_fetcher_restart(gb, false);
}

static bool gb_gpu_maybe_start_window(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     int wx;

     if (gpu->window_active || !gpu->window_enable || gpu->ly < gpu->wy)
     {
          return false;
     }

     wx = (int)gpu->wx - 7;

     /* No GBC, WX=0 (wx_real=-7) é um caso especial: a window não é
      * renderizada. No DMG o comportamento difere; clipamos para 0. */
     if (wx < 0)
     {
          if (gb->gbc)
               return false;
          wx = 0;
     }

     if (wx >= GB_LCD_WIDTH || (int)gpu->screen_x < wx)
     {
          return false;
     }

     gpu->window_rendered = true;
     gpu->fifo_discard = 0;
     gb_gpu_fetcher_restart(gb, true);
     return true;
}

static bool gb_gpu_sprite_covers_x(const struct gb_sprite *sprite, unsigned x)
{
     return (int)x >= sprite->x && (int)x < sprite->x + 8;
}

static bool gb_gpu_start_sprite_stall(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     unsigned i;

     if (!gpu->sprite_enable)
     {
          return false;
     }

     for (i = 0; i < GB_GPU_LINE_SPRITES; i++)
     {
          struct gb_sprite *s = &gpu->line_sprites[i];
          int first_x;

          if (s->x == GB_LCD_WIDTH * 2)
          {
               break;
          }

          if (gpu->line_sprite_stalled[i] ||
              s->x >= GB_LCD_WIDTH || s->x + 8 <= 0)
          {
               continue;
          }

          first_x = s->x < 0 ? 0 : s->x;
          if ((int)gpu->screen_x != first_x)
          {
               continue;
          }

          gpu->line_sprite_stalled[i] = true;
	          {
	               unsigned penalty = gb_gpu_obj_fetch_penalty(gpu, i, true);

	               if (penalty == 0)
	               {
	                    continue;
	               }

	               gpu->sprite_stall = penalty - 1;
	          }

          return true;
     }

     return false;
}

static void gb_gpu_overlay_sprites(struct gb *gb,
                                   unsigned x,
                                   struct gb_gpu_pixel *p)
{
     struct gb_gpu *gpu = &gb->gpu;
     unsigned i;

     if (!gpu->sprite_enable)
     {
          return;
     }

     for (i = 0; i < GB_GPU_LINE_SPRITES; i++)
     {
          struct gb_sprite *s = &gpu->line_sprites[i];

          if (s->x == GB_LCD_WIDTH * 2)
          {
               break;
          }

          if (!gb_gpu_sprite_covers_x(s, x))
          {
               continue;
          }

          if (gb_gpu_get_sprite_col(gb, s, x, gpu->ly, p))
          {
               break;
          }
     }
}

static void gb_gpu_step_pixel_transfer(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     struct gb_gpu_pixel p;

     gb_gpu_begin_pixel_transfer(gb);

     if (gpu->line_complete)
     {
          return;
     }

     if (gpu->sprite_stall)
     {
          /*
           * O hardware busca o sprite em paralelo com o fetcher de BG.
           * O FIFO de BG continua sendo preenchido durante o stall para
           * que não esvazie quando o stall terminar.
           */
          gpu->sprite_stall--;
          gb_gpu_fetcher_step(gb);
          return;
     }

     if (gb_gpu_maybe_start_window(gb))
     {
          return;
     }

     if (gb_gpu_start_sprite_stall(gb))
     {
          return;
     }

     gb_gpu_fetcher_step(gb);

     if (!gb_gpu_fifo_pop(gpu, &p))
     {
          return;
     }

     /* Aplicar BGP no pop (não no push) para que mudanças mid-scanline sejam
      * refletidas imediatamente em todos os pixels ainda não emitidos. */
     if (!gb->gbc || gb_gpu_cgb_dmg_compat(gb))
     {
          if (gb->gbc)
          {
               p.color.gbc_color =
                    gb_gpu_dmg_color_to_gbc(gb_gpu_palette_transform(p.raw, gpu->bgp));
          }
          else
          {
               p.color.dmg_color = gb_gpu_palette_transform(p.raw, gpu->bgp);
          }
     }

     if (gpu->fifo_discard)
     {
          gpu->fifo_discard--;
          return;
     }

     if (gpu->screen_x >= GB_LCD_WIDTH)
     {
          gpu->line_complete = true;
          if (gpu->line_pos + 1 > gpu->mode3_min_end)
          {
               gpu->mode3_min_end = gpu->line_pos + 1;
          }
          return;
     }

     gb_gpu_overlay_sprites(gb, gpu->screen_x, &p);
     gpu->line[gpu->screen_x] = p.color;
     gpu->screen_x++;

     if (gpu->screen_x >= GB_LCD_WIDTH)
     {
          gpu->line_complete = true;
          if (gpu->line_pos + 1 > gpu->mode3_min_end)
          {
               gpu->mode3_min_end = gpu->line_pos + 1;
          }
     }
}

static void gb_gpu_emit_cur_line(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     if (gpu->line_sent || gpu->ly >= GB_LCD_HEIGHT)
     {
          return;
     }

     if (gb->gbc)
     {
          gb->frontend.draw_line_gbc(gb, gpu->ly, gpu->line);
     }
     else
     {
          gb->frontend.draw_line_dmg(gb, gpu->ly, gpu->line);
     }

     gpu->line_sent = true;

     if (gpu->window_rendered)
     {
          gpu->window_line++;
     }
}

static void gb_gpu_finish_scanline(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     if (gpu->ly < VSYNC_START && !gpu->line_sent)
     {
          gb_gpu_emit_cur_line(gb);
     }

     gpu->ly++;
     gpu->line_pos = 0;
     gpu->lcd_enable_ly_quirk = false;
     gb_gpu_reset_line_state(gpu);

     if (gpu->ly == VSYNC_START)
     {
          /*
           * Linha 144: início do VBlank. Exponha LY=144 imediatamente no
           * limite da scanline para que jogos que fazem polling de LY não
           * saltem direto da última linha visível para o próximo frame.
           */
          gb->frontend.flip(gb);
          gb_irq_trigger(gb, GB_IRQ_VSYNC);

          if (!gpu->stat_irq_line && gpu->iten_mode2)
          {
               gb_irq_trigger(gb, GB_IRQ_LCD_STAT);
          }
     }

     if (gpu->ly >= VTOTAL)
     {
          gpu->ly = 0;
          gpu->window_line = 0;
          gb_gpu_reset_line_state(gpu);
     }

     gb_gpu_update_lyc_flag(gb);
     gb_gpu_update_stat_irq(gb);
}

/*
 * gb_gpu_sync — sincroniza o estado da PPU com o timestamp atual da CPU.
 *
 * Esta é a função que implementa a sincronização preguiçosa (lazy sync).
 * Ela "avança" a PPU por todos os ciclos decorridos desde a última sync.
 * O Modo 3 é especial: ele avança dot a dot para preservar o estado do
 * fetcher/FIFO entre leituras e escritas da CPU.
 *
 * Fluxo principal:
 *   1. Calcula quantos ciclos passaram (elapsed) com gb_sync_resync().
 *   2. Loop: consome ciclos em blocos para OAM/HBlank/VBlank e por dot para
 *      Pixel Transfer.
 *   3. Ao cruzar Modo 3→0, envia a scanline pronta, dispara HBlank STAT e
 *      executa HBlank DMA quando necessário.
 *   4. No fim da linha, incrementa LY, trata VBlank, LYC e Modo 2.
 *   5. Agenda o próximo evento com gb_sync_next().
 *
 * Durante o Modo 3 o próximo evento é curto para que o FIFO continue andando
 * mesmo quando a CPU não toca registradores de vídeo.
 */
void gb_gpu_sync(struct gb *gb)
{
     struct gb_gpu *gpu  = &gb->gpu;
     struct gb_hdma *hdma = &gb->hdma;
     int32_t elapsed = gb_sync_resync(gb, GB_SYNC_GPU);
     int32_t next_event;

     if (!gpu->master_enable)
     {
          /* PPU desligada (bit 7 do LCDC = 0): não processa nada */
          gb_sync_next(gb, GB_SYNC_GPU, GB_SYNC_NEVER);
          return;
     }

     if (gpu->line_pos >= gb_gpu_line_total(gb))
     {
          gb_gpu_finish_scanline(gb);
     }

     while (elapsed > 0)
     {
          uint8_t prev_mode = gb_gpu_get_mode(gb);
          int32_t step = 1;

          if (gpu->ly >= VSYNC_START)
          {
               /* VBlank has no pixel transfer, so we can jump to line end. */
               step = gb_gpu_line_total(gb) - gpu->line_pos;
          }
          else if (gpu->lcd_enable_ly_quirk && gpu->ly == 0 && prev_mode == 0
                   && gpu->line_pos < LCD_FIRST_MODE3_START)
          {
               /* First scanline after LCD enable: mode 0 for 72T before mode 3. */
               step = LCD_FIRST_MODE3_START - gpu->line_pos;
          }
          else if (prev_mode == 2)
          {
               step = MODE_2_CYCLES - gpu->line_pos;
          }
          else if (prev_mode == 0)
          {
               step = gb_gpu_line_total(gb) - gpu->line_pos;
          }
          else
          {
               gb_gpu_step_pixel_transfer(gb);
               step = 1;
          }

          if (step <= 0)
          {
               step = 1;
          }

          if (step > elapsed)
          {
               step = elapsed;
          }

          gb_gpu_maybe_fire_mode0_stat_early(gb, prev_mode, step);

          gpu->line_pos += step;
          elapsed -= step;

          if (prev_mode != gb_gpu_get_mode(gb))
          {
               if (gb_gpu_get_mode(gb) == 0)
               {
                    gb_gpu_emit_cur_line(gb);

                    if (hdma->run_on_hblank)
                    {
                         gb_hdma_hblank(gb);
                    }
               }

               gb_gpu_update_stat_irq(gb);
          }

          if (gpu->line_pos >= gb_gpu_line_total(gb))
          {
               gb_gpu_finish_scanline(gb);
          }
     }

     if (gpu->line_pos >= gb_gpu_line_total(gb))
     {
          gb_gpu_finish_scanline(gb);
     }

     /* Agenda o próximo ponto em que a PPU precisa avançar sozinha. */
     if (gpu->ly >= VSYNC_START)
     {
          next_event = gb_gpu_line_total(gb) - gpu->line_pos;
     }
     else
     {
          if (gpu->lcd_enable_ly_quirk && gpu->ly == 0
              && gb_gpu_get_mode(gb) == 0
              && gpu->line_pos < LCD_FIRST_MODE3_START)
          {
               next_event = LCD_FIRST_MODE3_START - gpu->line_pos;
          }
          else switch (gb_gpu_get_mode(gb))
          {
          case 2:
               next_event = MODE_2_CYCLES - gpu->line_pos;
               break;
          case 3:
               /*
                * Pixel transfer is now stateful. Keep scheduling short syncs
                * so register writes, VRAM/OAM access blocking and HBlank IRQs
                * see the FIFO at the right point inside the scanline.
                */
               next_event = 1;
               break;
          default:
               next_event = gb_gpu_line_total(gb) - gpu->line_pos;
               break;
          }
     }

     if (next_event <= 0)
     {
          next_event = 1;
     }

     gb_sync_next(gb, GB_SYNC_GPU, next_event);
}

/*
 * gb_gpu_set_lcd_stat — escreve no registrador LCD STAT (0xFF41).
 *
 * O LCD STAT controla quais condições disparam a IRQ LCD_STAT:
 *   Bit 6 — IRQ quando LY == LYC
 *   Bit 5 — IRQ no início do Modo 2 (OAM scan de cada linha)
 *   Bit 4 — IRQ no início do Modo 1 (VBlank)
 *   Bit 3 — IRQ no início do Modo 0 (HBlank)
 *
 * Habilitar a IRQ de Modo 0 pode mudar o momento do próximo evento da sync
 * (porque o evento passa a ocorrer no meio da linha, não no fim). Por isso
 * chamamos gb_gpu_sync() duas vezes: antes e depois de alterar iten_mode0.
 */
void gb_gpu_set_lcd_stat(struct gb *gb, uint8_t stat)
{
     struct gb_gpu *gpu = &gb->gpu;
     bool dmg_write_glitch;

     /* Sincroniza antes de mudar o estado para não perder eventos passados */
     gb_gpu_sync(gb);

     dmg_write_glitch = !gb->gbc && gpu->master_enable && !gpu->stat_irq_line &&
                        (gb_gpu_get_stat_mode(gb) < 2 || gpu->stat_lyc_flag);

     gpu->iten_mode0 = stat & 0x08;
     gpu->iten_mode1 = stat & 0x10;
     gpu->iten_mode2 = stat & 0x20;
     gpu->iten_lyc   = stat & 0x40;

     if (dmg_write_glitch)
     {
          /*
           * DMG quirk: writing STAT briefly behaves as if all STAT interrupt
           * sources were enabled. CGB does not have this write glitch.
           */
          gb_irq_trigger(gb, GB_IRQ_LCD_STAT);
     }

     gb_gpu_update_stat_irq(gb);
}

/*
 * gb_gpu_get_lcd_stat — lê o registrador LCD STAT (0xFF41).
 *
 * Retorna o estado atual da PPU incluindo:
 *   Bits 1–0 — modo atual (0, 1, 2 ou 3)
 *   Bit 2    — LY == LYC (coincidência de linha)
 *   Bits 6–3 — flags de enable das IRQs (espelho do que foi escrito)
 */
uint8_t gb_gpu_get_lcd_stat(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     uint8_t r = 0;

     if (!gpu->master_enable)
     {
          r |= 0x80;
          r |= gpu->stat_lyc_flag << 2;
          r |= gpu->iten_mode0 << 3;
          r |= gpu->iten_mode1 << 4;
          r |= gpu->iten_mode2 << 5;
          r |= gpu->iten_lyc   << 6;
          return r;
     }

     /* Sincroniza para que o modo retornado reflita o timestamp atual */
     gb_gpu_sync(gb);

     r |= 0x80;                           /* Bit 7: unused, reads as 1 */
     r |= gb_gpu_get_stat_mode(gb);       /* Bits 1–0: modo atual */
     r |= gb_gpu_stat_lyc_match(gb) << 2;
     r |= gpu->iten_mode0 << 3;
     r |= gpu->iten_mode1 << 4;
     r |= gpu->iten_mode2 << 5;
     r |= gpu->iten_lyc   << 6;

     return r;
}

/*
 * gb_gpu_set_lcdc — escreve no registrador LCDC (0xFF40).
 *
 * O LCDC é o registrador de controle principal da PPU:
 *
 *   Bit 7 — master_enable: liga/desliga toda a PPU.
 *            Desligar a PPU (bit 7 = 0) fora do VBlank pode danificar o LCD
 *            real — jogos corretos só desligam durante o VBlank.
 *            Ao desligar: a tela é limpa com branco e LY/line_pos são zerados.
 *
 *   Bit 6 — window_use_high_tm: qual tile map a janela usa (0x9800 ou 0x9C00)
 *   Bit 5 — window_enable
 *   Bit 4 — bg_window_use_sprite_ts: qual tile set BG/window usa
 *            0 = tile set de BG (0x8800, índice signed)
 *            1 = tile set de sprite (0x8000, índice unsigned)
 *   Bit 3 — bg_use_high_tm: qual tile map o fundo usa
 *   Bit 2 — tall_sprites: 0=8×8, 1=8×16
 *   Bit 1 — sprite_enable
 *   Bit 0 — bg_enable (DMG) / prioridade BG sobre sprites (GBC)
 */
void gb_gpu_set_lcdc(struct gb *gb, uint8_t lcdc)
{
     struct gb_gpu *gpu = &gb->gpu;
     bool master_enable;

     /* Sincroniza antes de alterar qualquer configuração */
     gb_gpu_sync(gb);

     gpu->bg_enable               = lcdc & 0x01;
     gpu->sprite_enable           = lcdc & 0x02;
     gpu->tall_sprites            = lcdc & 0x04;
     gpu->bg_use_high_tm          = lcdc & 0x08;
     gpu->bg_window_use_sprite_ts = lcdc & 0x10;
     gpu->window_enable           = lcdc & 0x20;
     gpu->window_use_high_tm      = lcdc & 0x40;
     master_enable                = lcdc & 0x80;

     if (master_enable != gpu->master_enable)
     {
          gpu->master_enable = master_enable;

          if (master_enable == false)
          {
               /*
                * PPU desligada: limpa a tela com branco e reinicia a posição.
                * No hardware real o LCD exibe branco quando a PPU está desligada.
                */
               union gb_gpu_color line[GB_LCD_WIDTH];
               unsigned i;

               for (i = 0; i < GB_LCD_WIDTH; i++)
               {
                    if (gb->gbc)
                         line[i].gbc_color = 0x7fff;
                    else
                         line[i].dmg_color = GB_COL_WHITE;
               }

               for (i = 0; i < GB_LCD_HEIGHT; i++)
               {
                    if (gb->gbc)
                         gb->frontend.draw_line_gbc(gb, i, line);
                    else
                         gb->frontend.draw_line_dmg(gb, i, line);
               }

               gpu->ly       = 0;
               gpu->line_pos = 0;
               gpu->lcd_enable_ly_quirk = false;
               gpu->stat_irq_line = false;
               gpu->window_line = 0;
               gb_gpu_reset_line_state(gpu);
          }
          else
          {
               bool preserve_stat_line = gpu->iten_lyc && gpu->stat_lyc_flag;

               gpu->lcd_enable_ly_quirk = !gb->gbc;
               gb_gpu_update_lyc_flag(gb);
               gpu->stat_irq_line = preserve_stat_line;
               gb_gpu_update_stat_irq(gb);
          }
          /* Reagenda a PPU com o novo estado (ativa ou inativa) */
          gb_gpu_sync(gb);
     }
}

void gb_gpu_set_lyc(struct gb *gb, uint8_t lyc)
{
     struct gb_gpu *gpu = &gb->gpu;

     gb_gpu_sync(gb);
     gpu->lyc = lyc;
     if (gpu->master_enable)
     {
          gb_gpu_update_lyc_flag(gb);
     }
     gb_gpu_update_stat_irq(gb);
}

/*
 * gb_gpu_get_lcdc — lê o registrador LCDC (0xFF40).
 *
 * Reconstrói o byte LCDC a partir dos campos individuais da estrutura gpu.
 */
uint8_t gb_gpu_get_lcdc(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     uint8_t lcdc = 0;

     gb_gpu_sync(gb);

     lcdc |= (gpu->bg_enable               << 0);
     lcdc |= (gpu->sprite_enable           << 1);
     lcdc |= (gpu->tall_sprites            << 2);
     lcdc |= (gpu->bg_use_high_tm          << 3);
     lcdc |= (gpu->bg_window_use_sprite_ts << 4);
     lcdc |= (gpu->window_enable           << 5);
     lcdc |= (gpu->window_use_high_tm      << 6);
     lcdc |= (gpu->master_enable           << 7);

     return lcdc;
}

/*
 * gb_gpu_get_ly — lê o registrador LY (0xFF44).
 *
 * LY indica a scanline que a PPU está processando atualmente (0–153).
 * Valores 144–153 indicam o VBlank. O valor muda ~59,7 vezes por segundo
 * em intervalos de 456 ciclos de CPU.
 *
 * Precisamos sincronizar antes de retornar porque LY pode ter avançado
 * desde a última sync sem que a PPU tenha sido invocada diretamente.
 */
uint8_t gb_gpu_get_ly(struct gb *gb)
{
     /* Força a PPU a avançar até o timestamp atual para ter LY correto */
     gb_gpu_sync(gb);

     return gb_gpu_visible_ly(gb);
}
