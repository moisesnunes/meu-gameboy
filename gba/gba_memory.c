#include <string.h>
#include "gba.h"

void gba_memory_reset(struct gba *gba)
{
     memset(gba->ewram, 0, sizeof(gba->ewram));
     memset(gba->iwram, 0, sizeof(gba->iwram));
     memset(gba->pram, 0, sizeof(gba->pram));
     memset(gba->vram, 0, sizeof(gba->vram));
     memset(gba->oam, 0, sizeof(gba->oam));
}

static bool timer0_irq_edge_accepted(struct gba *gba, uint16_t ie)
{
     uint16_t bit = (uint16_t)(1u << GBA_IRQ_TIMER0);
     int32_t delta = gba->sync.next_event[GBA_SYNC_TIMER] - gba->timestamp;

     return (ie & bit) &&
            gba->timer.ch[0].enable &&
            !gba->timer.ch[0].cascade &&
            gba->timer.ch[0].irq_en &&
            /* cancel-irq-ie shows this accepted-but-still-cancelable phase. */
            (delta == -6 || delta <= -8);
}

static bool irq_note_ie_write(struct gba *gba, uint16_t new_ie)
{
     uint16_t old_ie = gba->irq.ie;
     uint16_t cleared = old_ie & (uint16_t)~new_ie;

     if (!cleared || !gba->irq.ime || (gba->cpu.cpsr & GBA_CPSR_I))
          return false;

     if ((cleared & gba->irq.if_) ||
         ((cleared & (1u << GBA_IRQ_TIMER0)) && timer0_irq_edge_accepted(gba, old_ie)))
     {
          gba->irq.force = true;
          return true;
     }
     return false;
}

/* -------------------------------------------------------------------------
 * I/O register reads
 * ---------------------------------------------------------------------- */

static uint8_t io_read8(struct gba *gba, uint32_t addr)
{
     /* Align to 16-bit for most I/O */
     switch (addr)
     {
     /* GPU */
     case REG_DISPCNT ... REG_BLDY + 1:
          return gba_gpu_read8(gba, addr);

     /* APU: REG_SOUND1CNT_L(0x60)–REG_SOUNDCNT_X(0x84), SOUNDBIAS, wave RAM */
     case REG_SOUND1CNT_L ... REG_SOUNDCNT_X + 1:
     case REG_SOUNDBIAS:
     case REG_SOUNDBIAS + 1:
     case 0x04000090 ... 0x0400009F:
          return gba_apu_read_reg(gba, addr);

     /* Timers */
     case REG_TM0CNT_L:
          return gba_timer_read_counter(gba, 0) & 0xFF;
     case REG_TM0CNT_L + 1:
          return gba_timer_read_counter(gba, 0) >> 8;
     case REG_TM1CNT_L:
          return gba_timer_read_counter(gba, 1) & 0xFF;
     case REG_TM1CNT_L + 1:
          return gba_timer_read_counter(gba, 1) >> 8;
     case REG_TM2CNT_L:
          return gba_timer_read_counter(gba, 2) & 0xFF;
     case REG_TM2CNT_L + 1:
          return gba_timer_read_counter(gba, 2) >> 8;
     case REG_TM3CNT_L:
          return gba_timer_read_counter(gba, 3) & 0xFF;
     case REG_TM3CNT_L + 1:
          return gba_timer_read_counter(gba, 3) >> 8;
     case REG_TM0CNT_H:
          return (uint8_t)(gba->timer.ch[0].prescaler |
                           (gba->timer.ch[0].irq_en << 6) |
                           (gba->timer.ch[0].enable << 7));
     case REG_TM1CNT_H:
          return (uint8_t)(gba->timer.ch[1].prescaler |
                           (gba->timer.ch[1].cascade << 2) |
                           (gba->timer.ch[1].irq_en << 6) |
                           (gba->timer.ch[1].enable << 7));
     case REG_TM2CNT_H:
          return (uint8_t)(gba->timer.ch[2].prescaler |
                           (gba->timer.ch[2].cascade << 2) |
                           (gba->timer.ch[2].irq_en << 6) |
                           (gba->timer.ch[2].enable << 7));
     case REG_TM3CNT_H:
          return (uint8_t)(gba->timer.ch[3].prescaler |
                           (gba->timer.ch[3].cascade << 2) |
                           (gba->timer.ch[3].irq_en << 6) |
                           (gba->timer.ch[3].enable << 7));

     /* Input */
     case REG_KEYINPUT:
          return gba->input.keyinput & 0xFF;
     case REG_KEYINPUT + 1:
          return gba->input.keyinput >> 8;
     case REG_KEYCNT:
          return gba->input.keycnt & 0xFF;
     case REG_KEYCNT + 1:
          return gba->input.keycnt >> 8;

     /* IRQ */
     case REG_IE:
          return gba->irq.ie & 0xFF;
     case REG_IE + 1:
          return gba->irq.ie >> 8;
     case REG_IF:
          return gba->irq.if_ & 0xFF;
     case REG_IF + 1:
          return gba->irq.if_ >> 8;
     case REG_IME:
          return gba->irq.ime ? 1 : 0;

     case REG_WAITCNT:
          return gba->waitcnt & 0xFF;
     case REG_WAITCNT + 1:
          return gba->waitcnt >> 8;
     case REG_POSTFLG:
          return gba->postflg;

     default:
          return 0;
     }
}

