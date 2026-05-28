#include <stdio.h>
#include "gb.h"

/* ROM (banco 0 + 1) */
#define ROM_BASE 0x0000U
#define ROM_END (ROM_BASE + 0x8000U)
/* RAM de vídeo */
#define VRAM_BASE 0x8000U
#define VRAM_END (VRAM_BASE + 0x2000U)
/* RAM do cartucho (geralmente com bateria) */
#define CRAM_BASE 0xa000U
#define CRAM_END (CRAM_BASE + 0x2000U)
/* RAM interna */
#define IRAM_BASE 0xc000U
#define IRAM_END (IRAM_BASE + 0x2000U)
/* Espelho da RAM interna */
#define IRAM_ECHO_BASE 0xe000U
#define IRAM_ECHO_END (IRAM_ECHO_BASE + 0x1e00U)
/* Object Attribute Memory (configuração de sprites) */
#define OAM_BASE 0xfe00U
#define OAM_END (OAM_BASE + 0xa0U)
/* RAM de página zero (HRAM) */
#define ZRAM_BASE 0xff80U
#define ZRAM_END (ZRAM_BASE + 0x7fU)
/* Registrador de botões de entrada */
#define REG_INPUT 0xff00U
/* Dados seriais */
#define REG_SB 0xff01U
/* Controle serial */
#define REG_SC 0xff02U
/* Divisor do timer */
#define REG_DIV 0xff04U
/* Contador do timer */
#define REG_TIMA 0xff05U
/* Módulo do timer */
#define REG_TMA 0xff06U
/* Controlador do timer */
#define REG_TAC 0xff07U
/* Flags de interrupção */
#define REG_IF 0xff0fU
/* Registradores do canal de som 1 */
#define REG_NR10 0xff10U
#define REG_NR11 0xff11U
#define REG_NR12 0xff12U
#define REG_NR13 0xff13U
#define REG_NR14 0xff14U
/* Registradores do canal de som 2 */
#define REG_NR21 0xff16U
#define REG_NR22 0xff17U
#define REG_NR23 0xff18U
#define REG_NR24 0xff19U
/* Registradores do canal de som 3 */
#define REG_NR30 0xff1aU
#define REG_NR31 0xff1bU
#define REG_NR32 0xff1cU
#define REG_NR33 0xff1dU
#define REG_NR34 0xff1eU
/* Registradores do canal de som 4 */
#define REG_NR41 0xff20U
#define REG_NR42 0xff21U
#define REG_NR43 0xff22U
#define REG_NR44 0xff23U
/* Registradores de controle do som */
#define REG_NR50 0xff24U
#define REG_NR51 0xff25U
#define REG_NR52 0xff26U
/* RAM de forma de onda do canal 3 */
#define NR3_RAM_BASE 0xff30U
#define NR3_RAM_END 0xff40U
/* Registrador de controle do LCD */
#define REG_LCDC 0xff40U
/* Registrador de status do LCD */
#define REG_LCD_STAT 0xff41U
/* Scroll vertical do fundo */
#define REG_SCY 0xff42U
/* Scroll horizontal do fundo */
#define REG_SCX 0xff43U
/* Scanline atual */
#define REG_LY 0xff44U
/* Comparador de scanline */
#define REG_LYC 0xff45U
/* DMA */
#define REG_DMA 0xff46U
/* Paleta do fundo */
#define REG_BGP 0xff47U
/* Paleta de sprites 0 */
#define REG_OBP0 0xff48U
/* Paleta de sprites 1 */
#define REG_OBP1 0xff49U
/* Posição Y da janela */
#define REG_WY 0xff4aU
/* Posição X da janela */
#define REG_WX 0xff4bU
/* Registrador de habilitação de interrupções */
#define REG_IE 0xffffU

/*
 * Registradores exclusivos do GBC
 */
/* Controle de velocidade (dupla velocidade) */
#define REG_KEY1 0xff4dU
/* Seleção de banco da VRAM */
#define REG_VBK 0xff4fU
/* Byte alto do endereço de origem do HDMA */
#define REG_HDMA1 0xff51U
/* Byte baixo do endereço de origem do HDMA */
#define REG_HDMA2 0xff52U
/* Byte alto do endereço de destino do HDMA */
#define REG_HDMA3 0xff53U
/* Byte baixo do endereço de destino do HDMA */
#define REG_HDMA4 0xff54U
/* Comprimento, modo e disparo do HDMA */
#define REG_HDMA5 0xff55U
/* Endereço de escrita da paleta de fundo (GBC) */
#define REG_BCPS 0xff68U
/* Dados da paleta de fundo (GBC) */
#define REG_BCPD 0xff69U
/* Endereço de escrita da paleta de sprites (GBC) */
#define REG_OCPS 0xff6aU
/* Dados da paleta de sprites (GBC) */
#define REG_OCPD 0xff6bU
/* Seleção de banco da RAM interna (GBC) */
#define REG_SVBK 0xff70U
/* Modo de prioridade de objetos (GBC) */
#define REG_OPRI 0xff6cU
/* Registradores CGB não documentados */
#define REG_CGB_FF72 0xff72U
#define REG_CGB_FF73 0xff73U
#define REG_CGB_FF75 0xff75U
#define REG_PCM12 0xff76U
#define REG_PCM34 0xff77U

/* Retorna o offset real dentro da RAM interna, aplicando o banco alto (GBC). */
static uint16_t gb_memory_iram_off(struct gb *gb, uint16_t off)
{
     if (off >= 0x1000)
     {
          unsigned bank = gb->iram_high_bank;

          if (bank == 0)
          {
               bank = 1;
          }

          off += (bank - 1) * 0x1000;
     }

     return off;
}

static bool gb_memory_is_cgb_hardware(const struct gb *gb)
{
     return gb->gbc || gb->hw_model == GB_HW_CGB0 || gb->hw_model == GB_HW_CGB;
}

/* Lê um byte da paleta de cores GBC (BCPD/OCPD), bloqueando durante o Mode 3. */
static uint8_t gb_memory_cgb_palette_read(struct gb *gb,
                                          struct gb_color_palette *p)
{
     uint16_t index = p->write_index;
     unsigned palette = index >> 3;
     unsigned color_index = (index >> 1) & 3;
     uint16_t col;

     gb_gpu_sync(gb);

     if (gb->gpu.master_enable && gb_gpu_get_mode(gb) == 3)
     {
          return 0xff;
     }

     col = p->colors[palette][color_index];
     return (index & 1) ? (col >> 8) : (col & 0xff);
}

/* Escreve um byte na paleta de cores GBC (BCPD/OCPD) e avança o índice se auto-increment estiver ativo. */
static void gb_memory_cgb_palette_write(struct gb *gb,
                                        struct gb_color_palette *p,
                                        uint8_t val)
{
     uint16_t index = p->write_index;

     gb_gpu_sync(gb);

     if (!(gb->gpu.master_enable && gb_gpu_get_mode(gb) == 3))
     {
          unsigned palette = index >> 3;
          unsigned color_index = (index >> 1) & 3;
          bool high = index & 1;
          uint16_t col = p->colors[palette][color_index];

          if (high)
          {
               col &= 0x00ff;
               col |= (uint16_t)val << 8;
          }
          else
          {
               col &= 0xff00;
               col |= val;
          }

          p->colors[palette][color_index] = col;
     }

     if (p->auto_increment)
     {
          p->write_index = (p->write_index + 1) & 0x3f;
     }
}

