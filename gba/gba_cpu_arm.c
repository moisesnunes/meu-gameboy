/*
 * gba_cpu_arm.c — ARM instruction set execution for the ARM7TDMI core.
 *
 * Covers: data processing (all ALU opcodes), multiply/MLA/MULL/MLAL,
 * single/block transfers (LDR/STR/LDM/STM), branch, MRS/MSR, SWP, SWI,
 * and coprocessor stubs.  Called from gba_cpu_step() when CPSR_T is clear.
 */

#include <string.h>
#include "gba.h"

/*
 * Barrel shifter
 */

typedef struct
{
     uint32_t val;
     uint32_t carry; /* 0 or 1 */
} shift_result_t;

static shift_result_t barrel_shift(uint32_t val, uint8_t type, uint8_t amount,
                                   uint32_t cpsr_c, bool register_shift)
{
     shift_result_t r = {val, cpsr_c};
     if (register_shift && amount == 0)
          return r;
     switch (type)
     {
     case 0: /* LSL */
          if (amount == 0)
               return r;
          if (amount >= 32)
          {
               r.carry = (amount == 32) ? (val & 1) : 0;
               r.val = 0;
          }
          else
          {
               r.carry = (val >> (32 - amount)) & 1;
               r.val = val << amount;
          }
          break;
     case 1: /* LSR */
          if (amount == 0 || amount >= 32)
          {
               r.carry = (amount == 0 || amount == 32) ? (val >> 31) : 0;
               r.val = 0;
          }
          else
          {
               r.carry = (val >> (amount - 1)) & 1;
               r.val = val >> amount;
          }
          break;
     case 2: /* ASR */
          if (amount == 0 || amount >= 32)
          {
               r.carry = val >> 31;
               r.val = (uint32_t)((int32_t)val >> 31);
          }
          else
          {
               r.carry = (val >> (amount - 1)) & 1;
               r.val = (uint32_t)((int32_t)val >> amount);
          }
          break;
     case 3: /* ROR */
          if (amount == 0)
          {
               /* RRX: rotate right extended through carry */
               r.carry = val & 1;
               r.val = (val >> 1) | (cpsr_c << 31);
          }
          else
          {
               uint8_t rot = amount & 31;
               r.carry = (val >> ((amount - 1) & 31)) & 1;
               r.val = rot ? ((val >> rot) | (val << (32 - rot))) : val;
          }
          break;
     }
     return r;
}

/* Decode flexible second operand (register / immediate) */
static shift_result_t decode_op2(struct gba *gba, uint32_t instr, bool is_imm)
{
     struct gba_cpu *cpu = &gba->cpu;
     uint32_t cpsr_c = (cpu->cpsr >> 29) & 1;

     if (is_imm)
     {
          uint8_t rot = ((instr >> 8) & 0xF) * 2;
          uint32_t imm = instr & 0xFF;
          uint32_t val = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
          uint32_t carry = rot ? ((val >> 31) & 1) : cpsr_c;
          shift_result_t r = {val, carry};
          return r;
     }
     else
     {
          uint8_t rm = instr & 0xF;
          uint8_t type = (instr >> 5) & 0x3;
          bool reg_sh = (instr >> 4) & 1;
          uint8_t amount;
          uint32_t base = cpu->r[rm];
          /*
           * Hardware: if rm==PC with immediate shift, reads instr_addr+8.
           * Our r[15] = instr_addr+8+4, so subtract 4.
           * With register shift, reads instr_addr+12, so subtract 0 (already correct).
           */
          if (rm == 15)
               base -= reg_sh ? 0 : 4;

          if (reg_sh)
          {
               uint8_t rs = (instr >> 8) & 0xF;
               amount = cpu->r[rs] & 0xFF;
          }
          else
          {
               amount = (instr >> 7) & 0x1F;
          }
          return barrel_shift(base, type, amount, cpsr_c, reg_sh);
     }
}

/* Set N and Z flags based on result */
static void set_nz(struct gba_cpu *cpu, uint32_t result)
{
     cpu->cpsr &= ~(GBA_CPSR_N | GBA_CPSR_Z);
     if (result == 0)
          cpu->cpsr |= GBA_CPSR_Z;
     if (result >> 31)
          cpu->cpsr |= GBA_CPSR_N;
}

/* Set all ALU flags for ADD */
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

/* Set all ALU flags for SUB (b subtracted from a) */
static void set_flags_sub_borrow(struct gba_cpu *cpu, uint32_t a, uint32_t b,
                                 uint32_t borrow, uint32_t result)
{
     uint64_t unsigned_diff = (uint64_t)a - (uint64_t)b - (uint64_t)borrow;
     int64_t signed_diff = (int64_t)(int32_t)a - (int64_t)(int32_t)b -
                           (int64_t)borrow;

     set_nz(cpu, result);
     cpu->cpsr &= ~(GBA_CPSR_C | GBA_CPSR_V);
     if ((unsigned_diff >> 32) == 0)
          cpu->cpsr |= GBA_CPSR_C; /* no borrow */
     if (signed_diff < INT32_MIN || signed_diff > INT32_MAX)
          cpu->cpsr |= GBA_CPSR_V;
}