static uint16_t dma_ctrl_read16(struct gba *gba, int n)
{
     struct gba_dma_channel *ch = &gba->dma.ch[n];
     return (uint16_t)((ch->dst_mode << 5) |
                       (ch->src_mode << 7) |
                       (ch->repeat << 9) |
                       (ch->word_32 << 10) |
                       (ch->gamepak_drq << 11) |
                       (ch->timing << 12) |
                       (ch->irq_en << 14) |
                       (ch->enable << 15));
}

static uint16_t io_read16(struct gba *gba, uint32_t addr)
{
     addr &= ~1U;
     switch (addr)
     {
     case REG_DISPCNT ... REG_BLDY:
          return gba_gpu_read16(gba, addr);
     case REG_SOUND1CNT_L ... REG_SOUNDCNT_X:
     case REG_SOUNDBIAS:
     case 0x04000090 ... 0x0400009E:
          return (uint16_t)(gba_apu_read_reg(gba, addr) |
                            ((uint16_t)gba_apu_read_reg(gba, addr + 1) << 8));
     case REG_TM0CNT_L:
          return gba_timer_read_counter(gba, 0);
     case REG_TM1CNT_L:
          return gba_timer_read_counter(gba, 1);
     case REG_TM2CNT_L:
          return gba_timer_read_counter(gba, 2);
     case REG_TM3CNT_L:
          return gba_timer_read_counter(gba, 3);
     case REG_TM0CNT_H:
          return (uint16_t)(gba->timer.ch[0].prescaler |
                            (gba->timer.ch[0].irq_en << 6) |
                            (gba->timer.ch[0].enable << 7));
     case REG_TM1CNT_H:
          return (uint16_t)(gba->timer.ch[1].prescaler |
                            (gba->timer.ch[1].cascade << 2) |
                            (gba->timer.ch[1].irq_en << 6) |
                            (gba->timer.ch[1].enable << 7));
     case REG_TM2CNT_H:
          return (uint16_t)(gba->timer.ch[2].prescaler |
                            (gba->timer.ch[2].cascade << 2) |
                            (gba->timer.ch[2].irq_en << 6) |
                            (gba->timer.ch[2].enable << 7));
     case REG_TM3CNT_H:
          return (uint16_t)(gba->timer.ch[3].prescaler |
                            (gba->timer.ch[3].cascade << 2) |
                            (gba->timer.ch[3].irq_en << 6) |
                            (gba->timer.ch[3].enable << 7));
     case REG_DMA0CNT_H:
          return dma_ctrl_read16(gba, 0);
     case REG_DMA1CNT_H:
          return dma_ctrl_read16(gba, 1);
     case REG_DMA2CNT_H:
          return dma_ctrl_read16(gba, 2);
     case REG_DMA3CNT_H:
          return dma_ctrl_read16(gba, 3);
     case REG_KEYINPUT:
          return gba->input.keyinput;
     case REG_KEYCNT:
          return gba->input.keycnt;
     case REG_IE:
          return gba->irq.ie;
     case REG_IF:
          return gba->irq.if_;
     case REG_WAITCNT:
          return gba->waitcnt;
     case REG_IME:
          return gba->irq.ime ? 1 : 0;
     case REG_POSTFLG:
          return gba->postflg;
     default:
          return (uint16_t)(io_read8(gba, addr) |
                            ((uint16_t)io_read8(gba, addr + 1) << 8));
     }
}

