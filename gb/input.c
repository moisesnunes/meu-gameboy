#include "gb.h"

void gb_input_reset(struct gb *gb)
{
     struct gb_input *input = &gb->input;

     input->dpad_state = ~0x10;
     input->buttons_state = ~0x20;
     input->dpad_selected = gb->hw_model != GB_HW_CGB0 &&
                            gb->hw_model != GB_HW_CGB &&
                            gb->hw_model != GB_HW_SGB &&
                            gb->hw_model != GB_HW_SGB2;
     input->buttons_selected = input->dpad_selected;
}

void gb_input_set(struct gb *gb, unsigned button, bool pressed)
{
     struct gb_input *input = &gb->input;
     uint8_t *state;
     uint8_t prev_state;
     unsigned bit;

     prev_state = gb_input_get_state(gb);

     if (button <= GB_INPUT_DOWN)
     {
          state = &input->dpad_state;
          bit = button;
     }
     else
     {
          state = &input->buttons_state;
          bit = button - 4;
     }

     /* Lógica active-low: bit = 0 quando pressionado, 1 quando solto */
     if (pressed)
     {
          *state &= ~(1U << bit);
     }
     else
     {
          *state |= 1U << bit;
     }
     gb_debug_hw_trace_joypad(gb, gb_input_get_state(gb), pressed);

     if (pressed && prev_state != gb_input_get_state(gb))
     {
          /* Borda descendente em um terminal selecionado: dispara IRQ de joypad
           * e acorda a CPU caso esteja em estado STOP. */
          gb_irq_trigger(gb, GB_IRQ_INPUT);
     }
}

void gb_input_select(struct gb *gb, uint8_t selection)
{
     struct gb_input *input = &gb->input;

     input->dpad_selected = ((selection & 0x10) == 0);
     input->buttons_selected = ((selection & 0x20) == 0);
}

uint8_t gb_input_get_state(struct gb *gb)
{
     struct gb_input *input = &gb->input;
     uint8_t v = 0xff;

     if (input->dpad_selected)
     {
          v &= input->dpad_state;
     }

     if (input->buttons_selected)
     {
          v &= input->buttons_state;
     }

     return v;
}