static void set_flags_sub(struct gba_cpu *cpu, uint32_t a, uint32_t b, uint32_t result)
{
     set_flags_sub_borrow(cpu, a, b, 0, result);
}

/*
 * Data processing instructions
 */

static int exec_data_proc(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool is_imm = (instr >> 25) & 1;
     uint8_t opcode = (instr >> 21) & 0xF;
     bool set_cc = (instr >> 20) & 1;
     uint8_t rn = (instr >> 16) & 0xF;
     uint8_t rd = (instr >> 12) & 0xF;

     uint32_t mrs_pattern = instr & 0x0FBF0FFFU;
     if (mrs_pattern == 0x010F0000U || mrs_pattern == 0x014F0000U)
     {
          bool use_spsr = (instr >> 22) & 1;
          cpu->r[rd] = use_spsr ? cpu->spsr : cpu->cpsr;
          return 1;
     }

     shift_result_t op2 = decode_op2(gba, instr, is_imm);

     uint32_t msr_pattern = instr & 0x0DB0F000U;
     if (msr_pattern == 0x0120F000U || msr_pattern == 0x0160F000U)
     {
          bool use_spsr = (instr >> 22) & 1;
          uint32_t field = (instr >> 16) & 0xF;
          uint32_t mask = 0;
          uint32_t val = is_imm ? op2.val : cpu->r[instr & 0xF];

          if (field & 1)
               mask |= 0x000000FFU; /* control */
          if (field & 2)
               mask |= 0x0000FF00U; /* extension */
          if (field & 4)
               mask |= 0x00FF0000U; /* status */
          if (field & 8)
               mask |= 0xFF000000U; /* flags */

          if (use_spsr)
               cpu->spsr = (cpu->spsr & ~mask) | (val & mask);
          else
          {
               gba_cpu_set_cpsr(gba, (cpu->cpsr & ~mask) | (val & mask));
          }
          return 1;
     }

     bool register_shift = !is_imm && ((instr >> 4) & 1);
     uint32_t n = cpu->r[rn];
     /*
      * r[15] at this point = instr_addr+12. Most data-processing PC reads see
      * instr_addr+8, but register-controlled shifts expose the extra prefetch
      * cycle for Rn/operand2 PC reads.
      */
     if (rn == 15)
          n -= register_shift ? 0 : 4;
     uint32_t res = 0;
     bool write = true;

     switch (opcode)
     {
     case 0x0:
          res = n & op2.val;
          break; /* AND */
     case 0x1:
          res = n ^ op2.val;
          break; /* EOR */
     case 0x2:   /* SUB */
          res = n - op2.val;
          if (set_cc)
               set_flags_sub(cpu, n, op2.val, res);
          break;
     case 0x3: /* RSB */
          res = op2.val - n;
          if (set_cc)
               set_flags_sub(cpu, op2.val, n, res);
          break;
     case 0x4: /* ADD */
          res = n + op2.val;
          if (set_cc)
               set_flags_add(cpu, n, op2.val, res);
          break;
     case 0x5:
     { /* ADC */
          uint32_t c = (cpu->cpsr >> 29) & 1;
          res = n + op2.val + c;
          if (set_cc)
               set_flags_add_carry(cpu, n, op2.val, c, res);
          break;
     }
     case 0x6:
     { /* SBC */
          uint32_t c = (cpu->cpsr >> 29) & 1;
          res = n - op2.val - (1 - c);
          if (set_cc)
               set_flags_sub_borrow(cpu, n, op2.val, 1 - c, res);
          break;
     }
     case 0x7:
     { /* RSC */
          uint32_t c = (cpu->cpsr >> 29) & 1;
          res = op2.val - n - (1 - c);
          if (set_cc)
               set_flags_sub_borrow(cpu, op2.val, n, 1 - c, res);
          break;
     }
     case 0x8: /* TST */
          res = n & op2.val;
          write = false;
          if (!set_cc)
               goto mrs_msr;
          break;
     case 0x9: /* TEQ */
          res = n ^ op2.val;
          write = false;
          if (!set_cc)
               goto mrs_msr;
          break;
     case 0xA: /* CMP */
          res = n - op2.val;
          write = false;
          if (!set_cc)
               goto mrs_msr;
          if (set_cc)
               set_flags_sub(cpu, n, op2.val, res);
          break;
     case 0xB: /* CMN */
          res = n + op2.val;
          write = false;
          if (!set_cc)
               goto mrs_msr;
          if (set_cc)
               set_flags_add(cpu, n, op2.val, res);
          break;
     case 0xC:
          res = n | op2.val;
          break; /* ORR */
     case 0xD:
          res = op2.val;
          break; /* MOV */
     case 0xE:
          res = n & ~op2.val;
          break; /* BIC */
     case 0xF:
          res = ~op2.val;
          break; /* MVN */
     default:
          break;
     }

     if (set_cc && (opcode == 0x0 || opcode == 0x1 ||
                    opcode == 0xC || opcode == 0xD ||
                    opcode == 0xE || opcode == 0xF))
     {
          set_nz(cpu, res);
          cpu->cpsr &= ~GBA_CPSR_C;
          if (op2.carry)
               cpu->cpsr |= GBA_CPSR_C;
     }
     if (set_cc && (opcode == 8 || opcode == 9))
     {
          set_nz(cpu, res);
          cpu->cpsr &= ~GBA_CPSR_C;
          if (op2.carry)
               cpu->cpsr |= GBA_CPSR_C;
     }

     if (set_cc && rd == 15 && opcode >= 8 && opcode <= 11)
     {
          gba_cpu_set_cpsr(gba, gba_cpu_get_spsr(gba));
          return 1;
     }

     if (write)
     {
          if (rd == 15)
          {
               if (set_cc)
                    gba_cpu_set_cpsr(gba, gba_cpu_get_spsr(gba));
               /* Maintain invariant: r[15] = dest + 8 (ARM) or dest + 4 (THUMB) */
               bool t = (cpu->cpsr & GBA_CPSR_T) != 0;
               cpu->r[15] = (res & (t ? ~1U : ~3U)) + (t ? 4u : 8u);
               cpu->pipeline_valid = false;
          }
          else
          {
               cpu->r[rd] = res;
          }
     }
     return 1;

mrs_msr:
     /* MRS/MSR: encoded in bits 21-20 = 00/01 with opcode 8/9/A/B */
     {
          bool msr = (instr >> 21) & 1;
          if (!msr)
          {
               /* MRS: move PSR to register */
               bool use_spsr = (instr >> 22) & 1;
               cpu->r[rd] = use_spsr ? cpu->spsr : cpu->cpsr;
          }
          else
          {
               /* MSR: move register/immediate to PSR */
               bool use_spsr = (instr >> 22) & 1;
               uint32_t mask = 0;
               if ((instr >> 16) & 1)
                    mask |= 0xFF000000; /* flags */
               if ((instr >> 17) & 1)
                    mask |= 0x00FF0000; /* status */
               if ((instr >> 18) & 1)
                    mask |= 0x0000FF00; /* extension */
               if ((instr >> 19) & 1)
                    mask |= 0x000000FF; /* control */
               uint32_t val = is_imm ? op2.val : cpu->r[instr & 0xF];
               if (use_spsr)
               {
                    cpu->spsr = (cpu->spsr & ~mask) | (val & mask);
               }
               else
               {
                    /* In USR/SYS mode, control bits (mode, IRQ disable) cannot
                     * be changed via MSR — only condition flags (bits 31-24). */
                    enum gba_cpu_mode cur_mode = (enum gba_cpu_mode)(cpu->cpsr & GBA_CPSR_M);
                    bool privileged = (cur_mode != GBA_MODE_USR);
                    uint32_t eff_mask = privileged ? mask : (mask & 0xFF000000u);
                    gba_cpu_set_cpsr(gba, (cpu->cpsr & ~eff_mask) | (val & eff_mask));
               }
          }
     }
     return 1;
}