/* -------------------------------------------------------------------------
 * I/O register writes
 * ---------------------------------------------------------------------- */

static void io_write8(struct gba *gba, uint32_t addr, uint8_t val)
{
     switch (addr)
     {
     /* GPU */
     case REG_DISPCNT ... REG_BLDY + 1:
          gba_gpu_write8(gba, addr, val);
          break;

     /* APU: 0x60–0x84, SOUNDBIAS, wave RAM, FIFO */
     case REG_SOUND1CNT_L ... REG_SOUNDCNT_X:
     case REG_SOUNDBIAS:
     case REG_SOUNDBIAS + 1:
     case 0x04000090 ... 0x0400009F:
     case REG_FIFO_A ... REG_FIFO_B + 3:
          gba_apu_write_reg(gba, addr, val);
          break;

     /* Timers: write reload through 16-bit handler */
     case REG_TM0CNT_L:
     case REG_TM0CNT_L + 1:
     case REG_TM1CNT_L:
     case REG_TM1CNT_L + 1:
     case REG_TM2CNT_L:
     case REG_TM2CNT_L + 1:
     case REG_TM3CNT_L:
     case REG_TM3CNT_L + 1:
     {
          int n = (addr - REG_TM0CNT_L) / 4;
          gba_timer_sync(gba);
          if (addr & 1)
               gba->timer.ch[n].reload = (uint16_t)((gba->timer.ch[n].reload & 0x00FF) | (val << 8));
          else
               gba->timer.ch[n].reload = (uint16_t)((gba->timer.ch[n].reload & 0xFF00) | val);
          break;
     }
     case REG_TM0CNT_H:
          gba_timer_write_ctrl(gba, 0, val);
          break;
     case REG_TM1CNT_H:
          gba_timer_write_ctrl(gba, 1, val);
          break;
     case REG_TM2CNT_H:
          gba_timer_write_ctrl(gba, 2, val);
          break;
     case REG_TM3CNT_H:
          gba_timer_write_ctrl(gba, 3, val);
          break;

     /* DMA — handled via 32-bit writes but stub 8-bit too */
     case REG_DMA0CNT_H:
     case REG_DMA0CNT_H + 1:
     case REG_DMA1CNT_H:
     case REG_DMA1CNT_H + 1:
     case REG_DMA2CNT_H:
     case REG_DMA2CNT_H + 1:
     case REG_DMA3CNT_H:
     case REG_DMA3CNT_H + 1:
     {
          int n = (addr - REG_DMA0CNT_H) / 12;
          uint16_t cur = dma_ctrl_read16(gba, n);
          if (addr & 1)
               cur = (uint16_t)((cur & 0x00FF) | (val << 8));
          else
               cur = (uint16_t)((cur & 0xFF00) | val);
          gba_dma_write_ctrl(gba, n, cur);
          break;
     }

     /* Input */
     case REG_KEYCNT:
          gba->input.keycnt = (uint16_t)((gba->input.keycnt & 0xFF00) | val);
          break;
     case REG_KEYCNT + 1:
          gba->input.keycnt = (uint16_t)((gba->input.keycnt & 0x00FF) | (val << 8));
          break;

     /* IRQ */
     case REG_IE:
     {
          uint16_t new_ie = (uint16_t)((gba->irq.ie & 0xFF00) | val);
          if (!irq_note_ie_write(gba, new_ie))
               gba->irq.ie = new_ie;
          break;
     }
     case REG_IE + 1:
     {
          uint16_t new_ie = (uint16_t)((gba->irq.ie & 0x00FF) | (val << 8));
          if (!irq_note_ie_write(gba, new_ie))
               gba->irq.ie = new_ie;
          break;
     }
     case REG_IF:
          gba->irq.if_ &= (uint16_t)~val;
          break; /* write 1 to clear */
     case REG_IF + 1:
          gba->irq.if_ &= (uint16_t)~(val << 8);
          break;
     case REG_IME:
          gba->irq.ime = val & 1;
          break;

     case REG_WAITCNT:
          gba->waitcnt = (uint16_t)((gba->waitcnt & 0xFF00) | val);
          break;
     case REG_WAITCNT + 1:
          gba->waitcnt = (uint16_t)((gba->waitcnt & 0x00FF) | (val << 8));
          break;
     case REG_POSTFLG:
          gba->postflg |= val & 1;
          break;
     case REG_HALTCNT:
          if (val & 0x80)
          {
               gba->cpu.halted = true;
               gba->halt_mode = 2;
          }
          else if (gba->irq.ime)
          {
               gba->cpu.halted = true;
               gba->halt_mode = 1;
          }
          break;

     default:
          break;
     }
}