/* Registrador de desabilitação da boot ROM */
#define REG_BOOT 0xff50U
/* Porta de comunicação infravermelha (GBC) */
#define REG_RP 0xff56U

/* Tempo de transferência serial em T-cycles na velocidade simples (8 bits × 512 cycles/bit) */
#define SERIAL_BIT_CYCLES 512
#define SERIAL_TRANSFER_BITS 8

/* Lê uma palavra de 16 bits da OAM no offset indicado (little-endian). */
static uint16_t oam_get16(const struct gb *gb, unsigned off)
{
     return gb->gpu.oam[off] | ((uint16_t)gb->gpu.oam[off + 1] << 8);
}

/* Escreve uma palavra de 16 bits na OAM no offset indicado (little-endian). */
static void oam_set16(struct gb *gb, unsigned off, uint16_t value)
{
     gb->gpu.oam[off] = value & 0xff;
     gb->gpu.oam[off + 1] = value >> 8;
}

/* Fórmula do bug de OAM para escrita: corrupção de linha durante Mode 2. */
static uint16_t oam_glitch_write(uint16_t a, uint16_t b, uint16_t c)
{
     return ((a ^ c) & (b ^ c)) ^ c;
}

/* Fórmula primária do bug de OAM para leitura. */
static uint16_t oam_glitch_read(uint16_t a, uint16_t b, uint16_t c)
{
     return b | (a & c);
}

/* Fórmula secundária do bug de OAM para leitura (linhas com (row & 0x18) == 0x10). */
static uint16_t oam_glitch_read_secondary(uint16_t a, uint16_t b,
                                          uint16_t c, uint16_t d)
{
     return (b & (a | c | d)) | (a & c & d);
}

/* Fórmulas terciárias do bug de OAM para leitura (linhas com (row & 0x18) == 0x00). */
static uint16_t oam_glitch_read_tertiary_1(uint16_t a, uint16_t b,
                                           uint16_t c, uint16_t d,
                                           uint16_t e)
{
     return c | (a & b & d & e);
}

static uint16_t oam_glitch_read_tertiary_2(uint16_t a, uint16_t b,
                                           uint16_t c, uint16_t d,
                                           uint16_t e)
{
     return (c & (a | b | d | e)) | (a & b & d & e);
}

static uint16_t oam_glitch_read_tertiary_3(uint16_t a, uint16_t b,
                                           uint16_t c, uint16_t d,
                                           uint16_t e)
{
     return (c & (a | b | d | e)) | (b & d & e);
}

/* Fórmula quaternária do bug de OAM para leitura (caso especial row == 0x40). */
static uint16_t oam_glitch_read_quaternary(uint16_t a, uint16_t b,
                                           uint16_t c, uint16_t d,
                                           uint16_t e, uint16_t f,
                                           uint16_t g, uint16_t h)
{
     (void)a;
     return (e & (h | g | (~d & f) | c | b)) | (c & g & h);
}

/* Retorna a linha da OAM afetada pelo bug durante Mode 2, ou -1 se não aplicável. */
static int gb_memory_oam_bug_row(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     uint16_t row_pos = gpu->line_pos;
     unsigned index;

     if (gb->gbc || !gpu->master_enable || gpu->ly >= 144 || gpu->line_pos >= 76)
     {
          return -1;
     }

     if (row_pos < 4)
     {
          return 8;
     }

     index = row_pos / 2;
     return (int)((index & ~1U) * 4 + 8);
}

/* Copia 8 bytes de uma linha da OAM para outra (usada na simulação do bug de OAM). */
static void gb_memory_copy_oam_row(struct gb *gb, unsigned dst, unsigned src)
{
     for (unsigned i = 0; i < 8; i++)
     {
          gb->gpu.oam[dst + i] = gb->gpu.oam[src + i];
     }
}

void gb_memory_trigger_oam_bug(struct gb *gb, uint16_t addr)
{
     int row;

     if (addr < OAM_BASE || addr >= 0xff00U)
     {
          return;
     }

     gb_gpu_sync(gb);
     row = gb_memory_oam_bug_row(gb);
     if (row < 8 || row > 0x98)
     {
          return;
     }

     oam_set16(gb, row, oam_glitch_write(oam_get16(gb, row), oam_get16(gb, row - 8), oam_get16(gb, row - 4)));
     for (unsigned i = 2; i < 8; i++)
     {
          gb->gpu.oam[row + i] = gb->gpu.oam[row - 8 + i];
     }
}

static void gb_memory_trigger_oam_bug_read(struct gb *gb, uint16_t addr)
{
     int row;

     if (addr < OAM_BASE || addr >= 0xff00U)
     {
          return;
     }

     row = gb_memory_oam_bug_row(gb);
     if (row < 8 || row >= 0x98)
     {
          return;
     }

     if ((row & 0x18) == 0x10)
     {
          oam_set16(gb, row - 8,
                    oam_glitch_read_secondary(oam_get16(gb, row - 16),
                                              oam_get16(gb, row - 8),
                                              oam_get16(gb, row),
                                              oam_get16(gb, row - 4)));
          gb_memory_copy_oam_row(gb, row - 16, row - 8);
     }
     else if ((row & 0x18) == 0x00)
     {
          uint16_t v;

          if (row == 0x40)
          {
               v = oam_glitch_read_quaternary(oam_get16(gb, 0),
                                              oam_get16(gb, row),
                                              oam_get16(gb, row - 4),
                                              oam_get16(gb, row - 6),
                                              oam_get16(gb, row - 8),
                                              oam_get16(gb, row - 14),
                                              oam_get16(gb, row - 16),
                                              oam_get16(gb, row - 32));
          }
          else if (row == 0x20)
          {
               v = oam_glitch_read_tertiary_2(oam_get16(gb, row),
                                              oam_get16(gb, row - 4),
                                              oam_get16(gb, row - 8),
                                              oam_get16(gb, row - 16),
                                              oam_get16(gb, row - 32));
          }
          else if (row == 0x60)
          {
               v = oam_glitch_read_tertiary_3(oam_get16(gb, row),
                                              oam_get16(gb, row - 4),
                                              oam_get16(gb, row - 8),
                                              oam_get16(gb, row - 16),
                                              oam_get16(gb, row - 32));
          }
          else
          {
               v = oam_glitch_read_tertiary_1(oam_get16(gb, row),
                                              oam_get16(gb, row - 4),
                                              oam_get16(gb, row - 8),
                                              oam_get16(gb, row - 16),
                                              oam_get16(gb, row - 32));
          }

          oam_set16(gb, row - 8, v);
          gb_memory_copy_oam_row(gb, row - 32, row - 8);
          gb_memory_copy_oam_row(gb, row - 16, row - 8);
     }
     else
     {
          uint16_t v = oam_glitch_read(oam_get16(gb, row),
                                       oam_get16(gb, row - 8),
                                       oam_get16(gb, row - 4));

          oam_set16(gb, row - 8, v);
          oam_set16(gb, row, v);
     }

     gb_memory_copy_oam_row(gb, row, row - 8);

     if (row == 0x80)
     {
          gb_memory_copy_oam_row(gb, 0, row);
     }
}