/*
 * Branch
 */

static int exec_branch(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool link = (instr >> 24) & 1;
     uint32_t pc = cpu->r[15] - 12;
     /*
      * r[15] currently = instr_addr + 8 + 4 (already advanced by step).
      * The branch offset is relative to instr_addr + 8.
      * destination = (instr_addr + 8) + offset = (r[15] - 4) + offset
      * Then maintain invariant: r[15] = destination + 8
      */
     int32_t offset = (int32_t)(instr << 8) >> 6; /* sign-extend 24-bit, ×4 */
     uint32_t instr_pc8 = cpu->r[15] - 4;         /* = instr_addr + 8 */

     if (link)
          cpu->r[14] = instr_pc8 - 4; /* = instr_addr + 4 (next instr) */
     uint32_t target = instr_pc8 + (uint32_t)offset;
     cpu->r[15] = target + 8;
     cpu->pipeline_valid = false;
     gba_event_trace(gba, GBA_EVENT_BRANCH, pc, target, link ? 1u : 0u);
     return 3;
}

/* BX / BLX */
static int exec_bx(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool link = ((instr >> 4) & 0xF) == 3; /* BLX = lo4==3 */
     uint8_t rm = instr & 0xF;
     uint32_t addr = cpu->r[rm];
     uint32_t pc = cpu->r[15] - 12;

     /* LR = instr_addr + 4 = (r[15] - 4) - 4 */
     if (link)
          cpu->r[14] = cpu->r[15] - 8;

     if (addr & 1)
     {
          /* Branch to THUMB: destination = addr & ~1, invariant r[15] = dest + 4 */
          cpu->cpsr |= GBA_CPSR_T;
          cpu->r[15] = (addr & ~1U) + 4;
     }
     else
     {
          /* Branch to ARM: destination = addr & ~3, invariant r[15] = dest + 8 */
          cpu->cpsr &= ~GBA_CPSR_T;
          cpu->r[15] = (addr & ~3U) + 8;
     }
     cpu->pipeline_valid = false;
     gba_event_trace(gba, GBA_EVENT_BRANCH, pc, addr & ~1U,
                     (link ? 1u : 0u) | ((addr & 1U) ? 2u : 0u));
     return 3;
}