static void dma_write_src16(struct gba *gba, int n, uint32_t addr, uint16_t val)
{
     uint32_t cur = gba->dma.ch[n].src_latch;
     if (addr & 2)
          cur = (cur & 0x0000FFFFU) | ((uint32_t)val << 16);
     else
          cur = (cur & 0xFFFF0000U) | val;
     gba_dma_write_src(gba, n, cur);
}

static void dma_write_dst16(struct gba *gba, int n, uint32_t addr, uint16_t val)
{
     uint32_t cur = gba->dma.ch[n].dst_latch;
     if (addr & 2)
          cur = (cur & 0x0000FFFFU) | ((uint32_t)val << 16);
     else
          cur = (cur & 0xFFFF0000U) | val;
     gba_dma_write_dst(gba, n, cur);
}

static void io_write16(struct gba *gba, uint32_t addr, uint16_t val)
{
     addr &= ~1U;
     switch (addr)
     {
     case REG_DISPCNT ... REG_BLDY:
          gba_gpu_write16(gba, addr, val);
          break;
     case REG_SOUND1CNT_L ... REG_SOUNDCNT_X:
     case REG_SOUNDBIAS:
     case 0x04000090 ... 0x0400009E:
     case REG_FIFO_A:
     case REG_FIFO_A + 2:
     case REG_FIFO_B:
     case REG_FIFO_B + 2:
          gba_apu_write_reg(gba, addr, val & 0xFF);
          gba_apu_write_reg(gba, addr + 1, val >> 8);
          break;
     case REG_TM0CNT_L:
          gba_timer_write_reload(gba, 0, val);
          break;
     case REG_TM1CNT_L:
          gba_timer_write_reload(gba, 1, val);
          break;
     case REG_TM2CNT_L:
          gba_timer_write_reload(gba, 2, val);
          break;
     case REG_TM3CNT_L:
          gba_timer_write_reload(gba, 3, val);
          break;
     case REG_TM0CNT_H:
          gba_timer_write_ctrl(gba, 0, val);
          break;
     case REG_TM1CNT_H:
          gba_timer_write_ctrl(gba, 1, val);
          break;
     case REG_TM2CNT_H:
          gba_timer_write_ctrl(gba, 2, val);
          break;
     case REG_TM3CNT_H:
          gba_timer_write_ctrl(gba, 3, val);
          break;
     case REG_DMA0SAD:
     case REG_DMA0SAD + 2:
          dma_write_src16(gba, 0, addr, val);
          break;
     case REG_DMA1SAD:
     case REG_DMA1SAD + 2:
          dma_write_src16(gba, 1, addr, val);
          break;
     case REG_DMA2SAD:
     case REG_DMA2SAD + 2:
          dma_write_src16(gba, 2, addr, val);
          break;
     case REG_DMA3SAD:
     case REG_DMA3SAD + 2:
          dma_write_src16(gba, 3, addr, val);
          break;
     case REG_DMA0DAD:
     case REG_DMA0DAD + 2:
          dma_write_dst16(gba, 0, addr, val);
          break;
     case REG_DMA1DAD:
     case REG_DMA1DAD + 2:
          dma_write_dst16(gba, 1, addr, val);
          break;
     case REG_DMA2DAD:
     case REG_DMA2DAD + 2:
          dma_write_dst16(gba, 2, addr, val);
          break;
     case REG_DMA3DAD:
     case REG_DMA3DAD + 2:
          dma_write_dst16(gba, 3, addr, val);
          break;
     case REG_DMA0CNT_L:
          gba_dma_write_count(gba, 0, val);
          break;
     case REG_DMA1CNT_L:
          gba_dma_write_count(gba, 1, val);
          break;
     case REG_DMA2CNT_L:
          gba_dma_write_count(gba, 2, val);
          break;
     case REG_DMA3CNT_L:
          gba_dma_write_count(gba, 3, val);
          break;
     case REG_DMA0CNT_H:
          gba_dma_write_ctrl(gba, 0, val);
          break;
     case REG_DMA1CNT_H:
          gba_dma_write_ctrl(gba, 1, val);
          break;
     case REG_DMA2CNT_H:
          gba_dma_write_ctrl(gba, 2, val);
          break;
     case REG_DMA3CNT_H:
          gba_dma_write_ctrl(gba, 3, val);
          break;
     case REG_KEYCNT:
          gba->input.keycnt = val;
          break;
     case REG_IE:
          if (!irq_note_ie_write(gba, val))
               gba->irq.ie = val;
          break;
     case REG_IF:
          gba->irq.if_ &= (uint16_t)~val;
          break;
     case REG_WAITCNT:
          gba->waitcnt = val;
          break;
     case REG_IME:
          if ((val & 1) == 0 && gba->irq.ime && !(gba->cpu.cpsr & GBA_CPSR_I))
          {
               gba_timer_sync(gba);
               bool pending_now = (gba->irq.ie & gba->irq.if_) != 0;
               bool timer_edge_now = (gba->irq.ie & (1u << GBA_IRQ_TIMER0)) &&
                                     gba->timer.ch[0].enable &&
                                     !gba->timer.ch[0].cascade &&
                                     gba->timer.ch[0].irq_en &&
                                     gba->sync.next_event[GBA_SYNC_TIMER] - gba->timestamp <= 1;
               if (pending_now || timer_edge_now)
                    gba->irq.force = true;
          }
          gba->irq.ime = val & 1;
          break;
     case REG_POSTFLG:
          gba->postflg |= val & 1;
          gba->cpu.halted = true;
          gba->halt_mode = (val & 0x8000) ? 2 : 1;
          break;
     default:
          io_write8(gba, addr, val & 0xFF);
          io_write8(gba, addr + 1, val >> 8);
          break;
     }
}