void gb_serial_sync(struct gb *gb)
{
     if (!(gb->serial_control & 0x80))
     {
          gb_sync_next(gb, GB_SYNC_SERIAL, GB_SYNC_NEVER);
          return;
     }

     if (gb->serial_tx)
     {
          gb->serial_tx(gb, gb->serial_data);
     }

     /* Sem cabo conectado: o byte recebido é 0xFF */
     gb->serial_data = 0xff;
     gb->serial_control &= ~0x80;
     gb_irq_trigger(gb, GB_IRQ_SERIAL);
     gb_sync_next(gb, GB_SYNC_SERIAL, GB_SYNC_NEVER);
}

/* Calcula os T-cycles até o fim da transferência serial em andamento. */
static int32_t gb_serial_transfer_cycles(struct gb *gb)
{
     unsigned speed_scale = 1U << gb->double_speed;
     uint32_t divider = gb->timer.divider_counter;
     uint32_t phase = divider & ((SERIAL_BIT_CYCLES)-1);
     uint32_t first_bit_cycles = SERIAL_BIT_CYCLES - phase;
     uint32_t cpu_cycles = first_bit_cycles +
                           (SERIAL_TRANSFER_BITS - 1) * SERIAL_BIT_CYCLES;

     return (int32_t)((cpu_cycles + speed_scale - 1) / speed_scale);
}

/* Recarrega o contador de duração do canal de som, com correção para dupla velocidade no GBC. */
static void gb_memory_reload_spu_duration(struct gb *gb,
                                          struct gb_spu_duration *d,
                                          unsigned duration_max,
                                          uint8_t t1)
{
     gb_spu_duration_reload(d, duration_max, t1);

     if (gb->gbc && gb->double_speed && d->counter)
     {
          d->counter++;
     }
}

/* Sincroniza o DMA antes de qualquer acesso da CPU à memória, se necessário. */
static void gb_memory_sync_dma_before_cpu_access(struct gb *gb)
{
     if (gb->dma.running && !gb->dma.syncing &&
         gb->timestamp >= gb->sync.next_event[GB_SYNC_DMA])
     {
          gb_dma_sync(gb);
     }
}

/* Lê o byte que o DMA está transferindo no momento (conflito de barramento). */
static uint8_t gb_memory_dma_conflict_read(struct gb *gb)
{
     uint16_t source = gb->dma.source;

     if (gb->dma.position > 0)
     {
          source += gb->dma.position - 1;
     }

     gb->dma.syncing = true;
     uint8_t value = gb_memory_readb(gb, source);
     gb->dma.syncing = false;
     return value;
}

/* Retorna true se o endereço pertence ao barramento do cartucho (ROM ou CRAM). */
static bool gb_memory_dma_uses_cart_bus(uint16_t addr)
{
     return addr < ROM_END || (addr >= CRAM_BASE && addr < CRAM_END);
}

/* Retorna true se o endereço pertence ao barramento da RAM interna (IRAM ou espelho). */
static bool gb_memory_dma_uses_wram_bus(uint16_t addr)
{
     return (addr >= IRAM_BASE && addr < IRAM_END) ||
            (addr >= IRAM_ECHO_BASE && addr < IRAM_ECHO_END);
}

/* Retorna true se o DMA impede a CPU de acessar o endereço neste momento. */
static bool gb_memory_dma_blocks_cpu_access(struct gb *gb, uint16_t addr)
{
     if (!gb->dma.running || gb->dma.syncing || addr >= ZRAM_BASE)
     {
          return false;
     }

     if (addr >= OAM_BASE && addr < 0xff00U)
     {
          return gb->dma.restarting || gb->dma.position > 0;
     }

     if (gb->dma.delay > 0 || gb->dma.position == 0)
     {
          return false;
     }

     if (!gb->gbc)
     {
          return true;
     }

     if (addr >= VRAM_BASE && addr < VRAM_END)
     {
          return true;
     }

     if (gb_memory_dma_uses_cart_bus(gb->dma.source))
     {
          return gb_memory_dma_uses_cart_bus(addr);
     }

     if (gb_memory_dma_uses_wram_bus(gb->dma.source))
     {
          return gb_memory_dma_uses_wram_bus(addr);
     }

     return true;
}

