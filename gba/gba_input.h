#ifndef _GBA_INPUT_H_
#define _GBA_INPUT_H_

#include <stdint.h>
#include <stdbool.h>

struct gba;

/* KEYINPUT bit positions (0 = pressed) */
#define GBA_KEY_A 0
#define GBA_KEY_B 1
#define GBA_KEY_SELECT 2
#define GBA_KEY_START 3
#define GBA_KEY_RIGHT 4
#define GBA_KEY_LEFT 5
#define GBA_KEY_UP 6
#define GBA_KEY_DOWN 7
#define GBA_KEY_R 8
#define GBA_KEY_L 9

struct gba_input
{
     /* KEYINPUT: bits 0-9, active-low (0 = pressed) */
     uint16_t keyinput;
     /* KEYCNT:  IRQ enable + condition */
     uint16_t keycnt;
};

void gba_input_reset(struct gba *gba);
void gba_input_set(struct gba *gba, unsigned key, bool pressed);
uint16_t gba_input_get(struct gba *gba);

#endif /* _GBA_INPUT_H_ */