/* -------------------------------------------------------------------------
 * WAITCNT wait-state helper
 *
 * Returns extra cycles (on top of the 1 base cycle) for ROM/SRAM accesses.
 * sequential: true when fetching consecutive halfwords (pipeline fetch).
 * ---------------------------------------------------------------------- */

static const int waitcnt_sram_table[4] = {4, 3, 2, 8};
static const int waitcnt_ws_first[4] = {4, 3, 2, 8};
static const int waitcnt_ws0_seq[2] = {2, 1};
static const int waitcnt_ws1_seq[2] = {4, 1};
static const int waitcnt_ws2_seq[2] = {8, 1};

static int memory_wait_cycles(struct gba *gba, uint32_t addr, bool sequential)
{
     uint16_t wc = gba->waitcnt;
     int region = addr >> 24;
     switch (region)
     {
     case 0x0E:
     case 0x0F: /* SRAM */
          return waitcnt_sram_table[wc & 0x3];
     case 0x08:
     case 0x09: /* ROM WS0 */
          return sequential ? waitcnt_ws0_seq[(wc >> 4) & 1]
                            : waitcnt_ws_first[(wc >> 2) & 0x3];
     case 0x0A:
     case 0x0B: /* ROM WS1 */
          return sequential ? waitcnt_ws1_seq[(wc >> 7) & 1]
                            : waitcnt_ws_first[(wc >> 5) & 0x3];
     case 0x0C:
     case 0x0D: /* ROM WS2 */
          return sequential ? waitcnt_ws2_seq[(wc >> 10) & 1]
                            : waitcnt_ws_first[(wc >> 8) & 0x3];
     default:
          return 0;
     }
}

static bool vram_map_addr(struct gba *gba, uint32_t addr, uint32_t *mapped)
{
     uint32_t off = addr & 0x1FFFFU;

     if (off >= GBA_VRAM_SIZE)
     {
          if (gba->gpu.bg_mode >= 3 && off < 0x1C000U)
               return false;
          off -= 0x8000U;
     }

     *mapped = off;
     return true;
}

/* -------------------------------------------------------------------------
 * Public read interface
 * ---------------------------------------------------------------------- */

