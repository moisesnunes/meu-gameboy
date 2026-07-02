/*
 * gba_memory.c — GBA memory bus: read/write/fetch for all regions.
 *
 * Handles BIOS protection, open-bus, wait-state accounting (WAITCNT),
 * I/O register dispatch, DMA/IRQ/timer writes, and the ROM prefetch buffer.
 */

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

static uint16_t dma_ctrl_read16(struct gba *gba, int n);
static uint16_t gba_memory_read16_source(struct gba *gba, uint32_t addr,
                                         enum gba_memory_access_source source);

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

/*
 * I/O register reads
 */

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
     case REG_DMA0CNT_H:
          return dma_ctrl_read16(gba, 0) & 0xFF;
     case REG_DMA0CNT_H + 1:
          return dma_ctrl_read16(gba, 0) >> 8;
     case REG_DMA1CNT_H:
          return dma_ctrl_read16(gba, 1) & 0xFF;
     case REG_DMA1CNT_H + 1:
          return dma_ctrl_read16(gba, 1) >> 8;
     case REG_DMA2CNT_H:
          return dma_ctrl_read16(gba, 2) & 0xFF;
     case REG_DMA2CNT_H + 1:
          return dma_ctrl_read16(gba, 2) >> 8;
     case REG_DMA3CNT_H:
          return dma_ctrl_read16(gba, 3) & 0xFF;
     case REG_DMA3CNT_H + 1:
          return dma_ctrl_read16(gba, 3) >> 8;

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