/*
 * Load/Store single register
 */

static int exec_ldr_str(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool is_reg = (instr >> 25) & 1;
     bool pre = (instr >> 24) & 1;
     bool up = (instr >> 23) & 1;
     bool byte = (instr >> 22) & 1;
     bool writeback = (instr >> 21) & 1;
     bool load = (instr >> 20) & 1;
     uint8_t rn = (instr >> 16) & 0xF;
     uint8_t rd = (instr >> 12) & 0xF;

     uint32_t base = cpu->r[rn];
     /* r[15] = instr_addr+8+4; hardware address = instr_addr+8, so subtract 4 */
     if (rn == 15)
          base -= 4;

     uint32_t offset;
     if (is_reg)
     {
          shift_result_t sh = decode_op2(gba, instr, false);
          offset = sh.val;
     }
     else
     {
          offset = instr & 0xFFF;
     }

     uint32_t addr = pre ? (up ? base + offset : base - offset) : base;

     if (load)
     {
          uint32_t val;
          if (byte)
               val = gba_memory_read8(gba, addr);
          else
               val = gba_memory_read32(gba, addr);
          if (rd == 15)
          {
               /* LDR PC: can switch to THUMB if bit 0 set (ARMv5T) */
               if (val & 1)
               {
                    cpu->cpsr |= GBA_CPSR_T;
                    cpu->r[15] = (val & ~1U) + 4;
               }
               else
               {
                    cpu->r[15] = (val & ~3U) + 8;
               }
               cpu->pipeline_valid = false;
          }
          else
          {
               cpu->r[rd] = val;
          }
     }
     else
     {
          uint32_t val = cpu->r[rd];
          if (byte)
               gba_memory_write8(gba, addr, val & 0xFF);
          else
               gba_memory_write32(gba, addr, val);
     }

     if (!pre)
          addr = up ? base + offset : base - offset;
     if ((!pre || writeback) && !(load && rd == rn))
          cpu->r[rn] = addr;

     return load ? 3 : 2;
}

/* LDR/STR halfword, signed byte */
static int exec_ldr_str_h(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool pre = (instr >> 24) & 1;
     bool up = (instr >> 23) & 1;
     bool imm = (instr >> 22) & 1;
     bool writeback = (instr >> 21) & 1;
     bool load = (instr >> 20) & 1;
     uint8_t rn = (instr >> 16) & 0xF;
     uint8_t rd = (instr >> 12) & 0xF;
     uint8_t sh_h = (instr >> 5) & 0x3;

     uint32_t offset = imm ? ((instr & 0xF) | (((instr >> 8) & 0xF) << 4)) : cpu->r[instr & 0xF];
     uint32_t base = cpu->r[rn];
     uint32_t addr = pre ? (up ? base + offset : base - offset) : base;

     if (load)
     {
          uint32_t val = 0;
          switch (sh_h)
          {
          case 1:
          {
               uint32_t half = gba_memory_read16(gba, addr & ~1U);
               val = (addr & 1U) ? ((half >> 8) | (half << 24)) : half;
               break; /* LDRH */
          }
          case 2:
               val = (uint32_t)(int32_t)(int8_t)gba_memory_read8(gba, addr); /* LDRSB */
               break;
          case 3:
               if (addr & 1U)
                    val = (uint32_t)(int32_t)(int8_t)gba_memory_read8(gba, addr);
               else
                    val = (uint32_t)(int32_t)(int16_t)gba_memory_read16(gba, addr);
               break; /* LDRSH */
          }
          if (rd == 15)
          {
               cpu->r[15] = (val & ~3U) + 8;
               cpu->pipeline_valid = false;
          }
          else
          {
               cpu->r[rd] = val;
          }
     }
     else
     {
          if (sh_h == 1)
               gba_memory_write16(gba, addr, (uint16_t)cpu->r[rd]);
     }

     if (!pre)
          addr = up ? base + offset : base - offset;
     if ((!pre || writeback) && !(load && rd == rn))
          cpu->r[rn] = addr;
     return load ? 3 : 2;
}