uint8_t gba_memory_read8(struct gba *gba, uint32_t addr)
{
     switch (addr >> 24)
     {
     case 0x00:
          if (addr < GBA_BIOS_SIZE)
          {
               /* BIOS readable only when PC is in BIOS region */
               if (gba->bios && gba->cpu.r[GBA_PC] < GBA_BIOS_SIZE)
                    return gba->bios[addr];
               return (uint8_t)(gba->bios_open_bus >> ((addr & 3U) * 8));
          }
          return 0;
     case 0x01:
          return 0; /* unused */
     case 0x02:
          return gba->ewram[addr & (GBA_EWRAM_SIZE - 1)];
     case 0x03:
          return gba->iwram[addr & (GBA_IWRAM_SIZE - 1)];
     case 0x04:
          if (addr < GBA_IO_BASE + 0x400)
               return io_read8(gba, addr);
          return 0;
     case 0x05:
          return gba->pram[addr & (GBA_PAL_SIZE - 1)];
     case 0x06:
     {
          uint32_t off;
          if (!vram_map_addr(gba, addr, &off))
               return 0;
          return gba->vram[off];
     }
     case 0x07:
          return gba->oam[addr & (GBA_OAM_SIZE - 1)];
     case 0x08:
     case 0x09:
     case 0x0A:
     case 0x0B:
     case 0x0C:
     case 0x0D:
          return gba_cart_read8(gba, addr);
     case 0x0E:
     case 0x0F:
          return gba_cart_read8(gba, addr);
     default:
          return 0; /* open bus */
     }
}

uint16_t gba_memory_read16(struct gba *gba, uint32_t addr)
{
     if ((addr >> 24) >= 0x0E)
     {
          gba->mem_cycles += memory_wait_cycles(gba, addr, false);
          uint8_t v = gba_cart_read8(gba, addr);
          return (uint16_t)(v | ((uint16_t)v << 8));
     }
     /* ROM is a 16-bit bus: one halfword read is one bus access. */
     if ((addr >> 24) >= 0x08 && (addr >> 24) <= 0x0D)
     {
          gba->mem_cycles += memory_wait_cycles(gba, addr & ~1U, false);
          addr &= ~1U;
          return (uint16_t)(gba_cart_read8(gba, addr) |
                            ((uint16_t)gba_cart_read8(gba, addr + 1) << 8));
     }
     addr &= ~1U;
     if ((addr >> 24) == 0x04 && addr < GBA_IO_BASE + GBA_IO_SIZE)
          return io_read16(gba, addr);
     return (uint16_t)(gba_memory_read8(gba, addr) |
                       ((uint16_t)gba_memory_read8(gba, addr + 1) << 8));
}

uint32_t gba_memory_read32(struct gba *gba, uint32_t addr)
{
     uint32_t original_addr = addr;
     if (addr < GBA_BIOS_SIZE && !(gba->bios && gba->cpu.r[GBA_PC] < GBA_BIOS_SIZE))
     {
          uint32_t val = gba->bios_open_bus;
          if (gba->bios_open_bus_has_after_read)
          {
               gba->bios_open_bus = gba->bios_open_bus_after_read;
               gba->bios_open_bus_has_after_read = false;
          }
          return val;
     }
     if ((addr >> 24) >= 0x0E)
     {
          /* SRAM is 8-bit wide; 32-bit read costs 4× byte accesses */
          gba->mem_cycles += memory_wait_cycles(gba, addr, false) * 4;
          uint8_t v = gba_cart_read8(gba, addr);
          return (uint32_t)v | ((uint32_t)v << 8) |
                 ((uint32_t)v << 16) | ((uint32_t)v << 24);
     }
     addr &= ~3U;
     if ((addr >> 24) >= 0x08 && (addr >> 24) <= 0x0D)
     {
          gba->mem_cycles += memory_wait_cycles(gba, addr, false);
          gba->mem_cycles += memory_wait_cycles(gba, addr + 2, true);
          if ((addr & 0x1FFFFU) == 0)
               gba->mem_cycles += 2;
          uint32_t val = (uint32_t)gba_cart_read8(gba, addr) |
                         ((uint32_t)gba_cart_read8(gba, addr + 1) << 8) |
                         ((uint32_t)gba_cart_read8(gba, addr + 2) << 16) |
                         ((uint32_t)gba_cart_read8(gba, addr + 3) << 24);
          unsigned rot = (original_addr & 3U) * 8;
          return rot ? ((val >> rot) | (val << (32 - rot))) : val;
     }
     uint32_t val = (uint32_t)(gba_memory_read16(gba, addr) |
                               ((uint32_t)gba_memory_read16(gba, addr + 2) << 16));
     unsigned rot = (original_addr & 3U) * 8;
     return rot ? ((val >> rot) | (val << (32 - rot))) : val;
}