/* Leitura do debugger/UI: observa o byte mapeado sem acionar watchpoints ou sincronizar dispositivos. */
uint8_t gb_memory_peekb(struct gb *gb, uint16_t addr)
{
     if (gb->bootrom_mapped && gb->bootrom)
     {
          if (addr < 0x0100)
               return gb->bootrom[addr];
          if (gb->gbc && addr >= 0x0200 && addr < 0x0900)
               return gb->bootrom[addr];
     }

     if (addr >= ROM_BASE && addr < ROM_END)
          return gb_cart_rom_readb(gb, addr - ROM_BASE);

     if (addr >= ZRAM_BASE && addr < ZRAM_END)
          return gb->zram[addr - ZRAM_BASE];

     if (addr >= IRAM_BASE && addr < IRAM_END)
     {
          uint16_t off = gb_memory_iram_off(gb, addr - IRAM_BASE);
          return gb->iram[off];
     }

     if (addr >= IRAM_ECHO_BASE && addr < IRAM_ECHO_END)
     {
          uint16_t off = gb_memory_iram_off(gb, addr - IRAM_ECHO_BASE);
          return gb->iram[off];
     }

     if (addr >= VRAM_BASE && addr < VRAM_END)
     {
          uint16_t off = addr - VRAM_BASE;
          off += 0x2000 * gb->vram_high_bank;
          return gb->vram[off];
     }

     if (addr >= CRAM_BASE && addr < CRAM_END)
          return gb_cart_ram_readb(gb, addr - CRAM_BASE);

     if (addr >= OAM_BASE && addr < OAM_END)
          return gb->gpu.oam[addr - OAM_BASE];

     if (addr >= OAM_END && addr < 0xff00)
          return 0xff;

     if (addr == REG_INPUT)
          return gb_input_get_state(gb);
     if (addr == REG_SB)
          return gb->serial_data;
     if (addr == REG_SC)
          return gb->serial_control | 0x7e;
     if (addr == REG_DIV)
          return gb->timer.divider_counter >> 8;
     if (addr == REG_TIMA)
          return gb->timer.counter;
     if (addr == REG_TMA)
          return gb->timer.modulo;
     if (addr == REG_TAC)
          return gb_timer_get_config(gb);
     if (addr == REG_IF)
          return gb->irq.irq_flags | 0xE0;

     if (addr == REG_NR10)
          return 0x80 | gb->spu.nr1.sweep.shift |
                 (gb->spu.nr1.sweep.subtract << 3) |
                 (gb->spu.nr1.sweep.time << 4);
     if (addr == REG_NR11)
          return (gb->spu.nr1.wave.duty_cycle << 6) | 0x3f;
     if (addr == REG_NR12)
          return gb->spu.nr1.envelope_config;
     if (addr == REG_NR13)
          return 0xff;
     if (addr == REG_NR14)
          return (gb->spu.nr1.duration.enable << 6) | 0xbf;
     if (addr == REG_NR21)
          return (gb->spu.nr2.wave.duty_cycle << 6) | 0x3f;
     if (addr == REG_NR22)
          return gb->spu.nr2.envelope_config;
     if (addr == REG_NR23)
          return 0xff;
     if (addr == REG_NR24)
          return (gb->spu.nr2.duration.enable << 6) | 0xbf;
     if (addr == REG_NR30)
          return (gb->spu.nr3.enable << 7) | 0x7f;
     if (addr == REG_NR31)
          return 0xff;
     if (addr == REG_NR32)
          return (gb->spu.nr3.volume_shift << 5) | 0x9f;
     if (addr == REG_NR33)
          return 0xff;
     if (addr == REG_NR34)
          return (gb->spu.nr3.duration.enable << 6) | 0xbf;
     if (addr == REG_NR41)
          return 0xff;
     if (addr == REG_NR42)
          return gb->spu.nr4.envelope_config;
     if (addr == REG_NR43)
          return gb->spu.nr4.lfsr_config;
     if (addr == REG_NR44)
          return (gb->spu.nr4.duration.enable << 6) | 0xbf;
     if (addr == REG_NR50)
          return gb->spu.output_level;
     if (addr == REG_NR51)
          return gb->spu.sound_mux;
     if (addr == REG_NR52)
     {
          uint8_t r = 0x70;
          r |= gb->spu.nr1.running;
          r |= gb->spu.nr2.running << 1;
          r |= gb->spu.nr3.running << 2;
          r |= gb->spu.nr4.running << 3;
          r |= gb->spu.enable << 7;
          return r;
     }

     if (addr >= NR3_RAM_BASE && addr < NR3_RAM_END)
          return gb->spu.nr3.ram[addr - NR3_RAM_BASE];

     if (addr == REG_LCDC)
     {
          struct gb_gpu *gpu = &gb->gpu;
          uint8_t lcdc = 0;
          lcdc |= gpu->bg_enable << 0;
          lcdc |= gpu->sprite_enable << 1;
          lcdc |= gpu->tall_sprites << 2;
          lcdc |= gpu->bg_use_high_tm << 3;
          lcdc |= gpu->bg_window_use_sprite_ts << 4;
          lcdc |= gpu->window_enable << 5;
          lcdc |= gpu->window_use_high_tm << 6;
          lcdc |= gpu->master_enable << 7;
          return lcdc;
     }
     if (addr == REG_LCD_STAT)
     {
          struct gb_gpu *gpu = &gb->gpu;
          uint8_t r = 0;
          if (!gpu->master_enable)
               return 0;
          r |= gb_gpu_get_mode(gb);
          r |= (gpu->ly == gpu->lyc) << 2;
          r |= gpu->iten_mode0 << 3;
          r |= gpu->iten_mode1 << 4;
          r |= gpu->iten_mode2 << 5;
          r |= gpu->iten_lyc << 6;
          return r;
     }
     if (addr == REG_SCY)
          return gb->gpu.scy;
     if (addr == REG_SCX)
          return gb->gpu.scx;
     if (addr == REG_LY)
          return gb->gpu.ly;
     if (addr == REG_LYC)
          return gb->gpu.lyc;
     if (addr == REG_DMA)
          return gb->dma.source >> 8;
     if (addr == REG_BGP)
          return gb->gpu.bgp;
     if (addr == REG_OBP0)
          return gb->gpu.obp0;
     if (addr == REG_OBP1)
          return gb->gpu.obp1;
     if (addr == REG_WY)
          return gb->gpu.wy;
     if (addr == REG_WX)
          return gb->gpu.wx;
     if (addr == REG_IE)
          return gb->irq.irq_enable;

     if (gb->gbc && addr == REG_KEY1)
          return (gb->double_speed << 7) | gb->speed_switch_pending | 0x7e;
     if (gb_memory_is_cgb_hardware(gb) && addr == REG_VBK)
          return gb->vram_high_bank | 0xfe;
     if (gb->gbc && addr == REG_HDMA1)
          return gb->hdma.source >> 8;
     if (gb->gbc && addr == REG_HDMA2)
          return gb->hdma.source & 0xff;
     if (gb->gbc && addr == REG_HDMA3)
          return gb->hdma.destination >> 8;
     if (gb->gbc && addr == REG_HDMA4)
          return gb->hdma.destination & 0xff;
     if (gb->gbc && addr == REG_HDMA5)
          return ((!gb->hdma.run_on_hblank) << 7) | (gb->hdma.length & 0x7f);
     if (gb_memory_is_cgb_hardware(gb) && addr == REG_BCPS)
          return (gb->gpu.bg_palettes.auto_increment << 7) |
                 gb->gpu.bg_palettes.write_index | 0x40;
     if (gb->gbc && addr == REG_BCPD)
     {
          struct gb_color_palette *p = &gb->gpu.bg_palettes;
          uint16_t index = p->write_index;
          unsigned palette = index >> 3;
          unsigned color_index = (index >> 1) & 3;
          uint16_t col = p->colors[palette][color_index];
          return (index & 1) ? (col >> 8) : (col & 0xff);
     }
     if (gb_memory_is_cgb_hardware(gb) && addr == REG_OCPS)
          return (gb->gpu.sprite_palettes.auto_increment << 7) |
                 gb->gpu.sprite_palettes.write_index | 0x40;
     if (gb->gbc && addr == REG_OCPD)
     {
          struct gb_color_palette *p = &gb->gpu.sprite_palettes;
          uint16_t index = p->write_index;
          unsigned palette = index >> 3;
          unsigned color_index = (index >> 1) & 3;
          uint16_t col = p->colors[palette][color_index];
          return (index & 1) ? (col >> 8) : (col & 0xff);
     }
     if (gb->gbc && addr == REG_OPRI)
          return (gb->gpu.opri & 0x01) | 0xfe;
     if (gb->gbc && addr == REG_SVBK)
          return gb->iram_high_bank | 0xf8;
     if (gb_memory_is_cgb_hardware(gb) && addr == REG_CGB_FF72)
          return gb->cgb_reg_ff72;
     if (gb_memory_is_cgb_hardware(gb) && addr == REG_CGB_FF73)
          return gb->cgb_reg_ff73;
     if (gb_memory_is_cgb_hardware(gb) && addr == REG_CGB_FF75)
          return gb->cgb_reg_ff75 | 0x8f;
     if (gb_memory_is_cgb_hardware(gb) && (addr == REG_PCM12 || addr == REG_PCM34))
          return 0x00;

     if (addr == REG_BOOT)
          return gb->bootrom_mapped ? 0x00 : 0xff;
     if (addr < ZRAM_BASE)
          return 0xff;

     return 0xff;
}