static uint32_t get_user_reg(struct gba_cpu *cpu, int reg)
{
     switch (reg)
     {
     case 8:
          return cpu->r8_usr;
     case 9:
          return cpu->r9_usr;
     case 10:
          return cpu->r10_usr;
     case 11:
          return cpu->r11_usr;
     case 12:
          return cpu->r12_usr;
     case 13:
          return cpu->r13_usr;
     case 14:
          return cpu->r14_usr;
     default:
          return cpu->r[reg];
     }
}

static void set_user_reg(struct gba_cpu *cpu, int reg, uint32_t val)
{
     switch (reg)
     {
     case 8:
          cpu->r8_usr = val;
          break;
     case 9:
          cpu->r9_usr = val;
          break;
     case 10:
          cpu->r10_usr = val;
          break;
     case 11:
          cpu->r11_usr = val;
          break;
     case 12:
          cpu->r12_usr = val;
          break;
     case 13:
          cpu->r13_usr = val;
          break;
     case 14:
          cpu->r14_usr = val;
          break;
     default:
          cpu->r[reg] = val;
          break;
     }
}

/* LDM/STM */
static int exec_ldm_stm(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool pre = (instr >> 24) & 1;
     bool up = (instr >> 23) & 1;
     bool psr = (instr >> 22) & 1;
     bool wb = (instr >> 21) & 1;
     bool load = (instr >> 20) & 1;
     uint8_t rn = (instr >> 16) & 0xF;
     uint16_t list = instr & 0xFFFF;

     uint32_t base = cpu->r[rn];
     int count = __builtin_popcount(list);
     if (count == 0)
     {
          uint32_t start = up ? (pre ? base + 4 : base)
                              : (pre ? base - 0x40 : base - 0x3C);
          if (load)
          {
               uint32_t val = gba_memory_read32(gba, start);
               cpu->r[15] = (val & ~3U) + 8;
               cpu->pipeline_valid = false;
          }
          else
          {
               gba_memory_write32(gba, start, cpu->r[15]);
          }
          if (wb)
               cpu->r[rn] = up ? base + 0x40 : base - 0x40;
          return 2;
     }
     uint32_t addr = up ? base : base - count * 4;
     if (!up && pre)
          addr += 4;
     if (up && pre)
          addr += 0; /* handled below */
     uint32_t start = addr;

     /* Compute start for pre/post */
     start = up ? (pre ? base + 4 : base) : (pre ? base - count * 4 : base - count * 4 + 4);

     int cycles = load ? 7 : 1;
     uint32_t cur = start;
     uint32_t final_wb = up ? base + (uint32_t)count * 4 : base - (uint32_t)count * 4;
     int first_reg = __builtin_ctz(list);
     int i;
     for (i = 0; i < 16; i++)
     {
          if (!(list & (1 << i)))
               continue;
          if (load)
          {
               uint32_t val = gba_memory_read32(gba, cur & ~3U);
               if (i == 15)
               {
                    /* Maintain invariant */
                    cpu->r[15] = (val & ~3U) + 8;
                    cpu->pipeline_valid = false;
               }
               else
               {
                    if (psr)
                    {
                         set_user_reg(cpu, i, val);
                    }
                    else
                         cpu->r[i] = val;
               }
          }
          else
          {
               uint32_t val = psr ? get_user_reg(cpu, i) : cpu->r[i];
               if (wb && i == rn && i != first_reg)
                    val = final_wb;
               gba_memory_write32(gba, cur, val);
          }
          cur += 4;
          cycles++;
     }

     if (wb && !(load && (list & (1 << rn))))
     {
          cpu->r[rn] = final_wb;
     }

     if (load && psr && (list & (1 << 15)))
          gba_cpu_set_cpsr(gba, gba_cpu_get_spsr(gba));

     return cycles;
}

/* Multiply */
/* ARM7TDMI multiply latency: 1-4 extra cycles based on significant bytes of Rs.
 * Signed: checks whether upper bytes are all-0 or all-1 (sign extension).
 * Unsigned: checks whether upper bytes are all-0. */
static int mul_cycles_signed(uint32_t rs)
{
     if ((rs & 0xFFFFFF00u) == 0xFFFFFF00u || !(rs & 0xFFFFFF00u))
          return 1;
     if ((rs & 0xFFFF0000u) == 0xFFFF0000u || !(rs & 0xFFFF0000u))
          return 2;
     if ((rs & 0xFF000000u) == 0xFF000000u || !(rs & 0xFF000000u))
          return 3;
     return 4;
}

static int mul_cycles_unsigned(uint32_t rs)
{
     if (!(rs & 0xFFFFFF00u))
          return 1;
     if (!(rs & 0xFFFF0000u))
          return 2;
     if (!(rs & 0xFF000000u))
          return 3;
     return 4;
}

