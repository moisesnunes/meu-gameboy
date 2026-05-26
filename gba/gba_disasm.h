#ifndef _GBA_DISASM_H_
#define _GBA_DISASM_H_

#include <stddef.h>
#include <stdint.h>

struct gba;

unsigned gba_disasm_len(struct gba *gba, uint32_t addr, int thumb);
void gba_disasm(struct gba *gba, uint32_t addr, int thumb, char *out, size_t out_len);

#endif /* _GBA_DISASM_H_ */
