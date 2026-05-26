#include "gba.h"

void gba_input_reset(struct gba *gba)
{
     /* All buttons released: active-low, so all bits = 1 */
     gba->input.keyinput = 0x03FF;
     gba->input.keycnt = 0;
}

void gba_input_set(struct gba *gba, unsigned key, bool pressed)
{
     if (key >= 10)
          return;
     if (pressed)
          gba->input.keyinput &= (uint16_t)~(1U << key);
     else
          gba->input.keyinput |= (uint16_t)(1U << key);

     /* KEYCNT IRQ: trigger if condition met */
     if (gba->input.keycnt & (1U << 14))
     {
          uint16_t mask = gba->input.keycnt & 0x03FF;
          bool and_mode = (gba->input.keycnt >> 15) & 1;
          uint16_t pressed_bits = (~gba->input.keyinput) & 0x03FF;
          bool fire = and_mode
                          ? ((pressed_bits & mask) == mask)
                          : ((pressed_bits & mask) != 0);
          if (fire)
               gba_irq_trigger(gba, GBA_IRQ_KEYPAD);
     }
}

uint16_t gba_input_get(struct gba *gba)
{
     return gba->input.keyinput;
}
