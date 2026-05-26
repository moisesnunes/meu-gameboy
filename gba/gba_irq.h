#ifndef _GBA_IRQ_H_
#define _GBA_IRQ_H_

#include <stdint.h>
#include <stdbool.h>

struct gba;

/* GBA interrupt sources (IF/IE bits) */
enum gba_irq_token
{
     GBA_IRQ_VBLANK = 0, /* 0x04000004 bit 3 triggers */
     GBA_IRQ_HBLANK = 1,
     GBA_IRQ_VCOUNT = 2,
     GBA_IRQ_TIMER0 = 3,
     GBA_IRQ_TIMER1 = 4,
     GBA_IRQ_TIMER2 = 5,
     GBA_IRQ_TIMER3 = 6,
     GBA_IRQ_SERIAL = 7,
     GBA_IRQ_DMA0 = 8,
     GBA_IRQ_DMA1 = 9,
     GBA_IRQ_DMA2 = 10,
     GBA_IRQ_DMA3 = 11,
     GBA_IRQ_KEYPAD = 12,
     GBA_IRQ_GAMEPAK = 13,
     GBA_IRQ_NUM = 14,
};

struct gba_irq
{
     uint16_t ie;  /* 0x04000200: Interrupt Enable */
     uint16_t if_; /* 0x04000202: Interrupt Request Flags */
     bool ime;     /* 0x04000208: Interrupt Master Enable */
     bool force;   /* IRQ edge already accepted by CPU pipeline */
};

void gba_irq_reset(struct gba *gba);
void gba_irq_trigger(struct gba *gba, enum gba_irq_token which);
bool gba_irq_pending(struct gba *gba);

#endif /* _GBA_IRQ_H_ */
