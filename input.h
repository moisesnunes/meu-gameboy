#ifndef _GB_INPUT_H_
#define _GB_INPUT_H_

#define GB_INPUT_RIGHT 0
#define GB_INPUT_LEFT 1
#define GB_INPUT_UP 2
#define GB_INPUT_DOWN 3
#define GB_INPUT_A 4
#define GB_INPUT_B 5
#define GB_INPUT_SELECT 6
#define GB_INPUT_START 7

struct gb_input
{
    /* Estado do D-pad (direita, esquerda, cima, embaixo) */
    uint8_t dpad_state;

    /* Verdadeiro se D-pad selecionado */
    bool dpad_selected;

    /* Estado dos butões */
    uint8_t buttons_state;

    /*  Verdadeiro se butões selecionados  */
    bool buttons_selected;
};

void gb_input_reset(struct gb *gb);
void gb_input_set(struct gb *gb, unsigned buttom, bool pressed);
void gb_input_select(struct gb *gb, uint8_t selection);
uint8_t gb_input_get_state(struct gb *gb);

#endif
