#ifndef _GB_MEMORY_H_
#define _GB_MEMORY_H_

#include <stdint.h>

uint8_t gb_memory_readb(struct gb *gb, uint16_t addr);
uint8_t gb_memory_peekb(struct gb *gb, uint16_t addr);
void gb_memory_writeb(struct gb *gb, uint16_t addr, uint8_t val);
void gb_memory_trigger_oam_bug(struct gb *gb, uint16_t addr);
void gb_serial_sync(struct gb *gb);

#endif /* _GB_MEMORY_H_ */