static uint16_t io_read16_source(struct gba *gba, uint32_t addr,
                                 enum gba_memory_access_source source)
{
     addr &= ~1U;
     switch (addr)
     {
     case REG_DISPCNT ... REG_BLDY:
          return gba_gpu_read16_sampled(gba, addr,
                                        source == GBA_MEMORY_ACCESS_DMA
                                            ? GBA_GPU_SAMPLE_DMA
                                            : GBA_GPU_SAMPLE_CPU);
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

/*
 * I/O register writes
 */

static void dma_write_src8(struct gba *gba, int n, uint32_t addr, uint8_t val);
static void dma_write_dst8(struct gba *gba, int n, uint32_t addr, uint8_t val);
static void dma_write_count8(struct gba *gba, int n, uint32_t addr, uint8_t val);

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

     /* Timers: preserve the deferred reload latch for byte writes. */
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
          gba_timer_write_reload8(gba, n, (addr & 1U) != 0, val);
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

     /* DMA */
     case REG_DMA0SAD ... REG_DMA0SAD + 3:
          dma_write_src8(gba, 0, addr, val);
          break;
     case REG_DMA1SAD ... REG_DMA1SAD + 3:
          dma_write_src8(gba, 1, addr, val);
          break;
     case REG_DMA2SAD ... REG_DMA2SAD + 3:
          dma_write_src8(gba, 2, addr, val);
          break;
     case REG_DMA3SAD ... REG_DMA3SAD + 3:
          dma_write_src8(gba, 3, addr, val);
          break;
     case REG_DMA0DAD ... REG_DMA0DAD + 3:
          dma_write_dst8(gba, 0, addr, val);
          break;
     case REG_DMA1DAD ... REG_DMA1DAD + 3:
          dma_write_dst8(gba, 1, addr, val);
          break;
     case REG_DMA2DAD ... REG_DMA2DAD + 3:
          dma_write_dst8(gba, 2, addr, val);
          break;
     case REG_DMA3DAD ... REG_DMA3DAD + 3:
          dma_write_dst8(gba, 3, addr, val);
          break;
     case REG_DMA0CNT_L:
     case REG_DMA0CNT_L + 1:
          dma_write_count8(gba, 0, addr, val);
          break;
     case REG_DMA1CNT_L:
     case REG_DMA1CNT_L + 1:
          dma_write_count8(gba, 1, addr, val);
          break;
     case REG_DMA2CNT_L:
     case REG_DMA2CNT_L + 1:
          dma_write_count8(gba, 2, addr, val);
          break;
     case REG_DMA3CNT_L:
     case REG_DMA3CNT_L + 1:
          dma_write_count8(gba, 3, addr, val);
          break;
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
          /* If enabling IME with pending IE&IF, arm the delay */
          if (gba->irq.ime && (gba->irq.ie & gba->irq.if_) &&
              gba->irq.irq_delay == 0)
               gba->irq.irq_delay = 7; /* GBA_IRQ_DELAY */
          break;

     case REG_WAITCNT:
          gba->waitcnt = (uint16_t)((gba->waitcnt & 0xFF00) | val);
          gba->prefetch_en = (gba->waitcnt >> 14) & 1;
          break;
     case REG_WAITCNT + 1:
          gba->waitcnt = (uint16_t)((gba->waitcnt & 0x00FF) | (val << 8));
          gba->prefetch_en = (gba->waitcnt >> 14) & 1;
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

static void dma_write_src8(struct gba *gba, int n, uint32_t addr, uint8_t val)
{
     uint32_t cur = gba->dma.ch[n].src_latch;
     unsigned shift = (addr & 3U) * 8U;
     cur = (cur & ~(0xFFU << shift)) | ((uint32_t)val << shift);
     gba_dma_write_src(gba, n, cur);
}

static void dma_write_dst8(struct gba *gba, int n, uint32_t addr, uint8_t val)
{
     uint32_t cur = gba->dma.ch[n].dst_latch;
     unsigned shift = (addr & 3U) * 8U;
     cur = (cur & ~(0xFFU << shift)) | ((uint32_t)val << shift);
     gba_dma_write_dst(gba, n, cur);
}

static void dma_write_count8(struct gba *gba, int n, uint32_t addr, uint8_t val)
{
     uint16_t cur = gba->dma.ch[n].count_latch;
     if (addr & 1U)
          cur = (uint16_t)((cur & 0x00FFU) | ((uint16_t)val << 8));
     else
          cur = (uint16_t)((cur & 0xFF00U) | val);
     gba_dma_write_count(gba, n, cur);
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
          if (gba->irq.ime && (gba->irq.ie & gba->irq.if_) &&
              gba->irq.irq_delay == 0)
               gba->irq.irq_delay = 7; /* GBA_IRQ_DELAY */
          break;
     case REG_IF:
          gba->irq.if_ &= (uint16_t)~val;
          break;
     case REG_WAITCNT:
          gba->waitcnt = val;
          gba->prefetch_en = (val >> 14) & 1;
          break;
     case REG_IME:
          if ((val & 1) == 0 && gba->irq.ime && !(gba->cpu.cpsr & GBA_CPSR_I))
          {
               gba_timer_sync(gba);
               bool pending_now = (gba->irq.ie & gba->irq.if_) != 0;
               int32_t timer_delta = gba->sync.next_event[GBA_SYNC_TIMER] - gba->timestamp;
               bool timer_edge_now = (gba->irq.ie & (1u << GBA_IRQ_TIMER0)) &&
                                     gba->timer.ch[0].enable &&
                                     !gba->timer.ch[0].cascade &&
                                     gba->timer.ch[0].irq_en &&
                                     timer_delta <= 1;
               if (timer_edge_now)
                    gba->irq.if_ |= (uint16_t)(1u << GBA_IRQ_TIMER0);
               if (pending_now || timer_edge_now)
                    gba->irq.force = true;
          }
          gba->irq.ime = val & 1;
          if (gba->irq.ime && (gba->irq.ie & gba->irq.if_) &&
              gba->irq.irq_delay == 0)
               gba->irq.irq_delay = 7; /* GBA_IRQ_DELAY */
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

/*
 * WAITCNT wait-state helper
 *
 * Returns extra cycles (on top of the 1 base cycle) for ROM/SRAM accesses.
 * sequential: true when fetching consecutive halfwords (pipeline fetch).
 */

static const int waitcnt_sram_table[4] = {4, 3, 2, 8};
static const int waitcnt_ws_first[4] = {4, 3, 2, 8};
static const int waitcnt_ws0_seq[2] = {2, 1};
static const int waitcnt_ws1_seq[2] = {4, 1};
static const int waitcnt_ws2_seq[2] = {8, 1};

static const struct gba_memory_block memory_blocks[] = {
     [GBA_MEMORY_REGION_BIOS] = {"BIOS", GBA_BIOS_BASE, GBA_BIOS_SIZE, 0x00003FFFU},
     [GBA_MEMORY_REGION_UNUSED] = {"unused", 0, 0, 0},
     [GBA_MEMORY_REGION_EWRAM] = {"EWRAM", GBA_EWRAM_BASE, GBA_EWRAM_SIZE, 0x0003FFFFU},
     [GBA_MEMORY_REGION_IWRAM] = {"IWRAM", GBA_IWRAM_BASE, GBA_IWRAM_SIZE, 0x00007FFFU},
     [GBA_MEMORY_REGION_IO] = {"I/O", GBA_IO_BASE, GBA_IO_SIZE, 0x000003FFU},
     [GBA_MEMORY_REGION_PRAM] = {"palette", GBA_PAL_BASE, GBA_PAL_SIZE, 0x000003FFU},
     [GBA_MEMORY_REGION_VRAM] = {"VRAM", GBA_VRAM_BASE, GBA_VRAM_SIZE, 0x0001FFFFU},
     [GBA_MEMORY_REGION_OAM] = {"OAM", GBA_OAM_BASE, GBA_OAM_SIZE, 0x000003FFU},
     [GBA_MEMORY_REGION_ROM0] = {"ROM WS0", 0x08000000U, GBA_ROM_MAX_SIZE, 0x01FFFFFFU},
     [GBA_MEMORY_REGION_ROM1] = {"ROM WS1", 0x0A000000U, GBA_ROM_MAX_SIZE, 0x01FFFFFFU},
     [GBA_MEMORY_REGION_ROM2] = {"ROM WS2", 0x0C000000U, GBA_ROM_MAX_SIZE, 0x01FFFFFFU},
     [GBA_MEMORY_REGION_SRAM] = {"SRAM", GBA_SRAM_BASE, GBA_SRAM_SIZE, 0x00007FFFU},
};

enum gba_memory_region gba_memory_region_for_addr(uint32_t addr)
{
     switch (addr >> 24)
     {
     case 0x00: return addr < GBA_BIOS_SIZE ? GBA_MEMORY_REGION_BIOS : GBA_MEMORY_REGION_UNUSED;
     case 0x02: return GBA_MEMORY_REGION_EWRAM;
     case 0x03: return GBA_MEMORY_REGION_IWRAM;
     case 0x04: return addr < GBA_IO_BASE + GBA_IO_SIZE ? GBA_MEMORY_REGION_IO : GBA_MEMORY_REGION_UNUSED;
     case 0x05: return GBA_MEMORY_REGION_PRAM;
     case 0x06: return GBA_MEMORY_REGION_VRAM;
     case 0x07: return GBA_MEMORY_REGION_OAM;
     case 0x08:
     case 0x09: return GBA_MEMORY_REGION_ROM0;
     case 0x0A:
     case 0x0B: return GBA_MEMORY_REGION_ROM1;
     case 0x0C:
     case 0x0D: return GBA_MEMORY_REGION_ROM2;
     case 0x0E:
     case 0x0F: return GBA_MEMORY_REGION_SRAM;
     default: return GBA_MEMORY_REGION_UNUSED;
     }
}

const struct gba_memory_block *gba_memory_block_for_addr(uint32_t addr)
{
     return &memory_blocks[gba_memory_region_for_addr(addr)];
}

static uint32_t memory_open_bus(const struct gba *gba)
{
     return gba->cpu_bus;
}

static uint8_t memory_open_bus8(const struct gba *gba, uint32_t addr)
{
     return (uint8_t)(memory_open_bus(gba) >> ((addr & 3U) * 8));
}

int gba_memory_wait_cycles(const struct gba *gba,
                           const struct gba_memory_access *access)
{
     uint16_t wc = gba->waitcnt;
     switch (gba_memory_region_for_addr(access->addr))
     {
     case GBA_MEMORY_REGION_EWRAM:
          return access->width == GBA_MEMORY_ACCESS_32 ? 5 : 2;
     case GBA_MEMORY_REGION_SRAM:
          return waitcnt_sram_table[wc & 0x3];
     case GBA_MEMORY_REGION_ROM0:
          return access->sequential ? waitcnt_ws0_seq[(wc >> 4) & 1]
                                    : waitcnt_ws_first[(wc >> 2) & 0x3];
     case GBA_MEMORY_REGION_ROM1:
          return access->sequential ? waitcnt_ws1_seq[(wc >> 7) & 1]
                                    : waitcnt_ws_first[(wc >> 5) & 0x3];
     case GBA_MEMORY_REGION_ROM2:
          return access->sequential ? waitcnt_ws2_seq[(wc >> 10) & 1]
                                    : waitcnt_ws_first[(wc >> 8) & 0x3];
     default:
          return 0;
     }
}

static int cpu_wait_cycles(const struct gba *gba, uint32_t addr,
                           enum gba_memory_access_width width,
                           enum gba_memory_access_kind kind, bool sequential)
{
     const struct gba_memory_access access = {
          .addr = addr,
          .width = width,
          .source = GBA_MEMORY_ACCESS_CPU,
          .kind = kind,
          .sequential = sequential,
     };

     return gba_memory_wait_cycles(gba, &access);
}

/* Map a VRAM address to its physical offset, handling the 96KB→128KB mirror.
 * Addresses in 0x6000000–0x6017FFF mirror based on BG mode:
 * modes 3-5 reserve 0x10000–0x17FFF for BMP frame buffer (not remapped). */
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

/*
 * Public read interface
 */

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
          return (uint8_t)(gba->cpu_bus >> ((addr & 3U) * 8));
     case 0x01:
          return memory_open_bus8(gba, addr); /* unused region — open bus */
     case 0x02:
          return gba->ewram[addr & (GBA_EWRAM_SIZE - 1)];
     case 0x03:
          return gba->iwram[addr & (GBA_IWRAM_SIZE - 1)];
     case 0x04:
          if (addr < GBA_IO_BASE + 0x400)
               return io_read8(gba, addr);
          return memory_open_bus8(gba, addr); /* I/O gap — open bus */
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
          return memory_open_bus8(gba, addr); /* open bus */
     }
}

uint16_t gba_memory_read16(struct gba *gba, uint32_t addr)
{
     return gba_memory_read16_source(gba, addr, GBA_MEMORY_ACCESS_CPU);
}

uint16_t gba_memory_read16_dma(struct gba *gba, uint32_t addr)
{
     return gba_memory_read16_source(gba, addr, GBA_MEMORY_ACCESS_DMA);
}

static uint16_t gba_memory_read16_source(struct gba *gba, uint32_t addr,
                                         enum gba_memory_access_source source)
{
     if ((addr >> 24) >= 0x0E)
     {
          gba->mem_cycles += cpu_wait_cycles(gba, addr, GBA_MEMORY_ACCESS_16,
                                              GBA_MEMORY_ACCESS_READ, false);
          uint8_t v = gba_cart_read8(gba, addr);
          return (uint16_t)(v | ((uint16_t)v << 8));
     }
     /* ROM is a 16-bit bus: one halfword read is one bus access. */
     if ((addr >> 24) >= 0x08 && (addr >> 24) <= 0x0D)
     {
          if ((addr >> 24) == 0x0D && gba_cart_is_eeprom(gba))
               return gba_cart_eeprom_read(gba);
          gba->mem_cycles += cpu_wait_cycles(gba, addr & ~1U,
                                              GBA_MEMORY_ACCESS_16,
                                              GBA_MEMORY_ACCESS_READ, false);
          addr &= ~1U;
          return (uint16_t)(gba_cart_read8(gba, addr) |
                            ((uint16_t)gba_cart_read8(gba, addr + 1) << 8));
     }
     addr &= ~1U;
     if ((addr >> 24) == 0x04 && addr < GBA_IO_BASE + GBA_IO_SIZE)
          return io_read16_source(gba, addr, source);
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
          gba->mem_cycles += cpu_wait_cycles(gba, addr, GBA_MEMORY_ACCESS_8,
                                              GBA_MEMORY_ACCESS_READ, false) * 4;
          uint8_t v = gba_cart_read8(gba, addr);
          return (uint32_t)v | ((uint32_t)v << 8) |
                 ((uint32_t)v << 16) | ((uint32_t)v << 24);
     }
     addr &= ~3U;
     if ((addr >> 24) >= 0x08 && (addr >> 24) <= 0x0D)
     {
          gba->mem_cycles += cpu_wait_cycles(gba, addr, GBA_MEMORY_ACCESS_16,
                                              GBA_MEMORY_ACCESS_READ, false);
          gba->mem_cycles += cpu_wait_cycles(gba, addr + 2,
                                              GBA_MEMORY_ACCESS_16,
                                              GBA_MEMORY_ACCESS_READ, true);
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
     uint16_t val;
     if ((addr >> 24) >= 0x08 && (addr >> 24) <= 0x0D)
     {
          /* Prefetch buffer: when enabled, a sequential fetch that follows
           * directly from the previous fetch costs 1 cycle (buffer hit)
           * instead of the normal sequential wait states. */
          bool prefetch_hit = gba->prefetch_en && sequential &&
                              addr == gba->prefetch_last_addr + 2;
          if (prefetch_hit)
               gba->mem_cycles += 1;
          else
               gba->mem_cycles += cpu_wait_cycles(gba, addr,
                                                   GBA_MEMORY_ACCESS_16,
                                                   GBA_MEMORY_ACCESS_FETCH,
                                                   sequential);
          gba->prefetch_last_addr = addr;
          val = (uint16_t)(gba_cart_read8(gba, addr) |
                           ((uint16_t)gba_cart_read8(gba, addr + 1) << 8));
     }
     else
     {
          val = gba_memory_read16(gba, addr);
     }
     /* Update CPU bus with the fetched halfword, replicated to 32 bits */
     gba->cpu_bus = (uint32_t)val | ((uint32_t)val << 16);
     return val;
}

uint32_t gba_memory_fetch32(struct gba *gba, uint32_t addr, bool sequential)
{
     uint32_t original_addr = addr;
     addr &= ~3U;
     uint32_t val;
     if ((addr >> 24) >= 0x08 && (addr >> 24) <= 0x0D)
     {
          bool prefetch_hit = gba->prefetch_en && sequential &&
                              addr == gba->prefetch_last_addr + 2;
          gba->mem_cycles += prefetch_hit ? 1 :
              cpu_wait_cycles(gba, addr, GBA_MEMORY_ACCESS_16,
                              GBA_MEMORY_ACCESS_FETCH, sequential);
          /* Second halfword of a 32-bit fetch is always sequential */
          bool prefetch_hit2 = gba->prefetch_en &&
                               (addr + 2) == gba->prefetch_last_addr + 2;
          gba->mem_cycles += prefetch_hit2 ? 1 :
              cpu_wait_cycles(gba, addr + 2, GBA_MEMORY_ACCESS_16,
                              GBA_MEMORY_ACCESS_FETCH, true);
          if (!sequential && (addr & 0x1FFFFU) == 0)
               gba->mem_cycles += 2;
          gba->prefetch_last_addr = addr + 2;
          val = (uint32_t)gba_cart_read8(gba, addr) |
                ((uint32_t)gba_cart_read8(gba, addr + 1) << 8) |
                ((uint32_t)gba_cart_read8(gba, addr + 2) << 16) |
                ((uint32_t)gba_cart_read8(gba, addr + 3) << 24);
          unsigned rot = (original_addr & 3U) * 8;
          val = rot ? ((val >> rot) | (val << (32 - rot))) : val;
     }
     else
     {
          val = gba_memory_read32(gba, original_addr);
     }
     gba->cpu_bus = val;
     return val;
}

uint8_t gba_memory_peek8(struct gba *gba, uint32_t addr)
{
     switch (addr >> 24)
     {
     case 0x00:
          if (addr < GBA_BIOS_SIZE)
          {
               if (gba->bios)
                    return gba->bios[addr];
               return (uint8_t)(gba->bios_open_bus >> ((addr & 3U) * 8));
          }
          return (uint8_t)(gba->cpu_bus >> ((addr & 3U) * 8));
     case 0x02:
          return gba->ewram[addr & (GBA_EWRAM_SIZE - 1)];
     case 0x03:
          return gba->iwram[addr & (GBA_IWRAM_SIZE - 1)];
     case 0x04:
          if (addr < GBA_IO_BASE + GBA_IO_SIZE)
          {
               uint32_t aligned = addr & ~1U;
               uint16_t v = 0;
               switch (aligned)
               {
               case REG_DISPCNT ... REG_BLDY:
                    v = gba_gpu_read16(gba, aligned);
                    break;
               case REG_TM0CNT_L:
                    v = gba->timer.ch[0].counter;
                    break;
               case REG_TM1CNT_L:
                    v = gba->timer.ch[1].counter;
                    break;
               case REG_TM2CNT_L:
                    v = gba->timer.ch[2].counter;
                    break;
               case REG_TM3CNT_L:
                    v = gba->timer.ch[3].counter;
                    break;
               case REG_TM0CNT_H:
                    v = (uint16_t)(gba->timer.ch[0].prescaler |
                                   (gba->timer.ch[0].irq_en << 6) |
                                   (gba->timer.ch[0].enable << 7));
                    break;
               case REG_TM1CNT_H:
                    v = (uint16_t)(gba->timer.ch[1].prescaler |
                                   (gba->timer.ch[1].cascade << 2) |
                                   (gba->timer.ch[1].irq_en << 6) |
                                   (gba->timer.ch[1].enable << 7));
                    break;
               case REG_TM2CNT_H:
                    v = (uint16_t)(gba->timer.ch[2].prescaler |
                                   (gba->timer.ch[2].cascade << 2) |
                                   (gba->timer.ch[2].irq_en << 6) |
                                   (gba->timer.ch[2].enable << 7));
                    break;
               case REG_TM3CNT_H:
                    v = (uint16_t)(gba->timer.ch[3].prescaler |
                                   (gba->timer.ch[3].cascade << 2) |
                                   (gba->timer.ch[3].irq_en << 6) |
                                   (gba->timer.ch[3].enable << 7));
                    break;
               case REG_DMA0CNT_H:
                    v = dma_ctrl_read16(gba, 0);
                    break;
               case REG_DMA1CNT_H:
                    v = dma_ctrl_read16(gba, 1);
                    break;
               case REG_DMA2CNT_H:
                    v = dma_ctrl_read16(gba, 2);
                    break;
               case REG_DMA3CNT_H:
                    v = dma_ctrl_read16(gba, 3);
                    break;
               case REG_KEYINPUT:
                    v = gba->input.keyinput;
                    break;
               case REG_KEYCNT:
                    v = gba->input.keycnt;
                    break;
               case REG_IE:
                    v = gba->irq.ie;
                    break;
               case REG_IF:
                    v = gba->irq.if_;
                    break;
               case REG_WAITCNT:
                    v = gba->waitcnt;
                    break;
               case REG_IME:
                    v = gba->irq.ime ? 1 : 0;
                    break;
               case REG_POSTFLG:
                    v = gba->postflg;
                    break;
               default:
                    v = 0;
                    break;
               }
               return (uint8_t)(v >> ((addr & 1U) * 8));
          }
          return (uint8_t)(gba->cpu_bus >> ((addr & 3U) * 8));
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
          return gba_cart_read8(gba, addr);
     case 0x0D:
          if (gba_cart_is_eeprom(gba))
               return (uint8_t)gba_cart_eeprom_peek(gba);
          return gba_cart_read8(gba, addr);
     case 0x0E:
     case 0x0F:
          return gba_cart_read8(gba, addr);
     default:
          return (uint8_t)(gba->cpu_bus >> ((addr & 3U) * 8));
     }
}

uint16_t gba_memory_peek16(struct gba *gba, uint32_t addr)
{
     addr &= ~1U;
     if ((addr >> 24) == 0x0D && gba_cart_is_eeprom(gba))
          return gba_cart_eeprom_peek(gba);
     return (uint16_t)(gba_memory_peek8(gba, addr) |
                       ((uint16_t)gba_memory_peek8(gba, addr + 1) << 8));
}

uint32_t gba_memory_peek32(struct gba *gba, uint32_t addr)
{
     addr &= ~3U;
     return (uint32_t)(gba_memory_peek16(gba, addr) |
                       ((uint32_t)gba_memory_peek16(gba, addr + 2) << 16));
}

/*
 * Public write interface
 */

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
          if ((addr >> 24) == 0x0D && gba_cart_is_eeprom(gba))
               gba_cart_eeprom_write(gba, val, 1);
          else
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
     /* EEPROM is a 16-bit serial device.  A 32-bit store is not two EEPROM
      * commands and must not advance its serial state. */
     if ((addr >> 24) == 0x0D && gba_cart_is_eeprom(gba))
          return;
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
          case REG_TM0CNT_L:
               gba_timer_write_reload(gba, 0, val & 0xFFFF);
               gba_timer_write_ctrl_delayed(gba, 0, val >> 16,
                                            (val & 0x00800000U) ? 2 : 0);
               break;
          case REG_TM1CNT_L:
               gba_timer_write_reload(gba, 1, val & 0xFFFF);
               gba_timer_write_ctrl_delayed(gba, 1, val >> 16,
                                            (val & 0x00800000U) ? 2 : 0);
               break;
          case REG_TM2CNT_L:
               gba_timer_write_reload(gba, 2, val & 0xFFFF);
               gba_timer_write_ctrl_delayed(gba, 2, val >> 16,
                                            (val & 0x00800000U) ? 2 : 0);
               break;
          case REG_TM3CNT_L:
               gba_timer_write_reload(gba, 3, val & 0xFFFF);
               gba_timer_write_ctrl_delayed(gba, 3, val >> 16,
                                            (val & 0x00800000U) ? 2 : 0);
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