/* Lê um byte da memória no endereço `addr`, acionando watchpoints e sincronizando dispositivos. */
uint8_t gb_memory_readb(struct gb *gb, uint16_t addr)
{
     /* Verifica watchpoints de leitura */
     gb_debug_check_watchpoint(gb, addr, GB_WATCHPOINT_READ);

     /* ── Hardware viz bus activity ── */
     if (!gb->dma.syncing) {
          struct gb_sys_viz *sv = &gb->debug.sys_viz;
          sv->last_bus_addr = addr;
          if      (addr < 0x8000u)                            sv->fade_cpu_rom  = 1.0f;
          else if (addr < 0xa000u)                            sv->fade_cpu_vram = 1.0f;
          else if (addr < 0xc000u)                            sv->fade_cpu_rom  = 1.0f;
          else if (addr < 0xfe00u)                            sv->fade_cpu_wram = 1.0f;
          else if (addr < 0xff00u)                            sv->fade_cpu_oam  = 1.0f;
          else                                                sv->fade_cpu_io   = 1.0f;
          gb_debug_hw_trace_cpu_read(gb, addr, 0);
     }

     gb_memory_sync_dma_before_cpu_access(gb);

     /* O DMA de OAM ocupa o barramento de origem. No DMG a CPU só acessa a HRAM;
      * no CGB a CPU ainda acessa o outro barramento principal (cartucho vs WRAM). */
     if (gb_memory_dma_blocks_cpu_access(gb, addr))
     {
          if (addr >= OAM_BASE && addr < 0xff00U)
               return 0xff;

          return gb_memory_dma_conflict_read(gb);
     }

     if (gb->bootrom_mapped && gb->bootrom)
     {
          if (addr < 0x0100)
               return gb->bootrom[addr];
          if (gb->gbc && addr >= 0x0200 && addr < 0x0900)
               return gb->bootrom[addr];
     }

     if (addr >= ROM_BASE && addr < ROM_END)
     {
          return gb_cart_rom_readb(gb, addr - ROM_BASE);
     }

     if (addr >= ZRAM_BASE && addr < ZRAM_END)
     {
          return gb->zram[addr - ZRAM_BASE];
     }

     if (addr >= IRAM_BASE && addr < IRAM_END)
     {
          uint16_t off = gb_memory_iram_off(gb, addr - IRAM_BASE);

          return gb->iram[off];
     }

     if (addr >= IRAM_ECHO_BASE && addr < IRAM_ECHO_END)
     {
          uint16_t off = gb_memory_iram_off(gb, addr - IRAM_ECHO_BASE);

          return gb->iram[off];
     }

     if (addr >= VRAM_BASE && addr < VRAM_END)
     {
          gb_gpu_sync(gb);

          /* Leituras da VRAM são bloqueadas um pouco antes do Mode 3 gravável. */
          if (gb_gpu_vram_read_blocked(gb))
          {
               return 0xff;
          }

          uint16_t off = addr - VRAM_BASE;

          off += 0x2000 * gb->vram_high_bank;

          return gb->vram[off];
     }

     if (addr >= CRAM_BASE && addr < CRAM_END)
     {
          return gb_cart_ram_readb(gb, addr - CRAM_BASE);
     }

     if (addr >= OAM_BASE && addr < OAM_END)
     {
          gb_gpu_sync(gb);

          /* A OAM é inacessível durante os modos 2 e 3; no DMG o modo 2 pode corrompê-la. */
          if (gb_gpu_oam_read_blocked(gb))
          {
               gb_memory_trigger_oam_bug_read(gb, addr);
               return 0xff;
          }

          return gb->gpu.oam[addr - OAM_BASE];
     }

     if (addr >= OAM_END && addr < 0xff00)
     {
          gb_gpu_sync(gb);

          if (gb_gpu_oam_read_blocked(gb))
          {
               gb_memory_trigger_oam_bug_read(gb, addr);
          }

          return 0xff;
     }

     if (addr == REG_INPUT)
     {
          return gb_input_get_state(gb);
     }

     if (addr == REG_SB)
     {
          return gb->serial_data;
     }

     if (addr == REG_SC)
     {
          return gb->serial_control | 0x7e;
     }

     if (addr == REG_DIV)
     {
          gb_timer_sync(gb);
          /* Retorna os 8 bits altos do contador divisor */
          return gb->timer.divider_counter >> 8;
     }

     if (addr == REG_TIMA)
     {
          bool was_reloading = gb->timer.reload_pending;
          gb_timer_sync(gb);
          if (was_reloading && !gb->timer.reload_just_happened)
               return 0x00;
          return gb->timer.counter;
     }

     if (addr == REG_TMA)
     {
          return gb->timer.modulo;
     }

     if (addr == REG_TAC)
     {
          return gb_timer_get_config(gb);
     }

     if (addr == REG_IF)
     {
          return gb->irq.irq_flags | 0xE0;
     }

     if (addr == REG_NR10)
     {
          uint8_t r = 0x80;

          r |= gb->spu.nr1.sweep.shift;
          r |= gb->spu.nr1.sweep.subtract << 3;
          r |= gb->spu.nr1.sweep.time << 4;

          return r;
     }

     if (addr == REG_NR11)
     {
          return (gb->spu.nr1.wave.duty_cycle << 6) | 0x3f;
     }

     if (addr == REG_NR12)
     {
          return gb->spu.nr1.envelope_config;
     }

     if (addr == REG_NR13)
     {
          /* Somente escrita */
          return 0xff;
     }

     if (addr == REG_NR14)
     {
          return (gb->spu.nr1.duration.enable << 6) | 0xbf;
     }

     if (addr == REG_NR21)
     {
          return (gb->spu.nr2.wave.duty_cycle << 6) | 0x3f;
     }

     if (addr == REG_NR22)
     {
          return gb->spu.nr2.envelope_config;
     }

     if (addr == REG_NR23)
     {
          /* Somente escrita */
          return 0xff;
     }

     if (addr == REG_NR24)
     {
          return (gb->spu.nr2.duration.enable << 6) | 0xbf;
     }

     if (addr == REG_NR30)
     {
          gb_spu_sync(gb);
          return (gb->spu.nr3.enable << 7) | 0x7f;
     }

     if (addr == REG_NR31)
     {
          /* Somente escrita */
          return 0xff;
     }

     if (addr == REG_NR32)
     {
          return (gb->spu.nr3.volume_shift << 5) | 0x9f;
     }

     if (addr == REG_NR33)
     {
          /* Somente escrita */
          return 0xff;
     }

     if (addr == REG_NR34)
     {
          return (gb->spu.nr3.duration.enable << 6) | 0xbf;
     }

     if (addr == REG_NR41)
     {
          /* Somente leitura */
          return 0xff;
     }

     if (addr == REG_NR42)
     {
          return gb->spu.nr4.envelope_config;
     }

     if (addr == REG_NR43)
     {
          return gb->spu.nr4.lfsr_config;
     }

     if (addr == REG_NR44)
     {
          return (gb->spu.nr4.duration.enable << 6) | 0xbf;
     }

     if (addr == REG_NR50)
     {
          return gb->spu.output_level;
     }

     if (addr == REG_NR51)
     {
          return gb->spu.sound_mux;
     }

     if (addr == REG_NR52)
     {
          uint8_t r = 0x70;

          gb_spu_sync(gb);

          r |= gb->spu.nr1.running;
          r |= gb->spu.nr2.running << 1;
          r |= gb->spu.nr3.running << 2;
          r |= gb->spu.nr4.running << 3;
          r |= gb->spu.enable << 7;

          return r;
     }

     if (addr >= NR3_RAM_BASE && addr < NR3_RAM_END)
     {
          if (gb->spu.nr3.running)
          {
               gb_spu_sync(gb);
               if (!gb->gbc && gb->spu.nr3.access_cycles <= 0)
               {
                    /* DMG: a RAM de onda só é acessível dentro de ~2 ciclos em que
                     * a unidade de onda lê um byte; fora dessa janela retorna 0xFF */
                    return 0xff;
               }
               return gb->spu.nr3.ram[gb->spu.nr3.index / 2];
          }
          return gb->spu.nr3.ram[addr - NR3_RAM_BASE];
     }

     if (addr == REG_LCDC)
     {
          return gb_gpu_get_lcdc(gb);
     }

     if (addr == REG_LCD_STAT)
     {
          return gb_gpu_get_lcd_stat(gb);
     }

     if (addr == REG_SCY)
     {
          return gb->gpu.scy;
     }

     if (addr == REG_SCX)
     {
          return gb->gpu.scx;
     }

     if (addr == REG_LY)
     {
          return gb_gpu_get_ly(gb);
     }

     if (addr == REG_LYC)
     {
          return gb->gpu.lyc;
     }

     if (addr == REG_DMA)
     {
          return gb->dma.source >> 8;
     }

     if (addr == REG_BGP)
     {
          return gb->gpu.bgp;
     }

     if (addr == REG_OBP0)
     {
          return gb->gpu.obp0;
     }

     if (addr == REG_OBP1)
     {
          return gb->gpu.obp1;
     }

     if (addr == REG_WY)
     {
          return gb->gpu.wy;
     }

     if (addr == REG_WX)
     {
          return gb->gpu.wx;
     }

     if (addr == REG_IE)
     {
          return gb->irq.irq_enable;
     }

     if (gb->gbc && addr == REG_KEY1)
     {
          uint8_t r = 0;

          r |= gb->double_speed << 7;
          r |= gb->speed_switch_pending;

          return r | 0x7e;
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_VBK)
     {
          return gb->vram_high_bank | 0xfe;
     }

     if (gb->gbc && addr == REG_HDMA1)
          return gb->hdma.source >> 8;

     if (gb->gbc && addr == REG_HDMA2)
          return gb->hdma.source & 0xff;

     if (gb->gbc && addr == REG_HDMA3)
          return gb->hdma.destination >> 8;

     if (gb->gbc && addr == REG_HDMA4)
          return gb->hdma.destination & 0xff;

     if (gb->gbc && addr == REG_HDMA5)
     {
          /* Bit 7 = 0 quando a transferência H-Blank está ativa */
          uint8_t r = (!gb->hdma.run_on_hblank) << 7;
          r |= gb->hdma.length & 0x7f;
          return r;
     }

     if (gb->gbc && addr == REG_RP)
     {
          /* Bit 7: read-enable (echo); Bit 6: IR received (0 = sem sinal);
           * Bits 5-2: reservados (lêem 1 no hardware); Bit 1: LED state. */
          return (gb->ir_port & 0x83) | 0x3C;
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_BCPS)
     {
          uint8_t r = 0;

          r |= gb->gpu.bg_palettes.auto_increment << 7;
          r |= gb->gpu.bg_palettes.write_index;

          return r | 0x40;
     }

     if (gb->gbc && addr == REG_BCPD)
     {
          return gb_memory_cgb_palette_read(gb, &gb->gpu.bg_palettes);
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_OCPS)
     {
          uint8_t r = 0;

          r |= gb->gpu.sprite_palettes.auto_increment << 7;
          r |= gb->gpu.sprite_palettes.write_index;

          return r | 0x40;
     }

     if (gb->gbc && addr == REG_OCPD)
     {
          return gb_memory_cgb_palette_read(gb, &gb->gpu.sprite_palettes);
     }

     if (gb->gbc && addr == REG_OPRI)
     {
          /* Bits 7-1 leem como 1; bit 0 = modo de prioridade de sprites */
          return (gb->gpu.opri & 0x01) | 0xfe;
     }

     if (gb->gbc && addr == REG_SVBK)
     {
          return gb->iram_high_bank | 0xf8;
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_CGB_FF72)
          return gb->cgb_reg_ff72;

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_CGB_FF73)
          return gb->cgb_reg_ff73;

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_CGB_FF75)
          return gb->cgb_reg_ff75 | 0x8f;

     if (gb_memory_is_cgb_hardware(gb) && (addr == REG_PCM12 || addr == REG_PCM34))
          return 0x00;

     if (addr == REG_BOOT)
          return gb->bootrom_mapped ? 0x00 : 0xff;

     /* 0xff71-0xff7f são registradores de I/O não utilizados; leituras retornam 0xff */
     if (addr < ZRAM_BASE)
          return 0xff;

     printf("Unsupported read at address 0x%04x\n", addr);

     return 0xff;
}

