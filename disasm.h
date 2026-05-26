#ifndef _GB_DISASM_H_
#define _GB_DISASM_H_

#include <stdint.h>
#include <stdbool.h>

struct gb;

typedef enum {
     GB_DISASM_TARGET_NONE,
     GB_DISASM_TARGET_BRANCH,
     GB_DISASM_TARGET_CALL,
     GB_DISASM_TARGET_RST,
} gb_disasm_target_type;

struct gb_disasm_instr {
     uint16_t addr;
     uint8_t bytes[3];
     int len;
     char text[64];
     char mnemonic[16];
     char operands[48];
     bool has_target;
     uint16_t target;
     gb_disasm_target_type target_type;
};

/*
 * gb_disasm — decodifica a instrução em `addr` e escreve o mnemônico em `out`.
 * Retorna o tamanho da instrução em bytes (1, 2 ou 3).
 * `out` deve ter pelo menos 32 bytes.
 */
int gb_disasm(struct gb *gb, uint16_t addr, char *out, int out_size);
int gb_disasm_ex(struct gb *gb, uint16_t addr, struct gb_disasm_instr *out);
int gb_disasm_len(struct gb *gb, uint16_t addr);

#endif /* _GB_DISASM_H_ */
