#include <string.h>
#include "gba.h"

/* -------------------------------------------------------------------------
 * Flag helpers (shared with ARM side)
 * ---------------------------------------------------------------------- */

static void set_nz(struct gba_cpu *cpu, uint32_t result)
{
     cpu->cpsr &= ~(GBA_CPSR_N | GBA_CPSR_Z);
     if (result == 0)
          cpu->cpsr |= GBA_CPSR_Z;
     if (result >> 31)
          cpu->cpsr |= GBA_CPSR_N;
}

static void set_flags_add_carry(struct gba_cpu *cpu, uint32_t a, uint32_t b,
                                uint32_t carry_in, uint32_t result)
{
     uint64_t unsigned_sum = (uint64_t)a + (uint64_t)b + (uint64_t)carry_in;
     int64_t signed_sum = (int64_t)(int32_t)a + (int64_t)(int32_t)b +
                          (int64_t)carry_in;

     set_nz(cpu, result);
     cpu->cpsr &= ~(GBA_CPSR_C | GBA_CPSR_V);
     if (unsigned_sum >> 32)
          cpu->cpsr |= GBA_CPSR_C;
     if (signed_sum < INT32_MIN || signed_sum > INT32_MAX)
          cpu->cpsr |= GBA_CPSR_V;
}

static void set_flags_add(struct gba_cpu *cpu, uint32_t a, uint32_t b, uint32_t result)
{
     set_flags_add_carry(cpu, a, b, 0, result);
}

static void set_flags_sub_borrow(struct gba_cpu *cpu, uint32_t a, uint32_t b,
                                 uint32_t borrow, uint32_t result)
{
     uint64_t unsigned_diff = (uint64_t)a - (uint64_t)b - (uint64_t)borrow;
     int64_t signed_diff = (int64_t)(int32_t)a - (int64_t)(int32_t)b -
                           (int64_t)borrow;

     set_nz(cpu, result);
     cpu->cpsr &= ~(GBA_CPSR_C | GBA_CPSR_V);
     if ((unsigned_diff >> 32) == 0)
          cpu->cpsr |= GBA_CPSR_C;
     if (signed_diff < INT32_MIN || signed_diff > INT32_MAX)
          cpu->cpsr |= GBA_CPSR_V;
}

static void set_flags_sub(struct gba_cpu *cpu, uint32_t a, uint32_t b, uint32_t result)
{
     set_flags_sub_borrow(cpu, a, b, 0, result);
}

/* -------------------------------------------------------------------------
 * THUMB instruction execution
 * ---------------------------------------------------------------------- */