void gb_memory_writeb(struct gb *gb, uint16_t addr, uint8_t val)
{
     /* Verifica watchpoints de escrita */
     gb_debug_check_watchpoint(gb, addr, GB_WATCHPOINT_WRITE);

     /* ── Hardware viz bus activity ── */
     if (!gb->dma.syncing) {
          struct gb_sys_viz *sv = &gb->debug.sys_viz;
          sv->last_bus_addr = addr;
          sv->last_bus_data = val;
          if      (addr < 0x8000u)                            sv->fade_cpu_rom  = 1.0f;
          else if (addr < 0xa000u)                            sv->fade_cpu_vram = 1.0f;
          else if (addr < 0xc000u)                            sv->fade_cpu_rom  = 1.0f;
          else if (addr < 0xfe00u)                            sv->fade_cpu_wram = 1.0f;
          else if (addr < 0xff00u)                            sv->fade_cpu_oam  = 1.0f;
          else                                                sv->fade_cpu_io   = 1.0f;
          gb_debug_hw_trace_cpu_write(gb, addr, val);
     }

     gb_memory_sync_dma_before_cpu_access(gb);

     /* Durante o DMA de OAM, escritas no barramento ocupado não chegam ao destino. */
     if (gb_memory_dma_blocks_cpu_access(gb, addr))
     {
          return;
     }

     if (addr >= ROM_BASE && addr < ROM_END)
     {
          gb_cart_rom_writeb(gb, addr - ROM_BASE, val);
          return;
     }

     if (addr >= ZRAM_BASE && addr < ZRAM_END)
     {
          gb->zram[addr - ZRAM_BASE] = val;
          return;
     }

     if (addr >= IRAM_BASE && addr < IRAM_END)
     {
          uint16_t off = gb_memory_iram_off(gb, addr - IRAM_BASE);

          gb->iram[off] = val;
          return;
     }

     if (addr >= IRAM_ECHO_BASE && addr < IRAM_ECHO_END)
     {
          uint16_t off = gb_memory_iram_off(gb, addr - IRAM_ECHO_BASE);

          gb->iram[off] = val;
          return;
     }

     if (addr >= VRAM_BASE && addr < VRAM_END)
     {
          gb_gpu_sync(gb);

          /* Escritas na VRAM são ignoradas durante o Mode 3 (renderização). */
          if (gb_gpu_vram_blocked(gb))
          {
               return;
          }

          uint16_t off = addr - VRAM_BASE;

          off += 0x2000 * gb->vram_high_bank;

          gb->vram[off] = val;
          return;
     }

     if (addr >= CRAM_BASE && addr < CRAM_END)
     {
          gb_cart_ram_writeb(gb, addr - CRAM_BASE, val);
          return;
     }

     if (addr >= OAM_BASE && addr < OAM_END)
     {
          gb_gpu_sync(gb);

          /* A OAM é inacessível durante os modos 2 e 3; no DMG o modo 2 pode corrompê-la. */
          if (gb_gpu_oam_write_blocked(gb))
          {
               gb_memory_trigger_oam_bug(gb, addr);
               return;
          }

          gb->gpu.oam[addr - OAM_BASE] = val;
          return;
     }

     if (addr >= OAM_END && addr < 0xff00)
     {
          gb_gpu_sync(gb);

          if (gb_gpu_oam_write_blocked(gb))
          {
               gb_memory_trigger_oam_bug(gb, addr);
          }

          return;
     }

     if (addr == REG_INPUT)
     {
          gb_input_select(gb, val);
          return;
     }

     if (addr == REG_SB)
     {
          gb->serial_data = val;
          return;
     }

     if (addr == REG_SC)
     {
          gb->serial_control = val | 0x7e;

          if ((val & 0x81) == 0x81)
          {
               gb_timer_sync(gb);
               gb_sync_next(gb, GB_SYNC_SERIAL, gb_serial_transfer_cycles(gb));
          }
          return;
     }

     if (addr == REG_DIV)
     {
          uint16_t old_divider;

          gb_timer_sync(gb);
          gb_spu_sync(gb);
          old_divider = gb->timer.divider_counter;
          /* Escrever no divisor zera-o (independente do valor escrito) */
          gb_timer_reset_divider(gb);
          gb_spu_div_reset(gb, old_divider);
          return;
     }

     if (addr == REG_TIMA)
     {
          gb_timer_write_counter(gb, val);
          return;
     }

     if (addr == REG_TMA)
     {
          gb_timer_write_modulo(gb, val);
          return;
     }

     if (addr == REG_TAC)
     {
          gb_timer_set_config(gb, val);
          return;
     }

     if (addr == REG_IF)
     {
          gb->irq.irq_flags = val | 0xE0;
          return;
     }

     if (addr == REG_IE)
     {
          gb->irq.irq_enable = val;
          return;
     }

     if (addr == REG_NR10)
     {
          if (gb->spu.enable)
          {
               bool old_subtract;
               bool old_subtract_calculated;

               gb_spu_sync(gb);
               old_subtract = gb->spu.nr1.sweep.subtract;
               old_subtract_calculated = gb->spu.nr1.sweep.subtract_calculated;
               gb_spu_sweep_reload(&gb->spu.nr1.sweep, val);
               if (old_subtract && old_subtract_calculated &&
                   !gb->spu.nr1.sweep.subtract)
               {
                    gb->spu.nr1.running = false;
               }
          }
          return;
     }

     if (addr == REG_NR11)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr1.wave.duty_cycle = val >> 6;
               gb_memory_reload_spu_duration(gb, &gb->spu.nr1.duration,
                                             GB_SPU_NR1_T1_MAX,
                                             val & 0x3f);
          }
          else if (!gb->gbc)
          {
               /* DMG: bits do contador de duração são graváveis mesmo com o APU desligado */
               gb_spu_duration_reload(&gb->spu.nr1.duration,
                                      GB_SPU_NR1_T1_MAX, val & 0x3f);
          }
          return;
     }

     if (addr == REG_NR12)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               /* A configuração do envelope entra em vigor no início do som */
               gb->spu.nr1.envelope_config = val;
               if ((val & 0xf8) == 0)
               {
                    gb->spu.nr1.running = false;
               }
          }
          return;
     }

     if (addr == REG_NR13)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr1.sweep.divider.offset &= 0x700;
               gb->spu.nr1.sweep.divider.offset |= val;
          }
          return;
     }

     if (addr == REG_NR14)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr1.sweep.divider.offset &= 0xff;
               gb->spu.nr1.sweep.divider.offset |= ((uint16_t)val & 7) << 8;

               if (val & 0x80)
               {
                    gb_spu_nr1_start(gb);
               }

               gb_spu_duration_set_enable(gb, &gb->spu.nr1.duration,
                                          val & 0x40,
                                          &gb->spu.nr1.running,
                                          val & 0x80,
                                          GB_SPU_NR1_T1_MAX + 1);
          }
          return;
     }

     if (addr == REG_NR21)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr2.wave.duty_cycle = val >> 6;
               gb_memory_reload_spu_duration(gb, &gb->spu.nr2.duration,
                                             GB_SPU_NR2_T1_MAX,
                                             val & 0x3f);
          }
          else if (!gb->gbc)
          {
               /* DMG: bits do contador de duração são graváveis mesmo com o APU desligado */
               gb_spu_duration_reload(&gb->spu.nr2.duration,
                                      GB_SPU_NR2_T1_MAX, val & 0x3f);
          }
          return;
     }

     if (addr == REG_NR22)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               /* A configuração do envelope entra em vigor no início do som */
               gb->spu.nr2.envelope_config = val;
               if ((val & 0xf8) == 0)
               {
                    gb->spu.nr2.running = false;
               }
          }
          return;
     }

     if (addr == REG_NR23)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr2.divider.offset &= 0x700;
               gb->spu.nr2.divider.offset |= val;
          }
          return;
     }

     if (addr == REG_NR24)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr2.divider.offset &= 0xff;
               gb->spu.nr2.divider.offset |= ((uint16_t)val & 7) << 8;

               if (val & 0x80)
               {
                    gb_spu_nr2_start(gb);
               }

               gb_spu_duration_set_enable(gb, &gb->spu.nr2.duration,
                                          val & 0x40,
                                          &gb->spu.nr2.running,
                                          val & 0x80,
                                          GB_SPU_NR2_T1_MAX + 1);
          }
          return;
     }

     if (addr == REG_NR30)
     {
          if (gb->spu.enable)
          {
               /* Desabilitar o canal 3 o para. Habilitá-lo não o inicia
                * até que 0x80 seja escrito em NR34. */
               bool enable = (val & 0x80);

               gb_spu_sync(gb);
               gb->spu.nr3.enable = enable;
               if (!enable)
               {
                    gb->spu.nr3.running = false;
               }
          }
          return;
     }

     if (addr == REG_NR31)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr3.t1 = val;
               gb_memory_reload_spu_duration(gb, &gb->spu.nr3.duration,
                                             GB_SPU_NR3_T1_MAX,
                                             val);
          }
          else if (!gb->gbc)
          {
               /* DMG: contador de duração gravável mesmo com o APU desligado */
               gb->spu.nr3.t1 = val;
               gb_spu_duration_reload(&gb->spu.nr3.duration,
                                      GB_SPU_NR3_T1_MAX, val);
          }
          return;
     }

     if (addr == REG_NR32)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr3.volume_shift = (val >> 5) & 3;
          }
          return;
     }

     if (addr == REG_NR33)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr3.divider.offset &= 0x700;
               gb->spu.nr3.divider.offset |= val;
          }
          return;
     }

     if (addr == REG_NR34)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr3.divider.offset &= 0xff;
               gb->spu.nr3.divider.offset |= ((uint16_t)val & 7) << 8;

               if (val & 0x80)
               {
                    gb_spu_nr3_start(gb);
               }

               gb_spu_duration_set_enable(gb, &gb->spu.nr3.duration,
                                          val & 0x40,
                                          &gb->spu.nr3.running,
                                          val & 0x80,
                                          GB_SPU_NR3_T1_MAX + 1);
          }
          return;
     }

     if (addr == REG_NR41)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb_memory_reload_spu_duration(gb, &gb->spu.nr4.duration,
                                             GB_SPU_NR4_T1_MAX,
                                             val & 0x3f);
          }
          else if (!gb->gbc)
          {
               /* DMG: contador de duração gravável mesmo com o APU desligado */
               gb_spu_duration_reload(&gb->spu.nr4.duration,
                                      GB_SPU_NR4_T1_MAX, val & 0x3f);
          }
          return;
     }

     if (addr == REG_NR42)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               /* A configuração do envelope entra em vigor no início do som */
               gb->spu.nr4.envelope_config = val;
               if ((val & 0xf8) == 0)
               {
                    gb->spu.nr4.running = false;
               }
          }
          return;
     }

     if (addr == REG_NR43)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.nr4.lfsr_config = val;
          }
          return;
     }

     if (addr == REG_NR44)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);

               if (val & 0x80)
               {
                    gb_spu_nr4_start(gb);
               }

               gb_spu_duration_set_enable(gb, &gb->spu.nr4.duration,
                                          val & 0x40,
                                          &gb->spu.nr4.running,
                                          val & 0x80,
                                          GB_SPU_NR4_T1_MAX + 1);
          }
          return;
     }

     if (addr == REG_NR50)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.output_level = val;
               gb_spu_update_sound_amp(gb);
          }
          return;
     }

     if (addr == REG_NR51)
     {
          if (gb->spu.enable)
          {
               gb_spu_sync(gb);
               gb->spu.sound_mux = val;
               gb_spu_update_sound_amp(gb);
          }
          return;
     }

     if (addr == REG_NR52)
     {
          bool enable = val & 0x80;

          if (gb->spu.enable == enable)
          {
               /* Sem alteração */
               return;
          }

          gb_spu_sync(gb);

          if (!enable)
          {
               gb_spu_power_off(gb);
          }
          else
          {
               gb->spu.frame_seq_step = 0;
               gb->spu.enable = true;
          }

          return;
     }

     if (addr >= NR3_RAM_BASE && addr < NR3_RAM_END)
     {
          if (gb->spu.nr3.running)
          {
               gb_spu_sync(gb);
               if (!gb->gbc && gb->spu.nr3.access_cycles <= 0)
               {
                    /* DMG: escritas fora da janela de acesso de 2 ciclos são ignoradas */
                    return;
               }
               /* DMG/CGB: escrita redirecionada ao byte na posição atual da onda */
               gb->spu.nr3.ram[gb->spu.nr3.index / 2] = val;
          }
          else
          {
               gb->spu.nr3.ram[addr - NR3_RAM_BASE] = val;
          }
          return;
     }

     if (addr == REG_LCDC)
     {
          gb_gpu_set_lcdc(gb, val);
          return;
     }

     if (addr == REG_LCD_STAT)
     {
          gb_gpu_set_lcd_stat(gb, val);
          return;
     }

     if (addr == REG_SCY)
     {
          gb_gpu_sync(gb);
          gb->gpu.scy = val;
          return;
     }

     if (addr == REG_SCX)
     {
          gb_gpu_sync(gb);
          gb->gpu.scx = val;
          return;
     }

     if (addr == REG_LY)
     {
          /* Escrever em LY zera o contador de scanline no hardware real.
           * O valor escrito é ignorado — qualquer escrita redefine LY para 0. */
          gb_gpu_sync(gb);
          gb_gpu_reset_scanline(gb);
          gb_gpu_sync(gb);
          return;
     }

     if (addr == REG_LYC)
     {
          gb_gpu_set_lyc(gb, val);
          return;
     }

     if (addr == REG_DMA)
     {
          gb_dma_start(gb, val);
          return;
     }

     if (addr == REG_BGP)
     {
          gb_gpu_sync(gb);
          gb->gpu.bgp = val;
          return;
     }

     if (addr == REG_OBP0)
     {
          gb_gpu_sync(gb);
          gb->gpu.obp0 = val;
          return;
     }

     if (addr == REG_OBP1)
     {
          gb_gpu_sync(gb);
          gb->gpu.obp1 = val;
          return;
     }

     if (addr == REG_WY)
     {
          gb_gpu_sync(gb);
          gb->gpu.wy = val;
          return;
     }

     if (addr == REG_WX)
     {
          gb_gpu_sync(gb);
          gb->gpu.wx = val;
          return;
     }

     if (gb->gbc && addr == REG_KEY1)
     {
          gb->speed_switch_pending = val & 1;
          return;
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_VBK)
     {
          gb->vram_high_bank = val & 1;
          return;
     }

     if (addr == REG_BOOT)
     {
          /* Escrever qualquer valor não nulo desmapeia a boot ROM (trava unidirecional) */
          if (val != 0)
               gb->bootrom_mapped = false;
          return;
     }

     if (gb->gbc && addr == REG_HDMA1)
     {
          gb->hdma.source &= 0x00ff;
          gb->hdma.source |= (uint16_t)val << 8;
          return;
     }

     if (gb->gbc && addr == REG_HDMA2)
     {
          gb->hdma.source &= 0xff00;
          gb->hdma.source |= val & 0xf0; /* bits 3–0 ignorados */
          return;
     }

     if (gb->gbc && addr == REG_HDMA3)
     {
          gb->hdma.destination &= 0x00ff;
          gb->hdma.destination |= (uint16_t)val << 8;
          return;
     }

     if (gb->gbc && addr == REG_HDMA4)
     {
          gb->hdma.destination &= 0xff00;
          gb->hdma.destination |= val & 0xf0; /* bits 3–0 ignorados */
          return;
     }

     if (gb->gbc && addr == REG_HDMA5)
     {
          bool run_on_hblank = val & 0x80;

          gb->hdma.length = val & 0x7f;

          if (!run_on_hblank && gb->hdma.run_on_hblank)
          {
               /* Escrever 0 no bit 7 quando H-Blank DMA está ativa cancela a transferência */
               gb_gpu_sync(gb);
               gb->hdma.run_on_hblank = false;
          }
          else
          {
               gb_hdma_start(gb, run_on_hblank);
          }
          return;
     }

     if (gb->gbc && addr == REG_RP)
     {
          /* Bit 7: read-enable; Bit 1: LED output. Bit 6 (IR received) é só-leitura. */
          gb->ir_port = val & 0x83;
          return;
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_BCPS)
     {
          gb->gpu.bg_palettes.auto_increment = val & 0x80;
          gb->gpu.bg_palettes.write_index = val & 0x3f;
          return;
     }

     if (gb->gbc && addr == REG_BCPD)
     {
          gb_memory_cgb_palette_write(gb, &gb->gpu.bg_palettes, val);
          return;
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_OCPS)
     {
          gb->gpu.sprite_palettes.auto_increment = val & 0x80;
          gb->gpu.sprite_palettes.write_index = val & 0x3f;
          return;
     }

     if (gb->gbc && addr == REG_OCPD)
     {
          gb_memory_cgb_palette_write(gb, &gb->gpu.sprite_palettes, val);
          return;
     }

     if (gb->gbc && addr == REG_OPRI)
     {
          /* Apenas o bit 0 é gravável; locked após o boot ROM desabilitar-se */
          if (gb->bootrom_mapped)
               gb->gpu.opri = val & 0x01;
          return;
     }

     if (gb->gbc && addr == REG_SVBK)
     {
          gb->iram_high_bank = val & 7;
          return;
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_CGB_FF72)
     {
          gb->cgb_reg_ff72 = val;
          return;
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_CGB_FF73)
     {
          gb->cgb_reg_ff73 = val;
          return;
     }

     if (gb_memory_is_cgb_hardware(gb) && addr == REG_CGB_FF75)
     {
          gb->cgb_reg_ff75 = val;
          return;
     }

     if (gb_memory_is_cgb_hardware(gb) && (addr == REG_PCM12 || addr == REG_PCM34))
     {
          return;
     }

     /* Registradores de I/O não mapeados (0xFF00–0xFF7F) são silenciosamente
      * ignorados pelo hardware — ex: 0xFF15 (NR20 inexistente), 0xFF1F, 0xFF27–0xFF2F. */
     if (addr >= 0xff00 && addr < ZRAM_BASE)
          return;

     printf("Unsupported write at address 0x%04x [val=0x%02x]\n", addr, val);
}