uint16_t gba_memory_fetch16(struct gba *gba, uint32_t addr, bool sequential)
{
     addr &= ~1U;
     if ((addr >> 24) >= 0x08 && (addr >> 24) <= 0x0D)
     {
          gba->mem_cycles += memory_wait_cycles(gba, addr, sequential);
          return (uint16_t)(gba_cart_read8(gba, addr) |
                            ((uint16_t)gba_cart_read8(gba, addr + 1) << 8));
     }
     return gba_memory_read16(gba, addr);
}

uint32_t gba_memory_fetch32(struct gba *gba, uint32_t addr, bool sequential)
{
     uint32_t original_addr = addr;
     addr &= ~3U;
     if ((addr >> 24) >= 0x08 && (addr >> 24) <= 0x0D)
     {
          gba->mem_cycles += memory_wait_cycles(gba, addr, sequential);
          gba->mem_cycles += memory_wait_cycles(gba, addr + 2, true);
          if (!sequential && (addr & 0x1FFFFU) == 0)
               gba->mem_cycles += 2;
          uint32_t val = (uint32_t)gba_cart_read8(gba, addr) |
                         ((uint32_t)gba_cart_read8(gba, addr + 1) << 8) |
                         ((uint32_t)gba_cart_read8(gba, addr + 2) << 16) |
                         ((uint32_t)gba_cart_read8(gba, addr + 3) << 24);
          unsigned rot = (original_addr & 3U) * 8;
          return rot ? ((val >> rot) | (val << (32 - rot))) : val;
     }
     return gba_memory_read32(gba, original_addr);
}

uint8_t gba_memory_peek8(struct gba *gba, uint32_t addr)
{
     /* Simplified: same as read8 but bypasses some protections */
     return gba_memory_read8(gba, addr);
}

uint16_t gba_memory_peek16(struct gba *gba, uint32_t addr)
{
     addr &= ~1U;
     return (uint16_t)(gba_memory_peek8(gba, addr) |
                       ((uint16_t)gba_memory_peek8(gba, addr + 1) << 8));
}

uint32_t gba_memory_peek32(struct gba *gba, uint32_t addr)
{
     addr &= ~3U;
     return (uint32_t)(gba_memory_peek16(gba, addr) |
                       ((uint32_t)gba_memory_peek16(gba, addr + 2) << 16));
}

/* -------------------------------------------------------------------------
 * Public write interface
 * ---------------------------------------------------------------------- */

void gba_memory_write8(struct gba *gba, uint32_t addr, uint8_t val)
{
     switch (addr >> 24)
     {
     case 0x02:
          gba->ewram[addr & (GBA_EWRAM_SIZE - 1)] = val;
          break;
     case 0x03:
          gba->iwram[addr & (GBA_IWRAM_SIZE - 1)] = val;
          break;
     case 0x04:
          if (addr < GBA_IO_BASE + 0x400)
               io_write8(gba, addr, val);
          break;
     case 0x05:
     {
          /* Palette: 8-bit writes replicate to both bytes of the 16-bit entry */
          uint32_t off = addr & (GBA_PAL_SIZE - 1) & ~1U;
          gba->pram[off] = val;
          gba->pram[off + 1] = val;
          break;
     }
     case 0x06:
     {
          /* VRAM: 8-bit writes to OBJ area are ignored; BG area replicates */
          uint32_t off;
          if (!vram_map_addr(gba, addr, &off))
               break;
          uint32_t bg_limit = (gba->gpu.bg_mode >= 3) ? 0x14000 : 0x10000;
          if (off < bg_limit)
          {
               uint32_t a = off & ~1U;
               gba->vram[a] = val;
               gba->vram[a + 1] = val;
          }
          /* OBJ area 8-bit writes silently dropped */
          break;
     }
     case 0x07:
     {
          /* OAM: 8-bit writes ignored */
          break;
     }
     case 0x08:
     case 0x09:
     case 0x0A:
     case 0x0B:
     case 0x0C:
     case 0x0D:
          gba_cart_write8(gba, addr, val);
          break;
     case 0x0E:
     case 0x0F:
          gba_cart_write8(gba, addr, val);
          break;
     default:
          break;
     }
}