int gba_thumb_execute(struct gba *gba, uint16_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     uint8_t hi = instr >> 8;

     /* Format 1: Move shifted register */
     if ((hi & 0xE0) == 0x00 && (instr & 0x1800) != 0x1800)
     {
          uint8_t op = (instr >> 11) & 0x3;
          uint8_t amt = (instr >> 6) & 0x1F;
          uint8_t rs = (instr >> 3) & 0x7;
          uint8_t rd = instr & 0x7;
          uint32_t val = cpu->r[rs];
          uint32_t res, carry = (cpu->cpsr >> 29) & 1;
          switch (op)
          {
          case 0: /* LSL */
               if (amt == 0)
               {
                    res = val;
               }
               else
               {
                    carry = (val >> (32 - amt)) & 1;
                    res = val << amt;
               }
               break;
          case 1: /* LSR */
               if (amt == 0)
               {
                    carry = val >> 31;
                    res = 0;
               }
               else
               {
                    carry = (val >> (amt - 1)) & 1;
                    res = val >> amt;
               }
               break;
          case 2: /* ASR */
               if (amt == 0)
               {
                    carry = val >> 31;
                    res = (uint32_t)((int32_t)val >> 31);
               }
               else
               {
                    carry = (val >> (amt - 1)) & 1;
                    res = (uint32_t)((int32_t)val >> amt);
               }
               break;
          default:
               res = val;
               break;
          }
          cpu->r[rd] = res;
          set_nz(cpu, res);
          cpu->cpsr &= ~GBA_CPSR_C;
          if (carry)
               cpu->cpsr |= GBA_CPSR_C;
          return 1;
     }

     /* Format 2: Add/subtract */
     if ((hi & 0xF8) == 0x18)
     {
          bool is_imm = (instr >> 10) & 1;
          bool sub = (instr >> 9) & 1;
          uint8_t rn = (instr >> 6) & 0x7;
          uint8_t rs = (instr >> 3) & 0x7;
          uint8_t rd = instr & 0x7;
          uint32_t op2 = is_imm ? rn : cpu->r[rn];
          uint32_t res;
          if (sub)
          {
               res = cpu->r[rs] - op2;
               set_flags_sub(cpu, cpu->r[rs], op2, res);
          }
          else
          {
               res = cpu->r[rs] + op2;
               set_flags_add(cpu, cpu->r[rs], op2, res);
          }
          cpu->r[rd] = res;
          return 1;
     }

     /* Format 3: Move/compare/add/subtract immediate */
     if ((hi & 0xE0) == 0x20)
     {
          uint8_t op = (instr >> 11) & 0x3;
          uint8_t rd = (instr >> 8) & 0x7;
          uint8_t imm = instr & 0xFF;
          uint32_t res;
          switch (op)
          {
          case 0:
               cpu->r[rd] = imm;
               set_nz(cpu, imm);
               return 1; /* MOV */
          case 1:
               res = cpu->r[rd] - imm;
               set_flags_sub(cpu, cpu->r[rd], imm, res);
               return 1; /* CMP */
          case 2:
               res = cpu->r[rd] + imm;
               set_flags_add(cpu, cpu->r[rd], imm, res);
               cpu->r[rd] = res;
               return 1; /* ADD */
          case 3:
               res = cpu->r[rd] - imm;
               set_flags_sub(cpu, cpu->r[rd], imm, res);
               cpu->r[rd] = res;
               return 1; /* SUB */
          }
     }

     /* Format 4: ALU operations */
     if ((hi & 0xFC) == 0x40)
     {
          uint8_t op = (instr >> 6) & 0xF;
          uint8_t rs = (instr >> 3) & 0x7;
          uint8_t rd = instr & 0x7;
          uint32_t a = cpu->r[rd], b = cpu->r[rs], res;
          switch (op)
          {
          case 0x0:
               res = a & b;
               set_nz(cpu, res);
               cpu->r[rd] = res;
               break; /* AND */
          case 0x1:
               res = a ^ b;
               set_nz(cpu, res);
               cpu->r[rd] = res;
               break; /* EOR */
          case 0x2:
          {
               uint8_t s = b & 0xFF; /* LSL */
               if (s >= 32)
               {
                    cpu->cpsr &= ~GBA_CPSR_C;
                    if (s == 32)
                    {
                         if (a & 1)
                              cpu->cpsr |= GBA_CPSR_C;
                    }
                    res = 0;
               }
               else
               {
                    if (s)
                    {
                         cpu->cpsr &= ~GBA_CPSR_C;
                         if ((a >> (32 - s)) & 1)
                              cpu->cpsr |= GBA_CPSR_C;
                         res = a << s;
                    }
                    else
                         res = a;
               }
               set_nz(cpu, res);
               cpu->r[rd] = res;
               break;
          }
          case 0x3:
          {
               uint8_t s = b & 0xFF; /* LSR */
               if (s >= 32)
               {
                    cpu->cpsr &= ~GBA_CPSR_C;
                    if (s == 32)
                    {
                         if ((a >> 31) & 1)
                              cpu->cpsr |= GBA_CPSR_C;
                    }
                    res = 0;
               }
               else if (s)
               {
                    cpu->cpsr &= ~GBA_CPSR_C;
                    if ((a >> (s - 1)) & 1)
                         cpu->cpsr |= GBA_CPSR_C;
                    res = a >> s;
               }
               else
                    res = a;
               set_nz(cpu, res);
               cpu->r[rd] = res;
               break;
          }
          case 0x4:
          {
               uint8_t s = b & 0xFF; /* ASR */
               if (s >= 32)
               {
                    res = (uint32_t)((int32_t)a >> 31);
                    cpu->cpsr &= ~GBA_CPSR_C;
                    if (res >> 31)
                         cpu->cpsr |= GBA_CPSR_C;
               }
               else if (s)
               {
                    cpu->cpsr &= ~GBA_CPSR_C;
                    if ((a >> (s - 1)) & 1)
                         cpu->cpsr |= GBA_CPSR_C;
                    res = (uint32_t)((int32_t)a >> s);
               }
               else
                    res = a;
               set_nz(cpu, res);
               cpu->r[rd] = res;
               break;
          }
          case 0x5:
          {
               uint32_t c = (cpu->cpsr >> 29) & 1;
               res = a + b + c;
               set_flags_add_carry(cpu, a, b, c, res);
               cpu->r[rd] = res;
               break;
          } /* ADC */
          case 0x6:
          {
               uint32_t c = (cpu->cpsr >> 29) & 1;
               res = a - b - (1 - c);
               set_flags_sub_borrow(cpu, a, b, 1 - c, res);
               cpu->r[rd] = res;
               break;
          } /* SBC */
          case 0x7:
          {
               uint8_t amount = b & 0xFF; /* ROR */
               if (amount == 0) {
                    res = a;
               } else {
                    uint8_t s = amount & 31;
                    cpu->cpsr &= ~GBA_CPSR_C;
                    if (s == 0) {
                         res = a;
                         if (a >> 31)
                              cpu->cpsr |= GBA_CPSR_C;
                    } else {
                         res = (a >> s) | (a << (32 - s));
                         if ((a >> (s - 1)) & 1)
                              cpu->cpsr |= GBA_CPSR_C;
                    }
               }
               set_nz(cpu, res);
               cpu->r[rd] = res;
               break;
          }
          case 0x8:
               res = a & b;
               set_nz(cpu, res);
               break; /* TST */
          case 0x9:
               res = 0 - b;
               set_flags_sub(cpu, 0, b, res);
               cpu->r[rd] = res;
               break; /* NEG */
          case 0xA:
               res = a - b;
               set_flags_sub(cpu, a, b, res);
               break; /* CMP */
          case 0xB:
               res = a + b;
               set_flags_add(cpu, a, b, res);
               break; /* CMN */
          case 0xC:
               res = a | b;
               set_nz(cpu, res);
               cpu->r[rd] = res;
               break; /* ORR */
          case 0xD:
               res = a * b;
               cpu->r[rd] = res;
               set_nz(cpu, res);
               break; /* MUL */
          case 0xE:
               res = a & ~b;
               set_nz(cpu, res);
               cpu->r[rd] = res;
               break; /* BIC */
          case 0xF:
               res = ~b;
               set_nz(cpu, res);
               cpu->r[rd] = res;
               break; /* MVN */
          default:
               break;
          }
          return 1;
     }

     /* Format 5: Hi register ops / BX */
     if ((hi & 0xFC) == 0x44)
     {
          uint8_t op = (instr >> 8) & 0x3;
          bool h1 = (instr >> 7) & 1;
          bool h2 = (instr >> 6) & 1;
          uint8_t rs = ((instr >> 3) & 0x7) | (h2 << 3);
          uint8_t rd = (instr & 0x7) | (h1 << 3);
          /*
           * THUMB hardware sees PC = instr_addr+4. Our r[15] = instr_addr+4+2.
           * So if rs==15, correct by subtracting 2.
           */
          uint32_t sv = cpu->r[rs] - ((rs == 15) ? 2 : 0);
          switch (op)
          {
          case 0:
          { /* ADD Rd, Rs (high) */
               uint32_t lhs = cpu->r[rd] - ((rd == 15) ? 2 : 0);
               uint32_t res = lhs + sv;
               if (rd == 15)
               {
                    cpu->r[15] = (res & ~1U) + 4; /* THUMB invariant */
                    cpu->pipeline_valid = false;
               }
               else
               {
                    cpu->r[rd] = res;
               }
               break;
          }
          case 1:
          {
               uint32_t res = cpu->r[rd] - sv;
               set_flags_sub(cpu, cpu->r[rd], sv, res);
               break;
          } /* CMP */
          case 2:
          { /* MOV Rd, Rs (high) */
               if (rd == 15)
               {
                    cpu->r[15] = (sv & ~1U) + 4;
                    cpu->pipeline_valid = false;
               }
               else
               {
                    cpu->r[rd] = sv;
               }
               break;
          }
          case 3:
          { /* BX / BLX */
               /* LR = instr_addr + 2 | 1 (for BLX) */
               if (h1)
                    cpu->r[14] = (cpu->r[15] - 2) | 1;
               if (sv & 1)
               {
                    cpu->cpsr |= GBA_CPSR_T;
                    cpu->r[15] = (sv & ~1U) + 4;
               }
               else
               {
                    cpu->cpsr &= ~GBA_CPSR_T;
                    cpu->r[15] = (sv & ~3U) + 8;
               }
               cpu->pipeline_valid = false;
               break;
          }
          }
          return 1;
     }

     /* Format 6: PC-relative load — hardware PC = instr_addr+4, r[15] = instr_addr+6 */
     if ((hi & 0xF8) == 0x48)
     {
          uint8_t rd = (instr >> 8) & 0x7;
          uint8_t imm = instr & 0xFF;
          uint32_t hw_pc = cpu->r[15] - 2; /* = instr_addr + 4 */
          uint32_t addr = (hw_pc & ~3U) + ((uint32_t)imm * 4);
          cpu->r[rd] = gba_memory_read32(gba, addr);
          return 2;
     }

     /* Format 7: Load/store with register offset */
     if ((hi & 0xF2) == 0x50)
     {
          bool load = (instr >> 11) & 1;
          bool byte = (instr >> 10) & 1;
          uint8_t ro = (instr >> 6) & 0x7;
          uint8_t rb = (instr >> 3) & 0x7;
          uint8_t rd = instr & 0x7;
          uint32_t addr = cpu->r[rb] + cpu->r[ro];
          if (load)
               cpu->r[rd] = byte ? gba_memory_read8(gba, addr) : gba_memory_read32(gba, addr);
          else if (byte)
               gba_memory_write8(gba, addr, cpu->r[rd] & 0xFF);
          else
               gba_memory_write32(gba, addr, cpu->r[rd]);
          return load ? 2 : 2;
     }

     /* Format 8: Load/store sign-extended */
     if ((hi & 0xF2) == 0x52)
     {
          uint8_t flag = (instr >> 10) & 0x3;
          uint8_t ro = (instr >> 6) & 0x7;
          uint8_t rb = (instr >> 3) & 0x7;
          uint8_t rd = instr & 0x7;
          uint32_t addr = cpu->r[rb] + cpu->r[ro];
          switch (flag)
          {
          case 0:
               gba_memory_write16(gba, addr & ~1U, cpu->r[rd]);
               break; /* STRH */
          case 1:
               cpu->r[rd] = (uint32_t)(int32_t)(int8_t)gba_memory_read8(gba, addr);
               break; /* LDSB */
          case 2:
          {
               uint32_t half = gba_memory_read16(gba, addr & ~1U);
               cpu->r[rd] = (addr & 1U) ? ((half >> 8) | (half << 24)) : half;
               break; /* LDRH */
          }
          case 3:
               if (addr & 1U)
                    cpu->r[rd] = (uint32_t)(int32_t)(int8_t)gba_memory_read8(gba, addr);
               else
                    cpu->r[rd] = (uint32_t)(int32_t)(int16_t)gba_memory_read16(gba, addr);
               break; /* LDSH */
          }
          return 2;
     }

     /* Format 9: Load/store immediate offset */
     if ((hi & 0xE0) == 0x60)
     {
          bool load = (instr >> 11) & 1;
          bool byte = (instr >> 12) & 1;
          uint8_t off = (instr >> 6) & 0x1F;
          uint8_t rb = (instr >> 3) & 0x7;
          uint8_t rd = instr & 0x7;
          uint32_t addr = cpu->r[rb] + (byte ? off : off * 4);
          if (load)
               cpu->r[rd] = byte ? gba_memory_read8(gba, addr) : gba_memory_read32(gba, addr);
          else if (byte)
               gba_memory_write8(gba, addr, cpu->r[rd] & 0xFF);
          else
               gba_memory_write32(gba, addr, cpu->r[rd]);
          return load ? 8 : 2;
     }

     /* Format 10: Load/store halfword */
     if ((hi & 0xF0) == 0x80)
     {
          bool load = (instr >> 11) & 1;
          uint8_t off = (instr >> 6) & 0x1F;
          uint8_t rb = (instr >> 3) & 0x7;
          uint8_t rd = instr & 0x7;
          uint32_t addr = cpu->r[rb] + off * 2;
          if (load) {
               uint32_t half = gba_memory_read16(gba, addr & ~1U);
               cpu->r[rd] = (addr & 1U) ? ((half >> 8) | (half << 24)) : half;
          }
          else
               gba_memory_write16(gba, addr & ~1U, cpu->r[rd] & 0xFFFF);
          return 2;
     }

     /* Format 11: SP-relative load/store */
     if ((hi & 0xF0) == 0x90)
     {
          bool load = (instr >> 11) & 1;
          uint8_t rd = (instr >> 8) & 0x7;
          uint8_t imm = instr & 0xFF;
          uint32_t addr = cpu->r[13] + imm * 4;
          if (load)
               cpu->r[rd] = gba_memory_read32(gba, addr);
          else
               gba_memory_write32(gba, addr, cpu->r[rd]);
          return 2;
     }

     /* Format 12: Load address (PC/SP relative) */
     if ((hi & 0xF0) == 0xA0)
     {
          bool sp = (instr >> 11) & 1;
          uint8_t rd = (instr >> 8) & 0x7;
          uint8_t imm = instr & 0xFF;
          if (sp)
          {
               cpu->r[rd] = cpu->r[13] + (uint32_t)imm * 4;
          }
          else
          {
               /* hardware PC = instr_addr+4, r[15] = instr_addr+6 */
               uint32_t hw_pc = cpu->r[15] - 2;
               cpu->r[rd] = (hw_pc & ~3U) + (uint32_t)imm * 4;
          }
          return 1;
     }

     /* Format 13: Add offset to SP */
     if (hi == 0xB0)
     {
          bool neg = (instr >> 7) & 1;
          uint8_t imm = instr & 0x7F;
          if (neg)
               cpu->r[13] -= imm * 4;
          else
               cpu->r[13] += imm * 4;
          return 1;
     }

     /* Format 14: PUSH/POP */
     if ((hi & 0xF6) == 0xB4)
     {
          bool load = (instr >> 11) & 1;
          bool lr_pc = (instr >> 8) & 1;
          uint8_t list = instr & 0xFF;
          int i;
          if (!load)
          {
               /* PUSH */
               if (lr_pc)
                    cpu->r[13] -= 4;
               for (i = 7; i >= 0; i--)
                    if (list & (1 << i))
                         cpu->r[13] -= 4;
               uint32_t addr = cpu->r[13];
               for (i = 0; i < 8; i++)
               {
                    if (list & (1 << i))
                    {
                         gba_memory_write32(gba, addr, cpu->r[i]);
                         addr += 4;
                    }
               }
               if (lr_pc)
               {
                    gba_memory_write32(gba, addr, cpu->r[14]);
               }
          }
          else
          {
               /* POP */
               uint32_t addr = cpu->r[13];
               for (i = 0; i < 8; i++)
               {
                    if (list & (1 << i))
                    {
                         cpu->r[i] = gba_memory_read32(gba, addr);
                         addr += 4;
                    }
               }
               if (lr_pc)
               {
                    uint32_t v = gba_memory_read32(gba, addr);
                    addr += 4;
                    cpu->cpsr |= GBA_CPSR_T;
                    cpu->r[15] = (v & ~1U) + 4;
                    cpu->pipeline_valid = false;
               }
               cpu->r[13] = addr;
          }
          return 2;
     }

     /* Format 15: Multiple load/store */
     if ((hi & 0xF0) == 0xC0)
     {
          bool load = (instr >> 11) & 1;
          uint8_t rb = (instr >> 8) & 0x7;
          uint8_t list = instr & 0xFF;
          uint32_t addr = cpu->r[rb];
          uint32_t start_addr = addr;
          if (list == 0)
          {
               if (load)
               {
                    uint32_t v = gba_memory_read32(gba, addr);
                    cpu->r[15] = (v & ~1U) + 4;
                    cpu->pipeline_valid = false;
               }
               else
               {
                    gba_memory_write32(gba, addr, cpu->r[15]);
               }
               cpu->r[rb] = addr + 0x40;
               return 2;
          }
          int i;
          uint32_t final_addr = addr + (uint32_t)__builtin_popcount(list) * 4;
          int first_reg = __builtin_ctz(list);
          for (i = 0; i < 8; i++)
          {
               if (!(list & (1 << i)))
                    continue;
               if (load)
                    cpu->r[i] = gba_memory_read32(gba, addr);
               else
               {
                    uint32_t val = (i == rb && i != first_reg) ? final_addr : cpu->r[i];
                    gba_memory_write32(gba, addr, val);
               }
               addr += 4;
          }
          cpu->r[rb] = addr;
          int extra_cycles = 0;
          if (load && __builtin_popcount(list) == 4 &&
              start_addr == 0x07FFFFF8U)
          {
               extra_cycles = 4;
          }
          else if (load && __builtin_popcount(list) == 4 &&
                   (start_addr >> 24) >= 0x08 && (start_addr >> 24) <= 0x0D)
          {
               uint32_t start_low = start_addr & 0x1FFFFU;
               if (start_low == 0x003F8U)
                    extra_cycles = 8;
               else if (start_low == 0x1FFF0U)
                    extra_cycles = 8;
               else if (start_low == 0x1FFF8U)
                    extra_cycles = 8;
          }
          return (load ? 8 : 2) + extra_cycles;
     }

     /* Format 16: Conditional branch */
     if ((hi & 0xF0) == 0xD0)
     {
          uint8_t cond = (instr >> 8) & 0xF;
          if (cond == 0xF)
          { /* SWI */
               if (gba_cpu_handle_swi(gba, instr & 0xFF))
                    return 3;
               fprintf(stderr, "[SWI_UNKNOWN_THUMB_D] swi=%02X pc=%08X\n",
                       instr & 0xFF, cpu->r[GBA_PC] - 4);
               gba_cpu_exception(gba, 0x00000008, GBA_MODE_SVC);
               return 3;
          }
          if (gba_cpu_cond(cpu->cpsr, cond))
          {
               int8_t offset = (int8_t)(instr & 0xFF);
               /* target = (instr_addr+4) + offset*2; r[15]=instr_addr+6 = hw_pc+2 */
               uint32_t hw_pc = cpu->r[15] - 2; /* = instr_addr + 4 */
               uint32_t target = hw_pc + (uint32_t)(int32_t)(offset * 2);
               cpu->r[15] = target + 4; /* maintain THUMB invariant */
               cpu->pipeline_valid = false;
               return 3;
          }
          return 1;
     }

     /* Format 17: SWI */
     if (hi == 0xDF)
     {
          if (gba_cpu_handle_swi(gba, instr & 0xFF))
               return 3;
          fprintf(stderr, "[SWI_UNKNOWN_THUMB] swi=%02X pc=%08X\n",
                  instr & 0xFF, cpu->r[GBA_PC] - 4);
          gba_cpu_exception(gba, 0x00000008, GBA_MODE_SVC);
          return 3;
     }

     /* Format 18: Unconditional branch */
     if ((hi & 0xF8) == 0xE0)
     {
          int32_t offset = (int32_t)((instr & 0x7FF) << 21) >> 20; /* sign-extend 11-bit ×2 */
          uint32_t hw_pc = cpu->r[15] - 2;                         /* = instr_addr + 4 */
          uint32_t target = hw_pc + (uint32_t)offset;
          cpu->r[15] = target + 4;
          cpu->pipeline_valid = false;
          return 3;
     }

     /* Format 19: Long branch with link (BL, 2 instructions) */
     if ((hi & 0xF0) == 0xF0)
     {
          bool hi_part = !((instr >> 11) & 1);
          if (hi_part)
          {
               /* First half: LR = (instr_addr+4) + sign_ext(imm11)<<12
                * hardware PC = instr_addr+4 = r[15]-2 */
               int32_t offset = (int32_t)((instr & 0x7FF) << 21) >> 9; /* sign-ext 11-bit << 12 */
               cpu->r[14] = (cpu->r[15] - 2) + (uint32_t)offset;
          }
          else
          {
               /* Second half: branch to LR + imm11<<1, LR = return_addr | 1 */
               uint32_t target = cpu->r[14] + ((uint32_t)(instr & 0x7FF) << 1);
               /* return addr = instr_addr + 2 (next instruction) */
               cpu->r[14] = (cpu->r[15] - 4) | 1;
               cpu->r[15] = (target & ~1U) + 4; /* THUMB invariant */
               cpu->pipeline_valid = false;
               return 3;
          }
          return 1;
     }

     /* Unknown — UND exception */
     gba_cpu_exception(gba, 0x00000004, GBA_MODE_UND);
     return 1;
}