static int exec_mul(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool accum = (instr >> 21) & 1;
     bool set_cc = (instr >> 20) & 1;
     uint8_t rd = (instr >> 16) & 0xF;
     uint8_t rn = (instr >> 12) & 0xF;
     uint8_t rs = (instr >> 8) & 0xF;
     uint8_t rm = instr & 0xF;

     uint32_t res = cpu->r[rm] * cpu->r[rs];
     if (accum)
          res += cpu->r[rn];
     cpu->r[rd] = res;

     if (set_cc)
          set_nz(cpu, res);
     /* MUL: 1S + mI where m = signed latency of Rs; MLA adds 1 extra I */
     return 1 + mul_cycles_signed(cpu->r[rs]) + (accum ? 1 : 0);
}

/* Long multiply (UMULL/SMULL/UMLAL/SMLAL) */
static int exec_mull(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool sign = (instr >> 22) & 1;
     bool accum = (instr >> 21) & 1;
     bool set_cc = (instr >> 20) & 1;
     uint8_t rdhi = (instr >> 16) & 0xF;
     uint8_t rdlo = (instr >> 12) & 0xF;
     uint8_t rs = (instr >> 8) & 0xF;
     uint8_t rm = instr & 0xF;

     uint64_t res;
     if (sign)
     {
          res = (uint64_t)((int64_t)(int32_t)cpu->r[rm] * (int32_t)cpu->r[rs]);
     }
     else
     {
          res = (uint64_t)cpu->r[rm] * cpu->r[rs];
     }

     if (accum)
     {
          uint64_t acc = ((uint64_t)cpu->r[rdhi] << 32) | cpu->r[rdlo];
          res += acc;
     }

     cpu->r[rdlo] = (uint32_t)(res & 0xFFFFFFFF);
     cpu->r[rdhi] = (uint32_t)(res >> 32);

     if (set_cc)
     {
          cpu->cpsr &= ~(GBA_CPSR_N | GBA_CPSR_Z);
          if (res == 0)
               cpu->cpsr |= GBA_CPSR_Z;
          if (res >> 63)
               cpu->cpsr |= GBA_CPSR_N;
     }
     /* MULL: 1S + mI + 1I (long); MLAL adds 1 extra I.
      * SMULL/SMLAL use signed latency; UMULL/UMLAL use unsigned. */
     int m = sign ? mul_cycles_signed(cpu->r[rs]) : mul_cycles_unsigned(cpu->r[rs]);
     return 1 + m + 1 + (accum ? 1 : 0);
}

/* CLZ — Count Leading Zeros (ARMv5T)
 * Encoding: cond 0001 0110 1111 Rd 1111 0001 Rm
 * hi=0x16, lo4=0x1, bits[19:16]=0xF, bits[11:8]=0xF */
static int exec_clz(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     uint8_t rd = (instr >> 12) & 0xF;
     uint8_t rm = instr & 0xF;
     uint32_t val = cpu->r[rm];
     uint32_t count = 0;
     if (val == 0)
          count = 32;
     else
          while (!(val & 0x80000000U))
          {
               val <<= 1;
               count++;
          }
     cpu->r[rd] = count;
     return 1;
}

/* SMULxy / SMLAxy — 16-bit signed multiply (ARMv5E)
 * SMUL: cond 0001 0110 0000 Rd 0000 1yx0 Rm  (hi=0x16, bits[23:21]=011, set=0)
 * SMLA: cond 0001 0000 Rd   Rn Rs   1yx0 Rm  (hi=0x10..0x13, lo4=0x8..0xB)
 * Selects top (y=1) or bottom (y=0) halfword of each operand. */
static int exec_smulxy(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool y = (instr >> 6) & 1;
     bool x = (instr >> 5) & 1;
     uint8_t rd = (instr >> 16) & 0xF;
     uint8_t rn = (instr >> 12) & 0xF; /* accumulate register (SMLA only) */
     uint8_t rs = (instr >> 8) & 0xF;
     uint8_t rm = instr & 0xF;

     int16_t op1 = (int16_t)(x ? (cpu->r[rm] >> 16) : (cpu->r[rm] & 0xFFFF));
     int16_t op2 = (int16_t)(y ? (cpu->r[rs] >> 16) : (cpu->r[rs] & 0xFFFF));
     int32_t product = (int32_t)op1 * (int32_t)op2;

     bool accumulate = ((instr >> 21) & 0x7) == 0; /* SMLA: bits[23:21]=000 */
     if (accumulate)
     {
          int64_t result = (int64_t)product + (int32_t)cpu->r[rn];
          cpu->r[rd] = (uint32_t)result;
          /* set Q flag on overflow */
          if (result > (int64_t)0x7FFFFFFF || result < (int64_t)(-0x80000000LL))
               cpu->cpsr |= GBA_CPSR_Q;
     }
     else
     {
          cpu->r[rd] = (uint32_t)product;
     }
     return 1;
}

