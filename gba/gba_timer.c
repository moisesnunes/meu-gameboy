#include <string.h>
#include "gba.h"

static void timer_overflow(struct gba *gba, int n);
static void timer_commit_reload(struct gba_timer_channel *ch);
static void timer_advance(struct gba *gba, int n, int32_t elapsed);

void gba_timer_reset(struct gba *gba)
{
     memset(&gba->timer, 0, sizeof(gba->timer));
}

void gba_timer_write_reload(struct gba *gba, int n, uint16_t val)
{
     struct gba_timer_channel *ch = &gba->timer.ch[n];
     gba_timer_sync(gba);
     if (ch->enable && !ch->cascade)
     {
          ch->reload_pending = val;
          ch->pending_reload = true;
          ch->reload_delay = 1;
     }
     else
     {
          ch->reload = val;
          ch->pending_reload = false;
     }
}

void gba_timer_write_ctrl(struct gba *gba, int n, uint16_t val)
{
     struct gba_timer_channel *ch = &gba->timer.ch[n];
     bool was_enabled = ch->enable;
     bool new_enable = (val >> 7) & 1;

     gba_timer_sync(gba);

     if (was_enabled && !new_enable && !ch->cascade)
     {
          int prescaler = gba_timer_prescaler[ch->prescaler];
          ch->cycles_acc += 1;
          while (ch->cycles_acc >= prescaler)
          {
               ch->cycles_acc -= prescaler;
               ch->counter++;
               if (ch->counter == 0)
                    timer_overflow(gba, n);
          }
     }

     ch->prescaler = val & 0x3;
     ch->cascade = (val >> 2) & 1;
     ch->irq_en = (val >> 6) & 1;
     ch->enable = new_enable;

     /* Reload counter on enable rising edge */
     if (!was_enabled && ch->enable)
     {
          if (ch->pending_reload)
               timer_commit_reload(ch);
          ch->counter = ch->reload;
          /*
           * I/O writes run before the CPU step advances timestamp.  Delay the
           * first timer tick until the enabling store itself has completed.
           */
          ch->cycles_acc = -2;
     }

     gba_sync_next(gba, GBA_SYNC_TIMER, 1);
}

uint16_t gba_timer_read_counter(struct gba *gba, int n)
{
     gba_timer_sync(gba);
     return gba->timer.ch[n].counter;
}

static void timer_commit_reload(struct gba_timer_channel *ch)
{
     ch->reload = ch->reload_pending;
     ch->pending_reload = false;
     ch->reload_delay = 0;
}

static void timer_overflow(struct gba *gba, int n)
{
     struct gba_timer_channel *ch = &gba->timer.ch[n];

     ch->counter = ch->reload;

     if (ch->irq_en)
          gba_irq_trigger(gba, (enum gba_irq_token)(GBA_IRQ_TIMER0 + n));

     /* APU FIFO: notify on TM0/TM1 overflow */
     if (n == 0 || n == 1)
          gba_apu_fifo_tick(gba, n);

     /* Cascade: increment next timer */
     if (n + 1 < GBA_TIMER_COUNT && gba->timer.ch[n + 1].enable &&
         gba->timer.ch[n + 1].cascade)
     {
          gba->timer.ch[n + 1].counter++;
          if (gba->timer.ch[n + 1].counter == 0)
               timer_overflow(gba, n + 1);
     }
}

void gba_timer_sync(struct gba *gba)
{
     int32_t elapsed = gba_sync_resync(gba, GBA_SYNC_TIMER);
     int n;
     int32_t min_next = GBA_SYNC_NEVER;

     for (n = 0; n < GBA_TIMER_COUNT; n++)
     {
          struct gba_timer_channel *ch = &gba->timer.ch[n];
          if (!ch->enable || ch->cascade)
               continue;

          int prescaler = gba_timer_prescaler[ch->prescaler];
          int32_t remaining_elapsed = elapsed;

          while (ch->pending_reload && remaining_elapsed >= ch->reload_delay)
          {
               int32_t before_commit = ch->reload_delay;
               timer_advance(gba, n, before_commit);
               remaining_elapsed -= before_commit;
               timer_commit_reload(ch);
          }

          if (ch->pending_reload)
               ch->reload_delay -= remaining_elapsed;

          timer_advance(gba, n, remaining_elapsed);

          int32_t remaining = prescaler - ch->cycles_acc;
          /* cycles until this timer ticks once more, relative to elapsed start */
          int32_t next = remaining + (int32_t)(0xFFFF - ch->counter) * prescaler;
          if (next < min_next)
               min_next = next;
     }

     gba_sync_next(gba, GBA_SYNC_TIMER, min_next);
}

static void timer_advance(struct gba *gba, int n, int32_t elapsed)
{
     struct gba_timer_channel *ch = &gba->timer.ch[n];
     int prescaler = gba_timer_prescaler[ch->prescaler];

     ch->cycles_acc += elapsed;
     while (ch->cycles_acc >= prescaler)
     {
          ch->cycles_acc -= prescaler;
          ch->counter++;
          if (ch->counter == 0)
               timer_overflow(gba, n);
     }
}