void gba_memory_write16(struct gba *gba, uint32_t addr, uint16_t val)
{
     if ((addr >> 24) >= 0x0E)
     {
          gba_cart_write8(gba, addr, (uint8_t)(val >> ((addr & 1U) * 8)));
          return;
     }
     addr &= ~1U;
     switch (addr >> 24)
     {
     case 0x02:
     {
          uint32_t off = addr & (GBA_EWRAM_SIZE - 1);
          gba->ewram[off] = val & 0xFF;
          gba->ewram[off + 1] = val >> 8;
          break;
     }
     case 0x03:
     {
          uint32_t off = addr & (GBA_IWRAM_SIZE - 1);
          gba->iwram[off] = val & 0xFF;
          gba->iwram[off + 1] = val >> 8;
          break;
     }
     case 0x04:
          io_write16(gba, addr, val);
          break;
     case 0x05:
     {
          uint32_t off = addr & (GBA_PAL_SIZE - 1);
          gba->pram[off] = val & 0xFF;
          gba->pram[off + 1] = val >> 8;
          break;
     }
     case 0x06:
     {
          uint32_t off;
          if (!vram_map_addr(gba, addr, &off))
               break;
          gba->vram[off] = val & 0xFF;
          gba->vram[off + 1] = val >> 8;
          break;
     }
     case 0x07:
     {
          uint32_t off = addr & (GBA_OAM_SIZE - 1);
          gba->oam[off] = val & 0xFF;
          gba->oam[off + 1] = val >> 8;
          break;
     }
     case 0x08:
     case 0x09:
     case 0x0A:
     case 0x0B:
     case 0x0C:
     case 0x0D:
          gba_cart_write16(gba, addr, val);
          break;
     case 0x0E:
     case 0x0F:
          gba_cart_write16(gba, addr, val);
          break;
     default:
          gba_memory_write8(gba, addr, val & 0xFF);
          gba_memory_write8(gba, addr + 1, val >> 8);
          break;
     }
}

void gba_memory_write32(struct gba *gba, uint32_t addr, uint32_t val)
{
     if ((addr >> 24) >= 0x0E)
     {
          gba_cart_write8(gba, addr, (uint8_t)(val >> ((addr & 3U) * 8)));
          return;
     }
     addr &= ~3U;
     switch (addr >> 24)
     {
     case 0x04:
          /* Handle 32-bit I/O writes (DMA, affine BG ref) */
          switch (addr)
          {
          case REG_DMA0SAD:
               gba_dma_write_src(gba, 0, val);
               break;
          case REG_DMA0DAD:
               gba_dma_write_dst(gba, 0, val);
               break;
          case REG_DMA1SAD:
               gba_dma_write_src(gba, 1, val);
               break;
          case REG_DMA1DAD:
               gba_dma_write_dst(gba, 1, val);
               break;
          case REG_DMA2SAD:
               gba_dma_write_src(gba, 2, val);
               break;
          case REG_DMA2DAD:
               gba_dma_write_dst(gba, 2, val);
               break;
          case REG_DMA3SAD:
               gba_dma_write_src(gba, 3, val);
               break;
          case REG_DMA3DAD:
               gba_dma_write_dst(gba, 3, val);
               break;
          case REG_DMA0CNT_L:
               gba_dma_write_count(gba, 0, val & 0xFFFF);
               gba_dma_write_ctrl(gba, 0, val >> 16);
               break;
          case REG_DMA1CNT_L:
               gba_dma_write_count(gba, 1, val & 0xFFFF);
               gba_dma_write_ctrl(gba, 1, val >> 16);
               break;
          case REG_DMA2CNT_L:
               gba_dma_write_count(gba, 2, val & 0xFFFF);
               gba_dma_write_ctrl(gba, 2, val >> 16);
               break;
          case REG_DMA3CNT_L:
               gba_dma_write_count(gba, 3, val & 0xFFFF);
               gba_dma_write_ctrl(gba, 3, val >> 16);
               break;
          case REG_BG2X:
          case REG_BG2Y:
          case REG_BG3X:
          case REG_BG3Y:
               gba_gpu_write32(gba, addr, val);
               break;
          case REG_FIFO_A:
               gba_apu_fifo_push(gba, 0, val);
               break;
          case REG_FIFO_B:
               gba_apu_fifo_push(gba, 1, val);
               break;
          default:
               gba_memory_write16(gba, addr, val & 0xFFFF);
               gba_memory_write16(gba, addr + 2, val >> 16);
               break;
          }
          break;
     default:
          gba_memory_write16(gba, addr, val & 0xFFFF);
          gba_memory_write16(gba, addr + 2, val >> 16);
          break;
     }
}
