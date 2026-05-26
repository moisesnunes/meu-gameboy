#include <stdio.h>
#include <string.h>
#include "gb.h"
#include "disasm.h"

/* Tipo do operando para cálculo do tamanho da instrução */
typedef enum
{
     OP_NONE, /* sem operando        — 1 byte total */
     OP_N8,   /* imediato 8 bits     — 2 bytes total */
     OP_N16,  /* imediato 16 bits    — 3 bytes total */
     OP_R8,   /* offset relativo     — 2 bytes total (igual N8, sinalizado) */
} op_type;

typedef struct
{
     const char *fmt; /* string de formato: $n8, $n16, $r8 como placeholders */
     op_type op;
} instr_t;

/* Nomes dos 8 registradores usados nos opcodes de 8 bits */
static const char *reg8[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};

/* Nomes dos 4 pares de registradores de 16 bits (tabela rp) */
// static const char *reg16[4] = { "BC", "DE", "HL", "SP" };

/* Condições de branch */
// static const char *cond[4] = { "NZ", "Z", "NC", "C" };

/*
 * Tabela principal de opcodes (0x00–0xFF).
 * Opcodes marcados como NULL são inválidos/não implementados.
 * Usamos $n8, $n16, $r8 como placeholders que serão substituídos.
 */
static const instr_t main_table[256] = {
    /* 0x00 */ {"NOP", OP_NONE},
    /* 0x01 */ {"LD BC,$n16", OP_N16},
    /* 0x02 */ {"LD (BC),A", OP_NONE},
    /* 0x03 */ {"INC BC", OP_NONE},
    /* 0x04 */ {"INC B", OP_NONE},
    /* 0x05 */ {"DEC B", OP_NONE},
    /* 0x06 */ {"LD B,$n8", OP_N8},
    /* 0x07 */ {"RLCA", OP_NONE},
    /* 0x08 */ {"LD ($n16),SP", OP_N16},
    /* 0x09 */ {"ADD HL,BC", OP_NONE},
    /* 0x0A */ {"LD A,(BC)", OP_NONE},
    /* 0x0B */ {"DEC BC", OP_NONE},
    /* 0x0C */ {"INC C", OP_NONE},
    /* 0x0D */ {"DEC C", OP_NONE},
    /* 0x0E */ {"LD C,$n8", OP_N8},
    /* 0x0F */ {"RRCA", OP_NONE},
    /* 0x10 */ {"STOP", OP_N8},
    /* 0x11 */ {"LD DE,$n16", OP_N16},
    /* 0x12 */ {"LD (DE),A", OP_NONE},
    /* 0x13 */ {"INC DE", OP_NONE},
    /* 0x14 */ {"INC D", OP_NONE},
    /* 0x15 */ {"DEC D", OP_NONE},
    /* 0x16 */ {"LD D,$n8", OP_N8},
    /* 0x17 */ {"RLA", OP_NONE},
    /* 0x18 */ {"JR $r8", OP_R8},
    /* 0x19 */ {"ADD HL,DE", OP_NONE},
    /* 0x1A */ {"LD A,(DE)", OP_NONE},
    /* 0x1B */ {"DEC DE", OP_NONE},
    /* 0x1C */ {"INC E", OP_NONE},
    /* 0x1D */ {"DEC E", OP_NONE},
    /* 0x1E */ {"LD E,$n8", OP_N8},
    /* 0x1F */ {"RRA", OP_NONE},
    /* 0x20 */ {"JR NZ,$r8", OP_R8},
    /* 0x21 */ {"LD HL,$n16", OP_N16},
    /* 0x22 */ {"LD (HL+),A", OP_NONE},
    /* 0x23 */ {"INC HL", OP_NONE},
    /* 0x24 */ {"INC H", OP_NONE},
    /* 0x25 */ {"DEC H", OP_NONE},
    /* 0x26 */ {"LD H,$n8", OP_N8},
    /* 0x27 */ {"DAA", OP_NONE},
    /* 0x28 */ {"JR Z,$r8", OP_R8},
    /* 0x29 */ {"ADD HL,HL", OP_NONE},
    /* 0x2A */ {"LD A,(HL+)", OP_NONE},
    /* 0x2B */ {"DEC HL", OP_NONE},
    /* 0x2C */ {"INC L", OP_NONE},
    /* 0x2D */ {"DEC L", OP_NONE},
    /* 0x2E */ {"LD L,$n8", OP_N8},
    /* 0x2F */ {"CPL", OP_NONE},
    /* 0x30 */ {"JR NC,$r8", OP_R8},
    /* 0x31 */ {"LD SP,$n16", OP_N16},
    /* 0x32 */ {"LD (HL-),A", OP_NONE},
    /* 0x33 */ {"INC SP", OP_NONE},
    /* 0x34 */ {"INC (HL)", OP_NONE},
    /* 0x35 */ {"DEC (HL)", OP_NONE},
    /* 0x36 */ {"LD (HL),$n8", OP_N8},
    /* 0x37 */ {"SCF", OP_NONE},
    /* 0x38 */ {"JR C,$r8", OP_R8},
    /* 0x39 */ {"ADD HL,SP", OP_NONE},
    /* 0x3A */ {"LD A,(HL-)", OP_NONE},
    /* 0x3B */ {"DEC SP", OP_NONE},
    /* 0x3C */ {"INC A", OP_NONE},
    /* 0x3D */ {"DEC A", OP_NONE},
    /* 0x3E */ {"LD A,$n8", OP_N8},
    /* 0x3F */ {"CCF", OP_NONE},
    /* 0x40–0x7F: LD r,r e HALT — gerados dinamicamente (ver abaixo) */
    /* 0x40 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x44 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x48 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x4C */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x50 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x54 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x58 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x5C */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x60 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x64 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x68 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x6C */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x70 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x74 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x78 */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x7C */ {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    {NULL, OP_NONE},
    /* 0x80 */ {"ADD A,B", OP_NONE},
    {"ADD A,C", OP_NONE},
    /* 0x82 */ {"ADD A,D", OP_NONE},
    {"ADD A,E", OP_NONE},
    /* 0x84 */ {"ADD A,H", OP_NONE},
    {"ADD A,L", OP_NONE},
    /* 0x86 */ {"ADD A,(HL)", OP_NONE},
    {"ADD A,A", OP_NONE},
    /* 0x88 */ {"ADC A,B", OP_NONE},
    {"ADC A,C", OP_NONE},
    /* 0x8A */ {"ADC A,D", OP_NONE},
    {"ADC A,E", OP_NONE},
    /* 0x8C */ {"ADC A,H", OP_NONE},
    {"ADC A,L", OP_NONE},
    /* 0x8E */ {"ADC A,(HL)", OP_NONE},
    {"ADC A,A", OP_NONE},
    /* 0x90 */ {"SUB B", OP_NONE},
    {"SUB C", OP_NONE},
    /* 0x92 */ {"SUB D", OP_NONE},
    {"SUB E", OP_NONE},
    /* 0x94 */ {"SUB H", OP_NONE},
    {"SUB L", OP_NONE},
    /* 0x96 */ {"SUB (HL)", OP_NONE},
    {"SUB A", OP_NONE},
    /* 0x98 */ {"SBC A,B", OP_NONE},
    {"SBC A,C", OP_NONE},
    /* 0x9A */ {"SBC A,D", OP_NONE},
    {"SBC A,E", OP_NONE},
    /* 0x9C */ {"SBC A,H", OP_NONE},
    {"SBC A,L", OP_NONE},
    /* 0x9E */ {"SBC A,(HL)", OP_NONE},
    {"SBC A,A", OP_NONE},
    /* 0xA0 */ {"AND B", OP_NONE},
    {"AND C", OP_NONE},
    /* 0xA2 */ {"AND D", OP_NONE},
    {"AND E", OP_NONE},
    /* 0xA4 */ {"AND H", OP_NONE},
    {"AND L", OP_NONE},
    /* 0xA6 */ {"AND (HL)", OP_NONE},
    {"AND A", OP_NONE},
    /* 0xA8 */ {"XOR B", OP_NONE},
    {"XOR C", OP_NONE},
    /* 0xAA */ {"XOR D", OP_NONE},
    {"XOR E", OP_NONE},
    /* 0xAC */ {"XOR H", OP_NONE},
    {"XOR L", OP_NONE},
    /* 0xAE */ {"XOR (HL)", OP_NONE},
    {"XOR A", OP_NONE},
    /* 0xB0 */ {"OR B", OP_NONE},
    {"OR C", OP_NONE},
    /* 0xB2 */ {"OR D", OP_NONE},
    {"OR E", OP_NONE},
    /* 0xB4 */ {"OR H", OP_NONE},
    {"OR L", OP_NONE},
    /* 0xB6 */ {"OR (HL)", OP_NONE},
    {"OR A", OP_NONE},
    /* 0xB8 */ {"CP B", OP_NONE},
    {"CP C", OP_NONE},
    /* 0xBA */ {"CP D", OP_NONE},
    {"CP E", OP_NONE},
    /* 0xBC */ {"CP H", OP_NONE},
    {"CP L", OP_NONE},
    /* 0xBE */ {"CP (HL)", OP_NONE},
    {"CP A", OP_NONE},
    /* 0xC0 */ {"RET NZ", OP_NONE},
    /* 0xC1 */ {"POP BC", OP_NONE},
    /* 0xC2 */ {"JP NZ,$n16", OP_N16},
    /* 0xC3 */ {"JP $n16", OP_N16},
    /* 0xC4 */ {"CALL NZ,$n16", OP_N16},
    /* 0xC5 */ {"PUSH BC", OP_NONE},
    /* 0xC6 */ {"ADD A,$n8", OP_N8},
    /* 0xC7 */ {"RST 00H", OP_NONE},
    /* 0xC8 */ {"RET Z", OP_NONE},
    /* 0xC9 */ {"RET", OP_NONE},
    /* 0xCA */ {"JP Z,$n16", OP_N16},
    /* 0xCB */ {"PREFIX CB", OP_N8},
    /* 0xCC */ {"CALL Z,$n16", OP_N16},
    /* 0xCD */ {"CALL $n16", OP_N16},
    /* 0xCE */ {"ADC A,$n8", OP_N8},
    /* 0xCF */ {"RST 08H", OP_NONE},
    /* 0xD0 */ {"RET NC", OP_NONE},
    /* 0xD1 */ {"POP DE", OP_NONE},
    /* 0xD2 */ {"JP NC,$n16", OP_N16},
    /* 0xD3 */ {"INVALID", OP_NONE},
    /* 0xD4 */ {"CALL NC,$n16", OP_N16},
    /* 0xD5 */ {"PUSH DE", OP_NONE},
    /* 0xD6 */ {"SUB $n8", OP_N8},
    /* 0xD7 */ {"RST 10H", OP_NONE},
    /* 0xD8 */ {"RET C", OP_NONE},
    /* 0xD9 */ {"RETI", OP_NONE},
    /* 0xDA */ {"JP C,$n16", OP_N16},
    /* 0xDB */ {"INVALID", OP_NONE},
    /* 0xDC */ {"CALL C,$n16", OP_N16},
    /* 0xDD */ {"INVALID", OP_NONE},
    /* 0xDE */ {"SBC A,$n8", OP_N8},
    /* 0xDF */ {"RST 18H", OP_NONE},
    /* 0xE0 */ {"LDH ($FF00+$n8),A", OP_N8},
    /* 0xE1 */ {"POP HL", OP_NONE},
    /* 0xE2 */ {"LD ($FF00+C),A", OP_NONE},
    /* 0xE3 */ {"INVALID", OP_NONE},
    /* 0xE4 */ {"INVALID", OP_NONE},
    /* 0xE5 */ {"PUSH HL", OP_NONE},
    /* 0xE6 */ {"AND $n8", OP_N8},
    /* 0xE7 */ {"RST 20H", OP_NONE},
    /* 0xE8 */ {"ADD SP,$r8", OP_R8},
    /* 0xE9 */ {"JP (HL)", OP_NONE},
    /* 0xEA */ {"LD ($n16),A", OP_N16},
    /* 0xEB */ {"INVALID", OP_NONE},
    /* 0xEC */ {"INVALID", OP_NONE},
    /* 0xED */ {"INVALID", OP_NONE},
    /* 0xEE */ {"XOR $n8", OP_N8},
    /* 0xEF */ {"RST 28H", OP_NONE},
    /* 0xF0 */ {"LDH A,($FF00+$n8)", OP_N8},
    /* 0xF1 */ {"POP AF", OP_NONE},
    /* 0xF2 */ {"LD A,($FF00+C)", OP_NONE},
    /* 0xF3 */ {"DI", OP_NONE},
    /* 0xF4 */ {"INVALID", OP_NONE},
    /* 0xF5 */ {"PUSH AF", OP_NONE},
    /* 0xF6 */ {"OR $n8", OP_N8},
    /* 0xF7 */ {"RST 30H", OP_NONE},
    /* 0xF8 */ {"LD HL,SP+$r8", OP_R8},
    /* 0xF9 */ {"LD SP,HL", OP_NONE},
    /* 0xFA */ {"LD A,($n16)", OP_N16},
    /* 0xFB */ {"EI", OP_NONE},
    /* 0xFC */ {"INVALID", OP_NONE},
    /* 0xFD */ {"INVALID", OP_NONE},
    /* 0xFE */ {"CP $n8", OP_N8},
    /* 0xFF */ {"RST 38H", OP_NONE},
};

/*
 * Mnemônicos do grupo CB (prefixo 0xCB).
 * Os opcodes CB são altamente regulares:
 *   0x00–0x07  RLC r    (r = opcode & 7)
 *   0x08–0x0F  RRC r
 *   0x10–0x17  RL  r
 *   0x18–0x1F  RR  r
 *   0x20–0x27  SLA r
 *   0x28–0x2F  SRA r
 *   0x30–0x37  SWAP r
 *   0x38–0x3F  SRL r
 *   0x40–0x7F  BIT b,r  (b = (opcode >> 3) & 7)
 *   0x80–0xBF  RES b,r
 *   0xC0–0xFF  SET b,r
 */
static const char *cb_prefix[8] = {
    "RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"};

static bool is_relative_branch(uint8_t op)
{
     return op == 0x18 || op == 0x20 || op == 0x28 ||
            op == 0x30 || op == 0x38;
}

static bool is_absolute_jump(uint8_t op)
{
     return op == 0xC2 || op == 0xC3 || op == 0xCA ||
            op == 0xD2 || op == 0xDA;
}

static bool is_absolute_call(uint8_t op)
{
     return op == 0xC4 || op == 0xCC || op == 0xCD ||
            op == 0xD4 || op == 0xDC;
}

static bool rst_target(uint8_t op, uint16_t *target)
{
     if ((op & 0xC7) != 0xC7)
          return false;
     *target = (uint16_t)(op & 0x38);
     return true;
}

static void split_text(struct gb_disasm_instr *info)
{
     const char *sp = strchr(info->text, ' ');
     if (sp)
     {
          size_t n = (size_t)(sp - info->text);
          if (n >= sizeof(info->mnemonic))
               n = sizeof(info->mnemonic) - 1;
          memcpy(info->mnemonic, info->text, n);
          info->mnemonic[n] = '\0';
          snprintf(info->operands, sizeof(info->operands), "%s", sp + 1);
     }
     else
     {
          size_t n = strlen(info->text);
          if (n >= sizeof(info->mnemonic))
               n = sizeof(info->mnemonic) - 1;
          memcpy(info->mnemonic, info->text, n);
          info->mnemonic[n] = '\0';
          info->operands[0] = '\0';
     }
}

static void replace_token(char *dst, size_t dst_size, const char *fmt,
                          const char *token, const char *replacement)
{
     const char *p = fmt;
     char *q = dst;
     size_t token_len = strlen(token);

     while (*p && (size_t)(q - dst) + 1 < dst_size)
     {
          if (strncmp(p, token, token_len) == 0)
          {
               int wrote = snprintf(q, dst_size - (size_t)(q - dst), "%s", replacement);
               if (wrote < 0)
                    break;
               if ((size_t)wrote >= dst_size - (size_t)(q - dst))
               {
                    q = dst + dst_size - 1;
                    break;
               }
               q += wrote;
               p += token_len;
          }
          else
          {
               *q++ = *p++;
          }
     }
     *q = '\0';
}

int gb_disasm_ex(struct gb *gb, uint16_t addr, struct gb_disasm_instr *info)
{
     uint8_t op;
     uint8_t n8;
     uint16_t n16;

     memset(info, 0, sizeof(*info));
     info->addr = addr;
     info->bytes[0] = gb_memory_peekb(gb, addr);
     info->bytes[1] = gb_memory_peekb(gb, (uint16_t)(addr + 1));
     info->bytes[2] = gb_memory_peekb(gb, (uint16_t)(addr + 2));

     op = info->bytes[0];
     n8 = info->bytes[1];
     n16 = n8 | ((uint16_t)info->bytes[2] << 8);

     /* Bloco 0x40–0x7F: LD r,r e HALT */
     if (op >= 0x40 && op <= 0x7F)
     {
          int dst = (op >> 3) & 7;
          int src = op & 7;

          if (op == 0x76)
          {
               snprintf(info->text, sizeof(info->text), "HALT");
          }
          else
          {
               snprintf(info->text, sizeof(info->text), "LD %s,%s", reg8[dst], reg8[src]);
          }
          info->len = 1;
          split_text(info);
          return info->len;
     }

     /* Prefixo CB: instrução de 2 bytes */
     if (op == 0xCB)
     {
          int grp = (n8 >> 6) & 3;
          int bit = (n8 >> 3) & 7;
          int reg = n8 & 7;

          if (grp == 0)
          {
               snprintf(info->text, sizeof(info->text), "%s %s", cb_prefix[bit], reg8[reg]);
          }
          else if (grp == 1)
          {
               snprintf(info->text, sizeof(info->text), "BIT %d,%s", bit, reg8[reg]);
          }
          else if (grp == 2)
          {
               snprintf(info->text, sizeof(info->text), "RES %d,%s", bit, reg8[reg]);
          }
          else
          {
               snprintf(info->text, sizeof(info->text), "SET %d,%s", bit, reg8[reg]);
          }
          info->len = 2;
          split_text(info);
          return info->len;
     }

     /* Tabela principal */
     const instr_t *instr = &main_table[op];
     const char *fmt = instr->fmt;

     if (fmt == NULL)
     {
          snprintf(info->text, sizeof(info->text), "??? (%02X)", op);
          info->len = 1;
          split_text(info);
          return info->len;
     }

     switch (instr->op)
     {
     case OP_NONE:
          snprintf(info->text, sizeof(info->text), "%s", fmt);
          info->len = 1;
          if (rst_target(op, &info->target))
          {
               info->has_target = true;
               info->target_type = GB_DISASM_TARGET_RST;
          }
          split_text(info);
          return info->len;

     case OP_N8:
          {
               char replacement[16];
               snprintf(replacement, sizeof(replacement), "0x%02X", n8);
               replace_token(info->text, sizeof(info->text), fmt, "$n8", replacement);
          }
          info->len = 2;
          split_text(info);
          return info->len;

     case OP_N16:
     {
          char replacement[16];
          snprintf(replacement, sizeof(replacement), "0x%04X", n16);
          replace_token(info->text, sizeof(info->text), fmt, "$n16", replacement);
          info->len = 3;
          if (is_absolute_jump(op) || is_absolute_call(op))
          {
               info->has_target = true;
               info->target = n16;
               info->target_type = is_absolute_call(op) ? GB_DISASM_TARGET_CALL
                                                        : GB_DISASM_TARGET_BRANCH;
          }
          split_text(info);
     }
          return info->len;

     case OP_R8:
          {
               int8_t offset = (int8_t)n8;
               char replacement[32];

               if (is_relative_branch(op))
               {
                    info->has_target = true;
                    info->target = (uint16_t)(addr + 2 + offset);
                    info->target_type = GB_DISASM_TARGET_BRANCH;
                    snprintf(replacement, sizeof(replacement), "0x%04X (%+d)",
                             info->target, offset);
               }
               else
               {
                    snprintf(replacement, sizeof(replacement), "%+d", offset);
               }

               replace_token(info->text, sizeof(info->text), fmt, "$r8", replacement);
          }
          info->len = 2;
          split_text(info);
          return info->len;
     }

     snprintf(info->text, sizeof(info->text), "???");
     info->len = 1;
     split_text(info);
     return info->len;
}

int gb_disasm_len(struct gb *gb, uint16_t addr)
{
     struct gb_disasm_instr info;
     return gb_disasm_ex(gb, addr, &info);
}

int gb_disasm(struct gb *gb, uint16_t addr, char *out, int out_size)
{
     struct gb_disasm_instr info;
     int len = gb_disasm_ex(gb, addr, &info);
     snprintf(out, out_size, "%s", info.text);
     return len;
}
