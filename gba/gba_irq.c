#include "gba.h"

void gba_irq_reset(struct gba *gba)
{
     struct gba_irq *irq = &gba->irq;
     irq->ie = 0;
     irq->if_ = 0;
     irq->ime = false;
     irq->force = false;
}

void gba_irq_trigger(struct gba *gba, enum gba_irq_token which)
{
     uint16_t bit = (uint16_t)(1U << which);
     gba->irq.if_ |= bit;
     if (gba->bios_intr_wait_active && (gba->bios_intr_wait_mask & bit))
     {
          gba->bios_intr_wait_active = false;
          gba->bios_intr_wait_mask = 0;
          gba->cpu.halted = false;
          gba->halt_mode = 0;
          if (gba->halt_resume_cycles > 0)
          {
               gba->timestamp += gba->halt_resume_cycles;
               gba->halt_resume_cycles = 0;
          }
     }
     /* Wake CPU from halt if this IRQ is enabled */
     if (gba->irq.ie & bit)
     {
          gba->cpu.halted = false;
          gba->halt_mode = 0;
          if (gba->halt_resume_cycles > 0)
          {
               gba->timestamp += gba->halt_resume_cycles;
               gba->halt_resume_cycles = 0;
          }
     }
}

bool gba_irq_pending(struct gba *gba)
{
     return gba->irq.force || (gba->irq.ime && (gba->irq.ie & gba->irq.if_));
}