/* SMULWy / SMLAWy — 32×16 multiply, keep high 32 bits (ARMv5E)
 * SMULWy: cond 0001 0010 0000 Rd 0000 1y10 Rm  (hi=0x12, lo4=0xA or 0xE)
 * SMLAWy: cond 0001 0010 Rd   Rn Rs   1y00 Rm  (hi=0x12, lo4=0x8 or 0xC) */
static int exec_smulwy(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool y = (instr >> 6) & 1;
     bool accumulate = !((instr >> 5) & 1); /* bit5=0 → SMLAW, bit5=1 → SMULW */
     uint8_t rd = (instr >> 16) & 0xF;
     uint8_t rn = (instr >> 12) & 0xF;
     uint8_t rs = (instr >> 8) & 0xF;
     uint8_t rm = instr & 0xF;

     int16_t op2 = (int16_t)(y ? (cpu->r[rs] >> 16) : (cpu->r[rs] & 0xFFFF));
     int64_t product = (int64_t)(int32_t)cpu->r[rm] * op2;
     int32_t result = (int32_t)(product >> 16);

     if (accumulate)
     {
          int64_t acc = (int64_t)result + (int32_t)cpu->r[rn];
          cpu->r[rd] = (uint32_t)acc;
          if (acc > (int64_t)0x7FFFFFFF || acc < (int64_t)(-0x80000000LL))
               cpu->cpsr |= GBA_CPSR_Q;
     }
     else
     {
          cpu->r[rd] = (uint32_t)result;
     }
     return 1;
}

/* SMLALxy — 16×16 multiply accumulate into 64-bit pair (ARMv5E)
 * cond 0001 0100..0111 RdHi RdLo Rs 1yx0 Rm  (hi=0x14..0x17, lo4=0x8..0xB) */
static int exec_smlalxy(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool y = (instr >> 6) & 1;
     bool x = (instr >> 5) & 1;
     uint8_t rdhi = (instr >> 16) & 0xF;
     uint8_t rdlo = (instr >> 12) & 0xF;
     uint8_t rs = (instr >> 8) & 0xF;
     uint8_t rm = instr & 0xF;

     int16_t op1 = (int16_t)(x ? (cpu->r[rm] >> 16) : (cpu->r[rm] & 0xFFFF));
     int16_t op2 = (int16_t)(y ? (cpu->r[rs] >> 16) : (cpu->r[rs] & 0xFFFF));
     int64_t product = (int64_t)op1 * op2;
     int64_t acc = ((int64_t)cpu->r[rdhi] << 32) | cpu->r[rdlo];
     int64_t result = acc + product;

     cpu->r[rdlo] = (uint32_t)(result & 0xFFFFFFFF);
     cpu->r[rdhi] = (uint32_t)(result >> 32);
     return 2;
}

/* QADD / QSUB / QDADD / QDSUB — saturating add/sub (ARMv5E)
 * Encoding: cond 0001 0xx0 Rn Rd 0000 0101 Rm
 * hi=0x10..0x16 (even), lo4=0x5 */
static int exec_qadd_qsub(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     uint8_t op = (instr >> 21) & 0x3; /* 00=QADD 01=QSUB 10=QDADD 11=QDSUB */
     uint8_t rn = (instr >> 16) & 0xF;
     uint8_t rd = (instr >> 12) & 0xF;
     uint8_t rm = instr & 0xF;

     int32_t a = (int32_t)cpu->r[rm];
     int32_t b = (int32_t)cpu->r[rn];

     /* QDADD/QDSUB double Rn first, saturating */
     if (op & 2)
     {
          int64_t dbl = (int64_t)b * 2;
          if (dbl > (int64_t)0x7FFFFFFF)
          {
               dbl = 0x7FFFFFFF;
               cpu->cpsr |= GBA_CPSR_Q;
          }
          if (dbl < (int64_t)(-0x80000000LL))
          {
               dbl = -0x80000000LL;
               cpu->cpsr |= GBA_CPSR_Q;
          }
          b = (int32_t)dbl;
     }

     int64_t result = (op & 1) ? ((int64_t)a - b) : ((int64_t)a + b);
     if (result > (int64_t)0x7FFFFFFF)
     {
          result = 0x7FFFFFFF;
          cpu->cpsr |= GBA_CPSR_Q;
     }
     if (result < (int64_t)(-0x80000000LL))
     {
          result = -0x80000000LL;
          cpu->cpsr |= GBA_CPSR_Q;
     }

     cpu->r[rd] = (uint32_t)result;
     return 1;
}

/* SWP/SWPB */
static int exec_swp(struct gba *gba, uint32_t instr)
{
     struct gba_cpu *cpu = &gba->cpu;
     bool byte = (instr >> 22) & 1;
     uint8_t rn = (instr >> 16) & 0xF;
     uint8_t rd = (instr >> 12) & 0xF;
     uint8_t rm = instr & 0xF;

     uint32_t addr = cpu->r[rn];
     uint32_t old;
     if (byte)
     {
          old = gba_memory_read8(gba, addr);
          gba_memory_write8(gba, addr, cpu->r[rm] & 0xFF);
     }
     else
     {
          old = gba_memory_read32(gba, addr);
          gba_memory_write32(gba, addr, cpu->r[rm]);
     }
     cpu->r[rd] = old;
     return 4;
}

/* SWI */
static int exec_swi(struct gba *gba, uint32_t instr)
{
     uint32_t comment = instr & 0x00FFFFFF;
     if (gba_cpu_handle_swi(gba, comment))
          return 3;
     fprintf(stderr, "[SWI_UNKNOWN_ARM] swi=%02X pc=%08X\n",
             comment >> 16, gba->cpu.r[GBA_PC] - 8);
     gba_cpu_exception(gba, 0x00000008, GBA_MODE_SVC);
     return 3;
}

/*
 * Main ARM dispatch
 */

int gba_arm_execute(struct gba *gba, uint32_t instr)
{
     /* Bits 27-20 + 7-4 form the decode key */
     uint32_t hi = (instr >> 20) & 0xFF;
     uint32_t lo4 = (instr >> 4) & 0xF;

     /* Branch */
     if ((hi & 0xE0) == 0xA0)
          return exec_branch(gba, instr); /* B/BL */
     if (hi == 0x12 && lo4 == 1)
          return exec_bx(gba, instr); /* BX */
     if (hi == 0x12 && lo4 == 3)
          return exec_bx(gba, instr); /* BLX reg */
     /* CLZ: hi=0x16, lo4=0x1, bits[19:16]=0xF, bits[11:8]=0xF */
     if (hi == 0x16 && lo4 == 1 && ((instr >> 16) & 0xF) == 0xF && ((instr >> 8) & 0xF) == 0xF)
          return exec_clz(gba, instr);
     /* DSP multiplies — all share bits[7:4] with bit4=0 (lo4 even, bit3=1)
      * Must be checked before the halfword-transfer catch-all (bit4=1 there). */
     /* SMULWy / SMLAWy: hi=0x12, lo4=0x8/0xA/0xC/0xE */
     if (hi == 0x12 && (lo4 & 0x9) == 0x8)
          return exec_smulwy(gba, instr);
     /* SMULxy: hi=0x16, lo4=0x8..0xB */
     if (hi == 0x16 && (lo4 & 0x9) == 0x8)
          return exec_smulxy(gba, instr);
     /* SMLAxy: hi=0x10..0x13, lo4=0x8..0xB */
     if ((hi & 0xFC) == 0x10 && (lo4 & 0x9) == 0x8)
          return exec_smulxy(gba, instr);
     /* SMLALxy: hi=0x14..0x17, lo4=0x8..0xB */
     if ((hi & 0xFC) == 0x14 && (lo4 & 0x9) == 0x8)
          return exec_smlalxy(gba, instr);
     /* QADD/QSUB/QDADD/QDSUB: hi=0x10/0x12/0x14/0x16 (even), lo4=0x5 */
     if ((hi & 0xF9) == 0x10 && lo4 == 5)
          return exec_qadd_qsub(gba, instr);

     /* LDM/STM */
     if ((hi & 0xE0) == 0x80)
          return exec_ldm_stm(gba, instr);

     /* LDR/STR */
     if ((hi & 0xC0) == 0x40)
          return exec_ldr_str(gba, instr);

     /* Halfword/signed transfer: bits 27-25=000, bit7=1, bit4=1,
      * and bits 6-5 select H/SB/SH.  This must accept both register and
      * immediate offset forms; otherwise STRH is mistaken for data processing. */
     if ((instr & 0x0E000090U) == 0x00000090U && ((instr >> 5) & 0x3) != 0)
          return exec_ldr_str_h(gba, instr);

     /* Multiply */
     if ((hi & 0xFC) == 0 && lo4 == 9)
          return exec_mul(gba, instr);
     if ((hi & 0xF8) == 8 && lo4 == 9)
          return exec_mull(gba, instr);

     /* SWP */
     if ((hi & 0xFB) == 0x10 && lo4 == 9)
          return exec_swp(gba, instr);

     /* SWI */
     if ((hi & 0xF0) == 0xF0)
          return exec_swi(gba, instr);

     /* Data processing / PSR transfer */
     if ((hi & 0xC0) == 0)
          return exec_data_proc(gba, instr);

     /* Undefined / coprocessor → UND exception */
     gba_cpu_exception(gba, 0x00000004, GBA_MODE_UND);
     return 1;
}
