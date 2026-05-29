#include <string.h>
#include <stdio.h>
#include <math.h>
#include "gba.h"
#include "gba_disasm.h"

/* Forward declarations for instruction handlers */
int gba_arm_execute(struct gba *gba, uint32_t instr);
int gba_thumb_execute(struct gba *gba, uint16_t instr);

static int gba_no_bios_irq_entry_cycles(uint32_t intr_pc)
{
     if (intr_pc >= GBA_IWRAM_BASE && intr_pc < GBA_IWRAM_BASE + GBA_IWRAM_SIZE)
          return 23;
     if (intr_pc >= GBA_EWRAM_BASE && intr_pc < GBA_EWRAM_BASE + GBA_EWRAM_SIZE)
          return 43;
     if (intr_pc >= GBA_ROM_BASE && intr_pc < 0x0E000000u)
          return 45;
     return 23;
}

static int gba_bios_irq_entry_cycles(uint32_t intr_pc)
{
     if (intr_pc >= GBA_IWRAM_BASE && intr_pc < GBA_IWRAM_BASE + GBA_IWRAM_SIZE)
          return 8;
     if (intr_pc >= GBA_EWRAM_BASE && intr_pc < GBA_EWRAM_BASE + GBA_EWRAM_SIZE)
          return 28;
     if (intr_pc >= GBA_ROM_BASE && intr_pc < 0x0E000000u)
          return 24;
     return 8;
}

/* -------------------------------------------------------------------------
 * Mode switching / banked register save/restore
 * ---------------------------------------------------------------------- */

void gba_cpu_switch_mode(struct gba *gba, enum gba_cpu_mode new_mode)
{
     struct gba_cpu *cpu = &gba->cpu;
     enum gba_cpu_mode old_mode = (enum gba_cpu_mode)(cpu->cpsr & GBA_CPSR_M);

     if (old_mode == new_mode)
          return;

     /* Save current banked regs */
     switch (old_mode)
     {
     case GBA_MODE_USR:
     case GBA_MODE_SYS:
          cpu->r8_usr = cpu->r[8];
          cpu->r9_usr = cpu->r[9];
          cpu->r10_usr = cpu->r[10];
          cpu->r11_usr = cpu->r[11];
          cpu->r12_usr = cpu->r[12];
          cpu->r13_usr = cpu->r[13];
          cpu->r14_usr = cpu->r[14];
          break;
     case GBA_MODE_SVC:
          cpu->r8_usr = cpu->r[8];
          cpu->r9_usr = cpu->r[9];
          cpu->r10_usr = cpu->r[10];
          cpu->r11_usr = cpu->r[11];
          cpu->r12_usr = cpu->r[12];
          cpu->r13_svc = cpu->r[13];
          cpu->r14_svc = cpu->r[14];
          cpu->spsr_svc = cpu->spsr;
          break;
     case GBA_MODE_ABT:
          cpu->r8_usr = cpu->r[8];
          cpu->r9_usr = cpu->r[9];
          cpu->r10_usr = cpu->r[10];
          cpu->r11_usr = cpu->r[11];
          cpu->r12_usr = cpu->r[12];
          cpu->r13_abt = cpu->r[13];
          cpu->r14_abt = cpu->r[14];
          cpu->spsr_abt = cpu->spsr;
          break;
     case GBA_MODE_IRQ:
          cpu->r8_usr = cpu->r[8];
          cpu->r9_usr = cpu->r[9];
          cpu->r10_usr = cpu->r[10];
          cpu->r11_usr = cpu->r[11];
          cpu->r12_usr = cpu->r[12];
          cpu->r13_irq = cpu->r[13];
          cpu->r14_irq = cpu->r[14];
          cpu->spsr_irq = cpu->spsr;
          break;
     case GBA_MODE_UND:
          cpu->r8_usr = cpu->r[8];
          cpu->r9_usr = cpu->r[9];
          cpu->r10_usr = cpu->r[10];
          cpu->r11_usr = cpu->r[11];
          cpu->r12_usr = cpu->r[12];
          cpu->r13_und = cpu->r[13];
          cpu->r14_und = cpu->r[14];
          cpu->spsr_und = cpu->spsr;
          break;
     case GBA_MODE_FIQ:
          cpu->r8_fiq = cpu->r[8];
          cpu->r9_fiq = cpu->r[9];
          cpu->r10_fiq = cpu->r[10];
          cpu->r11_fiq = cpu->r[11];
          cpu->r12_fiq = cpu->r[12];
          cpu->r13_fiq = cpu->r[13];
          cpu->r14_fiq = cpu->r[14];
          cpu->spsr_fiq = cpu->spsr;
          break;
     default:
          break;
     }

     if (old_mode == GBA_MODE_FIQ && new_mode != GBA_MODE_FIQ)
     {
          cpu->r[8] = cpu->r8_usr;
          cpu->r[9] = cpu->r9_usr;
          cpu->r[10] = cpu->r10_usr;
          cpu->r[11] = cpu->r11_usr;
          cpu->r[12] = cpu->r12_usr;
     }

     /* Restore banked regs for new mode */
     switch (new_mode)
     {
     case GBA_MODE_USR:
     case GBA_MODE_SYS:
          cpu->r[13] = cpu->r13_usr;
          cpu->r[14] = cpu->r14_usr;
          break;
     case GBA_MODE_SVC:
          cpu->r[13] = cpu->r13_svc;
          cpu->r[14] = cpu->r14_svc;
          cpu->spsr = cpu->spsr_svc;
          break;
     case GBA_MODE_ABT:
          cpu->r[13] = cpu->r13_abt;
          cpu->r[14] = cpu->r14_abt;
          cpu->spsr = cpu->spsr_abt;
          break;
     case GBA_MODE_IRQ:
          cpu->r[13] = cpu->r13_irq;
          cpu->r[14] = cpu->r14_irq;
          cpu->spsr = cpu->spsr_irq;
          break;
     case GBA_MODE_UND:
          cpu->r[13] = cpu->r13_und;
          cpu->r[14] = cpu->r14_und;
          cpu->spsr = cpu->spsr_und;
          break;
     case GBA_MODE_FIQ:
          cpu->r[8] = cpu->r8_fiq;
          cpu->r[9] = cpu->r9_fiq;
          cpu->r[10] = cpu->r10_fiq;
          cpu->r[11] = cpu->r11_fiq;
          cpu->r[12] = cpu->r12_fiq;
          cpu->r[13] = cpu->r13_fiq;
          cpu->r[14] = cpu->r14_fiq;
          cpu->spsr = cpu->spsr_fiq;
          break;
     default:
          break;
     }

     cpu->cpsr = (cpu->cpsr & ~GBA_CPSR_M) | (uint32_t)new_mode;
}

void gba_cpu_set_cpsr(struct gba *gba, uint32_t new_cpsr)
{
     enum gba_cpu_mode new_mode = (enum gba_cpu_mode)(new_cpsr & GBA_CPSR_M);
     gba_cpu_switch_mode(gba, new_mode);
     gba->cpu.cpsr = new_cpsr;
}

uint32_t gba_cpu_get_spsr(struct gba *gba)
{
     return gba->cpu.spsr;
}

void gba_cpu_set_spsr(struct gba *gba, uint32_t val)
{
     gba->cpu.spsr = val;
}

/* -------------------------------------------------------------------------
 * Exception entry
 * ---------------------------------------------------------------------- */

void gba_cpu_exception(struct gba *gba, uint32_t vector, enum gba_cpu_mode mode)
{
     struct gba_cpu *cpu = &gba->cpu;
     uint32_t old_cpsr = cpu->cpsr;
     bool thumb = old_cpsr & GBA_CPSR_T;

     gba_cpu_switch_mode(gba, mode);

     cpu->spsr = old_cpsr;
     /*
      * At exception entry, r[15] already points to fetch_addr + 8 (ARM) or +4 (THUMB),
      * and has been advanced by +4/+2 for the current instruction.
      * LR = address of instruction after the one that caused the exception.
      *   ARM:   r[15] - 8 + 4 = r[15] - 4  (next instruction)
      *   THUMB: r[15] - 4 + 2 = r[15] - 2  (next instruction)
      * For IRQ the caller adds +4 on top of this to get the proper IRQ LR.
      */
     cpu->r[14] = thumb ? (cpu->r[15] - 2) : (cpu->r[15] - 4);

     /* Switch to ARM and mask IRQ while inside the exception handler. */
     cpu->cpsr &= ~GBA_CPSR_T;
     cpu->cpsr |= GBA_CPSR_I;
     if (mode == GBA_MODE_FIQ)
          cpu->cpsr |= GBA_CPSR_F;

     /* r[15] must satisfy invariant: r[15] = dest + 8 (always ARM after exception) */
     cpu->r[15] = vector + 8;
     cpu->pipeline_valid = false;
}

void gba_cpu_trigger_irq(struct gba *gba)
{
     gba->irq.force = false;
     if (!gba->bios)
     {
          struct gba_cpu *cpu = &gba->cpu;
          uint32_t handler = gba_memory_read32(gba, 0x03007FFC);
          if (handler == 0)
               return;

          bool thumb = (cpu->cpsr & GBA_CPSR_T) != 0;
          /*
           * No-BIOS IRQ HLE: enter IRQ mode and jump to the handler installed at
           * 03007FFC.  libgba installs IntrMain there; it expects IRQ mode and
           * performs its own SYS-mode switch around the game's callback.  We set
           * LR_irq so the final handler return lands on a marker where the HLE
           * restores the pre-IRQ registers/CPSR.
           */

          /* PC of interrupted instruction */
          uint32_t intr_pc = cpu->r[15] - (thumb ? 4u : 8u);

          /*
           * libgba's copied IntrMain returns with BX lr after restoring SPSR_irq.
           * Encode the interrupted state in LR bit 0 so BX lands back in the
           * correct ARM/THUMB pipeline invariant.
           */
          uint32_t hle_lr = thumb ? (intr_pc | 1u) : (intr_pc & ~3u);

          /* Save full context for HLE restore */
          gba->bios_irq_hle_active = true;
          gba->bios_irq_hle_return_r15 = intr_pc + (thumb ? 4u : 8u);
          gba->bios_irq_hle_regs[0] = cpu->r[0];
          gba->bios_irq_hle_regs[1] = cpu->r[1];
          gba->bios_irq_hle_regs[2] = cpu->r[2];
          gba->bios_irq_hle_regs[3] = cpu->r[3];
          gba->bios_irq_hle_regs[4] = cpu->r[12];
          gba->bios_irq_hle_cpsr = cpu->cpsr;

          uint32_t saved_cpsr = cpu->cpsr;

          /* Step 1: Enter IRQ mode */
          gba_cpu_switch_mode(gba, GBA_MODE_IRQ);
          cpu->spsr = saved_cpsr;   /* SPSR_irq */
          cpu->r[14] = hle_lr;      /* LR_irq for no-BIOS HLE return marker */
          cpu->cpsr = (cpu->cpsr & ~(GBA_CPSR_T | GBA_CPSR_M)) |
                      (uint32_t)GBA_MODE_IRQ | GBA_CPSR_I;

          /* Step 2: Jump to handler in ARM mode */
          cpu->cpsr &= ~GBA_CPSR_T;
          cpu->r[15] = (handler & ~3U) + 8;
          cpu->pipeline_valid = false;

          gba->timestamp += gba_no_bios_irq_entry_cycles(intr_pc);

          gba->bios_open_bus = 0xE25EF004;
          gba->bios_open_bus_after_read = 0xE55EC002;
          gba->bios_open_bus_has_after_read = true;
          return;
     }

     /*
      * IRQ LR = address of next instruction + 4 (so SUBS PC,LR,#4 returns correctly).
      * gba_cpu_exception sets LR = next_instr, so we add 4 here.
      */
     bool thumb = (gba->cpu.cpsr & GBA_CPSR_T) != 0;
     uint32_t intr_pc = gba->cpu.r[15] - (thumb ? 4u : 8u);
     gba_cpu_exception(gba, 0x00000018, GBA_MODE_IRQ);
     gba->cpu.r[14] += 4;
     gba->timestamp += gba_bios_irq_entry_cycles(intr_pc);
}

/* -------------------------------------------------------------------------
 * BIOS HLE sine table (used by BgAffineSet / ObjAffineSet)
 * ---------------------------------------------------------------------- */
static const int16_t bios_sine_table[256] = {
    (int16_t)0x0000, (int16_t)0x0192, (int16_t)0x0323, (int16_t)0x04B5,
    (int16_t)0x0645, (int16_t)0x07D5, (int16_t)0x0964, (int16_t)0x0AF1,
    (int16_t)0x0C7C, (int16_t)0x0E05, (int16_t)0x0F8C, (int16_t)0x1111,
    (int16_t)0x1294, (int16_t)0x1413, (int16_t)0x158F, (int16_t)0x1708,
    (int16_t)0x187D, (int16_t)0x19EF, (int16_t)0x1B5D, (int16_t)0x1CC6,
    (int16_t)0x1E2B, (int16_t)0x1F8B, (int16_t)0x20E7, (int16_t)0x223D,
    (int16_t)0x238E, (int16_t)0x24DA, (int16_t)0x261F, (int16_t)0x275F,
    (int16_t)0x2899, (int16_t)0x29CD, (int16_t)0x2AFA, (int16_t)0x2C21,
    (int16_t)0x2D41, (int16_t)0x2E5A, (int16_t)0x2F6B, (int16_t)0x3076,
    (int16_t)0x3179, (int16_t)0x3274, (int16_t)0x3367, (int16_t)0x3453,
    (int16_t)0x3536, (int16_t)0x3612, (int16_t)0x36E5, (int16_t)0x37AF,
    (int16_t)0x3871, (int16_t)0x392A, (int16_t)0x39DA, (int16_t)0x3A82,
    (int16_t)0x3B20, (int16_t)0x3BB6, (int16_t)0x3C42, (int16_t)0x3CC5,
    (int16_t)0x3D3E, (int16_t)0x3DAE, (int16_t)0x3E14, (int16_t)0x3E71,
    (int16_t)0x3EC5, (int16_t)0x3F0E, (int16_t)0x3F4E, (int16_t)0x3F84,
    (int16_t)0x3FB1, (int16_t)0x3FD3, (int16_t)0x3FEC, (int16_t)0x3FFB,
    (int16_t)0x4000, (int16_t)0x3FFB, (int16_t)0x3FEC, (int16_t)0x3FD3,
    (int16_t)0x3FB1, (int16_t)0x3F84, (int16_t)0x3F4E, (int16_t)0x3F0E,
    (int16_t)0x3EC5, (int16_t)0x3E71, (int16_t)0x3E14, (int16_t)0x3DAE,
    (int16_t)0x3D3E, (int16_t)0x3CC5, (int16_t)0x3C42, (int16_t)0x3BB6,
    (int16_t)0x3B20, (int16_t)0x3A82, (int16_t)0x39DA, (int16_t)0x392A,
    (int16_t)0x3871, (int16_t)0x37AF, (int16_t)0x36E5, (int16_t)0x3612,
    (int16_t)0x3536, (int16_t)0x3453, (int16_t)0x3367, (int16_t)0x3274,
    (int16_t)0x3179, (int16_t)0x3076, (int16_t)0x2F6B, (int16_t)0x2E5A,
    (int16_t)0x2D41, (int16_t)0x2C21, (int16_t)0x2AFA, (int16_t)0x29CD,
    (int16_t)0x2899, (int16_t)0x275F, (int16_t)0x261F, (int16_t)0x24DA,
    (int16_t)0x238E, (int16_t)0x223D, (int16_t)0x20E7, (int16_t)0x1F8B,
    (int16_t)0x1E2B, (int16_t)0x1CC6, (int16_t)0x1B5D, (int16_t)0x19EF,
    (int16_t)0x187D, (int16_t)0x1708, (int16_t)0x158F, (int16_t)0x1413,
    (int16_t)0x1294, (int16_t)0x1111, (int16_t)0x0F8C, (int16_t)0x0E05,
    (int16_t)0x0C7C, (int16_t)0x0AF1, (int16_t)0x0964, (int16_t)0x07D5,
    (int16_t)0x0645, (int16_t)0x04B5, (int16_t)0x0323, (int16_t)0x0192,
    (int16_t)0x0000, (int16_t)0xFE6E, (int16_t)0xFCDD, (int16_t)0xFB4B,
    (int16_t)0xF9BB, (int16_t)0xF82B, (int16_t)0xF69C, (int16_t)0xF50F,
    (int16_t)0xF384, (int16_t)0xF1FB, (int16_t)0xF074, (int16_t)0xEEEF,
    (int16_t)0xED6C, (int16_t)0xEBED, (int16_t)0xEA71, (int16_t)0xE8F8,
    (int16_t)0xE783, (int16_t)0xE611, (int16_t)0xE4A3, (int16_t)0xE33A,
    (int16_t)0xE1D5, (int16_t)0xE075, (int16_t)0xDF19, (int16_t)0xDDC3,
    (int16_t)0xDC72, (int16_t)0xDB26, (int16_t)0xD9E1, (int16_t)0xD8A1,
    (int16_t)0xD767, (int16_t)0xD633, (int16_t)0xD506, (int16_t)0xD3DF,
    (int16_t)0xD2BF, (int16_t)0xD1A6, (int16_t)0xD095, (int16_t)0xCF8A,
    (int16_t)0xCE87, (int16_t)0xCD8C, (int16_t)0xCC99, (int16_t)0xCBAD,
    (int16_t)0xCACA, (int16_t)0xC9EE, (int16_t)0xC91B, (int16_t)0xC851,
    (int16_t)0xC78F, (int16_t)0xC6D6, (int16_t)0xC626, (int16_t)0xC57E,
    (int16_t)0xC4E0, (int16_t)0xC44A, (int16_t)0xC3BE, (int16_t)0xC33B,
    (int16_t)0xC2C2, (int16_t)0xC252, (int16_t)0xC1EC, (int16_t)0xC18F,
    (int16_t)0xC13B, (int16_t)0xC0F2, (int16_t)0xC0B2, (int16_t)0xC07C,
    (int16_t)0xC04F, (int16_t)0xC02D, (int16_t)0xC014, (int16_t)0xC005,
    (int16_t)0xC000, (int16_t)0xC005, (int16_t)0xC014, (int16_t)0xC02D,
    (int16_t)0xC04F, (int16_t)0xC07C, (int16_t)0xC0B2, (int16_t)0xC0F2,
    (int16_t)0xC13B, (int16_t)0xC18F, (int16_t)0xC1EC, (int16_t)0xC252,
    (int16_t)0xC2C2, (int16_t)0xC33B, (int16_t)0xC3BE, (int16_t)0xC44A,
    (int16_t)0xC4E0, (int16_t)0xC57E, (int16_t)0xC626, (int16_t)0xC6D6,
    (int16_t)0xC78F, (int16_t)0xC851, (int16_t)0xC91B, (int16_t)0xC9EE,
    (int16_t)0xCACA, (int16_t)0xCBAD, (int16_t)0xCC99, (int16_t)0xCD8C,
    (int16_t)0xCE87, (int16_t)0xCF8A, (int16_t)0xD095, (int16_t)0xD1A6,
    (int16_t)0xD2BF, (int16_t)0xD3DF, (int16_t)0xD506, (int16_t)0xD633,
    (int16_t)0xD767, (int16_t)0xD8A1, (int16_t)0xD9E1, (int16_t)0xDB26,
    (int16_t)0xDC72, (int16_t)0xDDC3, (int16_t)0xDF19, (int16_t)0xE075,
    (int16_t)0xE1D5, (int16_t)0xE33A, (int16_t)0xE4A3, (int16_t)0xE611,
    (int16_t)0xE783, (int16_t)0xE8F8, (int16_t)0xEA71, (int16_t)0xEBED,
    (int16_t)0xED6C, (int16_t)0xEEEF, (int16_t)0xF074, (int16_t)0xF1FB,
    (int16_t)0xF384, (int16_t)0xF50F, (int16_t)0xF69C, (int16_t)0xF82B,
    (int16_t)0xF9BB, (int16_t)0xFB4B, (int16_t)0xFCDD, (int16_t)0xFE6E};

/* -------------------------------------------------------------------------
 * BIOS HLE helper: ArcTan (used internally by ArcTan2)
 * ---------------------------------------------------------------------- */
static void bios_arctan(struct gba_cpu *cpu)
{
     int32_t i = (int32_t)cpu->r[0];
     int32_t a = -((i * i) >> 14);
     int32_t b = ((0xA9 * a) >> 14) + 0x390;
     b = ((b * a) >> 14) + 0x91C;
     b = ((b * a) >> 14) + 0xFB6;
     b = ((b * a) >> 14) + 0x16AA;
     b = ((b * a) >> 14) + 0x2081;
     b = ((b * a) >> 14) + 0x3651;
     b = ((b * a) >> 14) + 0xA2F9;
     cpu->r[0] = (uint32_t)((i * b) >> 16);
     cpu->r[1] = (uint32_t)a;
     cpu->r[3] = (uint32_t)b;
}

/* -------------------------------------------------------------------------
 * BIOS HLE — SWI dispatcher
 * ---------------------------------------------------------------------- */
bool gba_cpu_handle_swi(struct gba *gba, uint32_t comment)
{
     struct gba_cpu *cpu = &gba->cpu;
     uint32_t num = comment > 0xFF ? ((comment >> 16) & 0xFF) : (comment & 0xFF);

     if (gba->bios && gba_cpu_current_pc(cpu) < GBA_BIOS_SIZE)
          return false;

     switch (num)
     {

     /* ---- 0x00 SoftReset ---- */
     case 0x00:
     {
          /* Reset stacks / registers and jump to ROM or EWRAM depending on flag */
          uint8_t dest_flag = gba_memory_read8(gba, 0x03007FFA);
          memset(&gba->iwram[0x7E00], 0, 0x200);
          cpu->cpsr = (uint32_t)GBA_MODE_SYS;
          cpu->r[13] = 0x03007F00;
          cpu->r[14] = 0x00000000;
          cpu->r13_irq = 0x03007FA0;
          cpu->r14_irq = 0x00000000;
          cpu->spsr_irq = 0x00000000;
          cpu->r13_svc = 0x03007FE0;
          cpu->r14_svc = 0x00000000;
          cpu->spsr_svc = 0x00000000;
          uint32_t dest = dest_flag ? 0x02000000 : 0x08000000;
          cpu->r[15] = dest + 8;
          cpu->pipeline_valid = false;
          return true;
     }

     /* ---- 0x01 RegisterRamReset ---- */
     case 0x01:
     {
          uint32_t flags = cpu->r[0];
          /* Clear DISPCNT forced-blank so screen comes back. */
          uint16_t dispcnt = gba_memory_read16(gba, REG_DISPCNT);
          gba_memory_write16(gba, REG_DISPCNT, (uint16_t)(dispcnt & ~0x0080u));
          if (flags & 0x01)
               memset(gba->ewram, 0, GBA_EWRAM_SIZE);
          if (flags & 0x02)
               memset(gba->iwram, 0, 0x7E00);
          if (flags & 0x04)
               memset(gba->pram, 0, GBA_PAL_SIZE);
          if (flags & 0x08)
               memset(gba->vram, 0, GBA_VRAM_SIZE);
          if (flags & 0x10)
               memset(gba->oam, 0, GBA_OAM_SIZE);
          if (flags & 0x80)
          {
               /* Reset most I/O registers */
               for (int i = 0; i < 0x10; i++)
                    gba_memory_write16(gba, 0x04000200 + (uint32_t)(i * 2), 0);
               for (int i = 0; i < 0x0F; i++)
                    gba_memory_write16(gba, 0x04000004 + (uint32_t)(i * 2), 0);
               for (int i = 0; i < 0x20; i++)
                    gba_memory_write16(gba, 0x04000020 + (uint32_t)(i * 2), 0);
               for (int i = 0; i < 0x18; i++)
                    gba_memory_write16(gba, 0x040000B0 + (uint32_t)(i * 2), 0);
               gba_memory_write16(gba, 0x04000130, 0);
               gba_memory_write16(gba, 0x04000020, 0x0100);
               gba_memory_write16(gba, 0x04000030, 0x0100);
               gba_memory_write16(gba, 0x04000026, 0x0100);
               gba_memory_write16(gba, 0x04000036, 0x0100);
          }
          if (flags & 0x20)
          {
               for (int i = 0; i < 8; i++)
                    gba_memory_write16(gba, 0x04000110 + (uint32_t)(i * 2), 0);
               gba_memory_write16(gba, 0x04000134, 0x8000);
               for (int i = 0; i < 7; i++)
                    gba_memory_write16(gba, 0x04000140 + (uint32_t)(i * 2), 0);
          }
          if (flags & 0x40)
          {
               gba_memory_write8(gba, 0x04000084, 0);
               gba_memory_write8(gba, 0x04000084, 0x80);
               gba_memory_write32(gba, 0x04000080, 0x880E0000);
               gba_memory_write8(gba, 0x04000070, 0x70);
               for (int i = 0; i < 8; i++)
                    gba_memory_write16(gba, 0x04000090 + (uint32_t)(i * 2), 0);
               gba_memory_write8(gba, 0x04000070, 0);
               for (int i = 0; i < 8; i++)
                    gba_memory_write16(gba, 0x04000090 + (uint32_t)(i * 2), 0);
               gba_memory_write8(gba, 0x04000084, 0);
          }
          return true;
     }

     /* ---- 0x02 Halt ---- */
     case 0x02:
          cpu->halted = true;
          gba->halt_mode = 1;
          return true;

     /* ---- 0x03 Stop ---- */
     case 0x03:
          cpu->halted = true;
          gba->halt_mode = 2;
          return true;

     /* ---- 0x04 IntrWait ---- */
     case 0x04:
     {
          uint32_t discard = cpu->r[0];
          uint32_t mask = cpu->r[1];
          if (discard)
               gba->irq.if_ &= ~(uint16_t)mask;
          gba->bios_intr_wait_active = true;
          gba->bios_intr_wait_mask = (uint16_t)mask;
          cpu->halted = true;
          gba->halt_mode = 1;
          return true;
     }

     /* ---- 0x05 VBlankIntrWait ---- */
     case 0x05:
          cpu->r[0] = 1;
          cpu->r[1] = 1; /* VBLANK flag */
          gba->irq.if_ &= ~0x0001u;
          gba->bios_intr_wait_active = true;
          gba->bios_intr_wait_mask = 0x0001u;
          cpu->halted = true;
          gba->halt_mode = 1;
          return true;

     /* ---- 0x06 Div ---- */
     case 0x06:
     {
          int32_t numer = (int32_t)cpu->r[0];
          int32_t denom = (int32_t)cpu->r[1];
          if (denom == 0)
          {
               cpu->r[0] = (numer < 0) ? 0xFFFFFFFFu : 1u;
               cpu->r[1] = (uint32_t)numer;
               cpu->r[3] = cpu->r[0];
          }
          else
          {
               int32_t quot = numer / denom;
               int32_t rem = numer % denom;
               cpu->r[0] = (uint32_t)quot;
               cpu->r[1] = (uint32_t)rem;
               cpu->r[3] = (uint32_t)(quot < 0 ? -quot : quot);
          }
          return true;
     }

     /* ---- 0x07 DivARM (swapped args) ---- */
     case 0x07:
     {
          uint32_t tmp = cpu->r[0];
          cpu->r[0] = cpu->r[1];
          cpu->r[1] = tmp;
          /* recurse via Div */
          int32_t numer = (int32_t)cpu->r[0];
          int32_t denom = (int32_t)cpu->r[1];
          if (denom == 0)
          {
               cpu->r[0] = (numer < 0) ? 0xFFFFFFFFu : 1u;
               cpu->r[1] = (uint32_t)numer;
               cpu->r[3] = cpu->r[0];
          }
          else
          {
               int32_t quot = numer / denom;
               int32_t rem = numer % denom;
               cpu->r[0] = (uint32_t)quot;
               cpu->r[1] = (uint32_t)rem;
               cpu->r[3] = (uint32_t)(quot < 0 ? -quot : quot);
          }
          return true;
     }

     /* ---- 0x08 Sqrt ---- */
     case 0x08:
     {
          uint32_t n = cpu->r[0];
          uint32_t bit = 1u << 30;
          uint32_t root = 0;
          while (bit > n)
               bit >>= 2;
          while (bit != 0)
          {
               if (n >= root + bit)
               {
                    n -= root + bit;
                    root = (root >> 1) + bit;
               }
               else
               {
                    root >>= 1;
               }
               bit >>= 2;
          }
          cpu->r[0] = root;
          gba->bios_open_bus = 0xE3A02004;
          return true;
     }

     /* ---- 0x09 ArcTan ---- */
     case 0x09:
          bios_arctan(cpu);
          return true;

     /* ---- 0x0A ArcTan2 ---- */
     case 0x0A:
     {
          int32_t x = (int32_t)cpu->r[0];
          int32_t y = (int32_t)cpu->r[1];
          int32_t res = 0;
          if (y == 0)
          {
               res = (int32_t)((x >> 16) & 0x8000);
          }
          else if (x == 0)
          {
               res = (int32_t)(((y >> 16) & 0x8000) + 0x4000);
          }
          else
          {
               int ax = x < 0 ? -x : x;
               int ay = y < 0 ? -y : y;
               if ((ax > ay) || ((ax == ay) && !((x < 0) && (y < 0))))
               {
                    cpu->r[1] = (uint32_t)x;
                    cpu->r[0] = (uint32_t)(y << 14);
                    /* inline Div */
                    {
                         int32_t q = (int32_t)cpu->r[0] / (int32_t)cpu->r[1];
                         cpu->r[0] = (uint32_t)q;
                    }
                    bios_arctan(cpu);
                    if (x < 0)
                         res = (int32_t)(0x8000 + (int32_t)cpu->r[0]);
                    else
                         res = (int32_t)((((y >> 16) & 0x8000) << 1) + (int32_t)cpu->r[0]);
               }
               else
               {
                    cpu->r[0] = (uint32_t)(x << 14);
                    /* inline Div */
                    {
                         int32_t q = (int32_t)cpu->r[0] / y;
                         cpu->r[0] = (uint32_t)q;
                    }
                    bios_arctan(cpu);
                    res = (int32_t)((0x4000 + ((y >> 16) & 0x8000)) - (int32_t)cpu->r[0]);
               }
          }
          cpu->r[0] = (uint32_t)res;
          cpu->r[3] = 0x170;
          return true;
     }

     /* ---- 0x0B CpuSet ---- */
     case 0x0B:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t cnt = cpu->r[2];
          uint32_t count = cnt & 0x1FFFFF;
          bool is32 = (cnt >> 26) & 1;
          bool fill = (cnt >> 24) & 1;
          uint32_t aligned_dst = is32 ? (dst & ~3u) : (dst & ~1u);

          if (!is32 && count == 1 && aligned_dst == REG_POSTFLG)
          {
               if (gba->irq.ie & gba->irq.if_)
                    gba->halt_resume_cycles =
                         ((cpu->r[15] >> 24) == 0x03) ? 107 : 179;
               else
                    gba->halt_resume_cycles = 50;
          }
          else if (!is32 && count == 2 && aligned_dst == REG_DMA0CNT_L)
               gba->halt_resume_cycles = 49;

          if (is32)
          {
               src &= ~3u;
               dst &= ~3u;
               uint32_t val = fill ? gba_memory_read32(gba, src) : 0;
               for (uint32_t i = 0; i < count; i++)
               {
                    if (!fill)
                         val = gba_memory_read32(gba, src);
                    src += 4;
                    gba_memory_write32(gba, dst, val);
                    dst += 4;
               }
          }
          else
          {
               uint16_t val = fill ? gba_memory_read16(gba, src) : 0;
               for (uint32_t i = 0; i < count; i++)
               {
                    if (!fill)
                         val = gba_memory_read16(gba, src);
                    src += 2;
                    gba_memory_write16(gba, dst, val);
                    dst += 2;
               }
          }
          return true;
     }

     /* ---- 0x0C CpuFastSet ---- */
     case 0x0C:
     {
          uint32_t src = cpu->r[0] & ~3u;
          uint32_t dst = cpu->r[1] & ~3u;
          uint32_t cnt = cpu->r[2];
          uint32_t count = cnt & 0x1FFFFF;
          bool fill = (cnt >> 24) & 1;
          /* transfers in blocks of 8 words */
          count = (count + 7u) & ~7u;
          uint32_t val = fill ? gba_memory_read32(gba, src) : 0;
          for (uint32_t i = 0; i < count; i++)
          {
               if (!fill)
               {
                    val = gba_memory_read32(gba, src);
                    src += 4;
               }
               gba_memory_write32(gba, dst, val);
               dst += 4;
          }
          return true;
     }

     /* ---- 0x0D GetBiosChecksum ---- */
     case 0x0D:
          cpu->r[0] = 0xBAAE187F;
          return true;

     /* ---- 0x0E BgAffineSet ---- */
     case 0x0E:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          int32_t num = (int32_t)cpu->r[2];
          for (int32_t i = 0; i < num; i++)
          {
               int32_t cx = (int32_t)gba_memory_read32(gba, src);
               src += 4;
               int32_t cy = (int32_t)gba_memory_read32(gba, src);
               src += 4;
               int16_t dispx = (int16_t)gba_memory_read16(gba, src);
               src += 2;
               int16_t dispy = (int16_t)gba_memory_read16(gba, src);
               src += 2;
               int16_t rx = (int16_t)gba_memory_read16(gba, src);
               src += 2;
               int16_t ry = (int16_t)gba_memory_read16(gba, src);
               src += 2;
               uint16_t theta = (uint16_t)(gba_memory_read16(gba, src) >> 8);
               src += 4;
               int32_t a = bios_sine_table[(theta + 0x40) & 0xFF];
               int32_t b = bios_sine_table[theta & 0xFF];
               int16_t dx = (int16_t)((rx * a) >> 14);
               int16_t dmx = (int16_t)((rx * b) >> 14);
               int16_t dy = (int16_t)((ry * b) >> 14);
               int16_t dmy = (int16_t)((ry * a) >> 14);
               gba_memory_write16(gba, dst, (uint16_t)dx);
               dst += 2;
               gba_memory_write16(gba, dst, (uint16_t)-dmx);
               dst += 2;
               gba_memory_write16(gba, dst, (uint16_t)dy);
               dst += 2;
               gba_memory_write16(gba, dst, (uint16_t)dmy);
               dst += 2;
               int32_t startx = cx - (int32_t)dx * dispx + (int32_t)dmx * dispy;
               int32_t starty = cy - (int32_t)dy * dispx - (int32_t)dmy * dispy;
               gba_memory_write32(gba, dst, (uint32_t)startx);
               dst += 4;
               gba_memory_write32(gba, dst, (uint32_t)starty);
               dst += 4;
          }
          return true;
     }

     /* ---- 0x0F ObjAffineSet ---- */
     case 0x0F:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          int32_t num = (int32_t)cpu->r[2];
          uint32_t offset = cpu->r[3];
          for (int32_t i = 0; i < num; i++)
          {
               int16_t rx = (int16_t)gba_memory_read16(gba, src);
               src += 2;
               int16_t ry = (int16_t)gba_memory_read16(gba, src);
               src += 2;
               uint16_t theta = (uint16_t)(gba_memory_read16(gba, src) >> 8);
               src += 4;
               int32_t a = bios_sine_table[(theta + 0x40) & 0xFF];
               int32_t b = bios_sine_table[theta & 0xFF];
               gba_memory_write16(gba, dst, (uint16_t)((int32_t)(rx * a) >> 14));
               dst += offset;
               gba_memory_write16(gba, dst, (uint16_t)(-(int16_t)((int32_t)(rx * b) >> 14)));
               dst += offset;
               gba_memory_write16(gba, dst, (uint16_t)((int32_t)(ry * b) >> 14));
               dst += offset;
               gba_memory_write16(gba, dst, (uint16_t)((int32_t)(ry * a) >> 14));
               dst += offset;
          }
          return true;
     }

     /* ---- 0x10 BitUnPack ---- */
     case 0x10:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t header = cpu->r[2];
          int len = (int)(int16_t)gba_memory_read16(gba, header);
          int bits = gba_memory_read8(gba, header + 2);
          int dstbits = gba_memory_read8(gba, header + 3);
          uint32_t base = gba_memory_read32(gba, header + 4);
          bool addbase = (base & 0x80000000) != 0;
          base &= 0x7FFFFFFF;
          int revbits = 8 - bits;
          uint32_t data = 0;
          int bitwritecount = 0;
          while (len > 0)
          {
               int mask = 0xFF >> revbits;
               uint8_t b = gba_memory_read8(gba, src++);
               int bitcount = 0;
               while (bitcount < 8)
               {
                    uint32_t d = (uint32_t)(b & mask);
                    uint32_t val = d >> bitcount;
                    if (d || addbase)
                         val += base;
                    data |= val << bitwritecount;
                    bitwritecount += dstbits;
                    if (bitwritecount >= 32)
                    {
                         gba_memory_write32(gba, dst, data);
                         dst += 4;
                         data = 0;
                         bitwritecount = 0;
                    }
                    mask <<= bits;
                    bitcount += bits;
               }
               len--;
          }
          return true;
     }

     /* ---- 0x11 LZ77UnCompWram ---- */
     case 0x11:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t hdr = gba_memory_read32(gba, src);
          src += 4;
          int len = (int)(hdr >> 8);
          while (len > 0)
          {
               uint8_t flags = gba_memory_read8(gba, src++);
               for (int i = 0; i < 8 && len > 0; i++, flags <<= 1)
               {
                    if (flags & 0x80)
                    {
                         uint16_t info = (uint16_t)(gba_memory_read8(gba, src++) << 8);
                         info |= gba_memory_read8(gba, src++);
                         int length = (info >> 12) + 3;
                         int offset = (info & 0x0FFF);
                         uint32_t wofs = dst - (uint32_t)offset - 1;
                         for (int j = 0; j < length && len > 0; j++, len--)
                              gba_memory_write8(gba, dst++, gba_memory_read8(gba, wofs++));
                    }
                    else
                    {
                         gba_memory_write8(gba, dst++, gba_memory_read8(gba, src++));
                         len--;
                    }
               }
          }
          cpu->r[0] = src;
          cpu->r[1] = dst;
          cpu->r[3] = 0;
          return true;
     }

     /* ---- 0x12 LZ77UnCompVram (halfword writes) ---- */
     case 0x12:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t hdr = gba_memory_read32(gba, src);
          src += 4;
          int len = (int)(hdr >> 8);
          int bytecount = 0, byteshift = 0;
          uint32_t writeval = 0;
          while (len > 0)
          {
               uint8_t flags = gba_memory_read8(gba, src++);
               for (int i = 0; i < 8 && len > 0; i++, flags <<= 1)
               {
                    if (flags & 0x80)
                    {
                         uint16_t info = (uint16_t)(gba_memory_read8(gba, src++) << 8);
                         info |= gba_memory_read8(gba, src++);
                         int length = (info >> 12) + 3;
                         int offset = (info & 0x0FFF);
                         uint32_t wofs = dst + (uint32_t)bytecount - (uint32_t)offset - 1;
                         for (int j = 0; j < length && len > 0; j++, len--)
                         {
                              writeval |= (uint32_t)(gba_memory_read8(gba, wofs++) << byteshift);
                              byteshift += 8;
                              bytecount++;
                              if (bytecount == 2)
                              {
                                   gba_memory_write16(gba, dst, (uint16_t)writeval);
                                   dst += 2;
                                   bytecount = 0;
                                   byteshift = 0;
                                   writeval = 0;
                              }
                         }
                    }
                    else
                    {
                         writeval |= (uint32_t)(gba_memory_read8(gba, src++) << byteshift);
                         byteshift += 8;
                         bytecount++;
                         len--;
                         if (bytecount == 2)
                         {
                              gba_memory_write16(gba, dst, (uint16_t)writeval);
                              dst += 2;
                              bytecount = 0;
                              byteshift = 0;
                              writeval = 0;
                         }
                    }
               }
          }
          cpu->r[0] = src;
          cpu->r[1] = dst;
          cpu->r[3] = 0;
          return true;
     }

     /* ---- 0x13 HuffUnComp ---- */
     case 0x13:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t hdr = gba_memory_read32(gba, src);
          src += 4;
          uint8_t treesize = gba_memory_read8(gba, src++);
          uint32_t treestart = src;
          src += ((uint32_t)(treesize + 1) << 1) - 1;
          int len = (int)(hdr >> 8);
          int bitdepth = (int)(hdr & 0x0F); /* 4 or 8 */
          uint32_t mask = 0x80000000u;
          uint32_t bitdata = gba_memory_read32(gba, src);
          src += 4;
          uint8_t rootnode = gba_memory_read8(gba, treestart);
          uint8_t curnode = rootnode;
          int pos = 0;
          bool writedata = false;
          int byteshift = 0, bytecount = 0;
          uint32_t writeval = 0;
          if (bitdepth == 8)
          {
               while (len > 0)
               {
                    if (pos == 0)
                         pos++;
                    else
                         pos += (int)((((uint32_t)curnode & 0x3F) + 1) << 1);
                    if (bitdata & mask)
                    {
                         if (curnode & 0x40)
                              writedata = true;
                         curnode = gba_memory_read8(gba, treestart + (uint32_t)pos + 1);
                    }
                    else
                    {
                         if (curnode & 0x80)
                              writedata = true;
                         curnode = gba_memory_read8(gba, treestart + (uint32_t)pos);
                    }
                    if (writedata)
                    {
                         writeval |= (uint32_t)curnode << byteshift;
                         bytecount++;
                         byteshift += 8;
                         pos = 0;
                         curnode = rootnode;
                         writedata = false;
                         if (bytecount == 4)
                         {
                              gba_memory_write32(gba, dst, writeval);
                              dst += 4;
                              writeval = 0;
                              bytecount = 0;
                              byteshift = 0;
                              len -= 4;
                         }
                    }
                    mask >>= 1;
                    if (mask == 0)
                    {
                         mask = 0x80000000u;
                         bitdata = gba_memory_read32(gba, src);
                         src += 4;
                    }
               }
          }
          else
          { /* 4-bit */
               int halflen = 0, value = 0;
               while (len > 0)
               {
                    if (pos == 0)
                         pos++;
                    else
                         pos += (int)((((uint32_t)curnode & 0x3F) + 1) << 1);
                    if (bitdata & mask)
                    {
                         if (curnode & 0x40)
                              writedata = true;
                         curnode = gba_memory_read8(gba, treestart + (uint32_t)pos + 1);
                    }
                    else
                    {
                         if (curnode & 0x80)
                              writedata = true;
                         curnode = gba_memory_read8(gba, treestart + (uint32_t)pos);
                    }
                    if (writedata)
                    {
                         if (halflen == 0)
                              value |= curnode;
                         else
                              value |= (curnode << 4);
                         halflen += 4;
                         if (halflen == 8)
                         {
                              writeval |= (uint32_t)value << byteshift;
                              bytecount++;
                              byteshift += 8;
                              halflen = 0;
                              value = 0;
                              if (bytecount == 4)
                              {
                                   gba_memory_write32(gba, dst, writeval);
                                   dst += 4;
                                   writeval = 0;
                                   bytecount = 0;
                                   byteshift = 0;
                                   len -= 4;
                              }
                         }
                         pos = 0;
                         curnode = rootnode;
                         writedata = false;
                    }
                    mask >>= 1;
                    if (mask == 0)
                    {
                         mask = 0x80000000u;
                         bitdata = gba_memory_read32(gba, src);
                         src += 4;
                    }
               }
          }
          return true;
     }

     /* ---- 0x14 RLUnCompWram ---- */
     case 0x14:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t hdr = gba_memory_read32(gba, src & ~3u);
          src += 4;
          int len = (int)(hdr >> 8);
          while (len > 0)
          {
               uint8_t d = gba_memory_read8(gba, src++);
               int l = (int)(d & 0x7F);
               if (d & 0x80)
               {
                    uint8_t data = gba_memory_read8(gba, src++);
                    l += 3;
                    for (int j = 0; j < l && len > 0; j++, len--)
                         gba_memory_write8(gba, dst++, data);
               }
               else
               {
                    l++;
                    for (int j = 0; j < l && len > 0; j++, len--)
                         gba_memory_write8(gba, dst++, gba_memory_read8(gba, src++));
               }
          }
          return true;
     }

     /* ---- 0x15 RLUnCompVram (halfword writes) ---- */
     case 0x15:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t hdr = gba_memory_read32(gba, src & ~3u);
          src += 4;
          int len = (int)(hdr >> 8);
          int bytecount = 0, byteshift = 0;
          uint32_t writeval = 0;
          while (len > 0)
          {
               uint8_t d = gba_memory_read8(gba, src++);
               int l = (int)(d & 0x7F);
               uint8_t data = 0;
               if (d & 0x80)
               {
                    data = gba_memory_read8(gba, src++);
                    l += 3;
               }
               else
               {
                    l++;
               }
               for (int j = 0; j < l && len > 0; j++, len--)
               {
                    uint8_t b = (d & 0x80) ? data : gba_memory_read8(gba, src++);
                    writeval |= (uint32_t)b << byteshift;
                    byteshift += 8;
                    bytecount++;
                    if (bytecount == 2)
                    {
                         gba_memory_write16(gba, dst, (uint16_t)writeval);
                         dst += 2;
                         bytecount = 0;
                         byteshift = 0;
                         writeval = 0;
                    }
               }
          }
          return true;
     }

     /* ---- 0x16 Diff8bitUnFilterWram ---- */
     case 0x16:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t hdr = gba_memory_read32(gba, src);
          src += 4;
          int len = (int)(hdr >> 8);
          uint8_t data = gba_memory_read8(gba, src++);
          gba_memory_write8(gba, dst++, data);
          len--;
          while (len-- > 0)
          {
               data += gba_memory_read8(gba, src++);
               gba_memory_write8(gba, dst++, data);
          }
          return true;
     }

     /* ---- 0x17 Diff8bitUnFilterVram (halfword writes) ---- */
     case 0x17:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t hdr = gba_memory_read32(gba, src);
          src += 4;
          int len = (int)(hdr >> 8);
          uint8_t data = gba_memory_read8(gba, src++);
          uint16_t wdata = data;
          int shift = 8, bytes = 1;
          while (len >= 2)
          {
               data += gba_memory_read8(gba, src++);
               wdata |= (uint16_t)(data << shift);
               bytes++;
               shift += 8;
               if (bytes == 2)
               {
                    gba_memory_write16(gba, dst, wdata);
                    dst += 2;
                    len -= 2;
                    bytes = 0;
                    wdata = 0;
                    shift = 0;
               }
          }
          return true;
     }

     /* ---- 0x18 Diff16bitUnFilter ---- */
     case 0x18:
     {
          uint32_t src = cpu->r[0];
          uint32_t dst = cpu->r[1];
          uint32_t hdr = gba_memory_read32(gba, src);
          src += 4;
          int len = (int)(hdr >> 8);
          uint16_t data = gba_memory_read16(gba, src);
          src += 2;
          gba_memory_write16(gba, dst, data);
          dst += 2;
          len -= 2;
          while (len >= 2)
          {
               data += gba_memory_read16(gba, src);
               src += 2;
               gba_memory_write16(gba, dst, data);
               dst += 2;
               len -= 2;
          }
          return true;
     }

     /* ---- 0x19 SoundBias ---- */
     case 0x19:
     {
          /* Set SOUNDBIAS to r0 value (bias level in bits 1-9) */
          uint32_t level = cpu->r[0] & 0x3FF;
          gba_memory_write16(gba, 0x04000088, (uint16_t)(level << 1));
          return true;
     }

     /* ---- 0x1F MidiKey2Freq ---- */
     case 0x1F:
     {
          uint32_t wavedata = cpu->r[0];
          uint32_t mk = cpu->r[1];
          uint32_t fp = cpu->r[2];
          int32_t freq = (int32_t)gba_memory_read32(gba, wavedata + 4);
          double tmp = ((double)(180 - (int32_t)mk)) - ((double)fp / 256.0);
          tmp = pow(2.0, tmp / 12.0);
          cpu->r[0] = (uint32_t)((double)freq / tmp);
          return true;
     }

     /* ---- Sound driver stubs (SWI 0x28-0x2D) ---- */
     case 0x28: /* SndDriverInit */
     case 0x29: /* SndDriverMode */
     case 0x2A: /* SndDriverMain */
     case 0x2B: /* SndDriverVSync */
     case 0x2C: /* SndDriverVSyncOff */
     case 0x2D: /* SndDriverVSyncOn */
     case 0x2E: /* SndChannelClear */
     case 0x2F: /* SndDriverJmpTableCopy */
          return true;

     default:
          return false;
     }
}

/* -------------------------------------------------------------------------
 * Reset
 * ---------------------------------------------------------------------- */

void gba_cpu_reset(struct gba *gba)
{
     struct gba_cpu *cpu = &gba->cpu;
     memset(cpu, 0, sizeof(*cpu));

     /* Initial CPSR: SVC mode, IRQ+FIQ disabled, ARM state */
     cpu->cpsr = (uint32_t)GBA_MODE_SVC | GBA_CPSR_I | GBA_CPSR_F;

     /* Stack pointers per mode (GBA BIOS initializes these, but set defaults) */
     cpu->r13_svc = 0x03007FE0;
     cpu->r13_irq = 0x03007FA0;
     cpu->r13_fiq = 0x03007F60;
     cpu->r13_usr = 0x03007F00;

     cpu->r[13] = cpu->r13_svc;
     /* r[15] invariant: r[15] = start_addr + 8 */
     cpu->r[15] = 0x00000008; /* start at BIOS 0x00000000 + 8 */
     cpu->pipeline_valid = false;
     cpu->halted = false;
}

/* -------------------------------------------------------------------------
 * Main step loop
 * ---------------------------------------------------------------------- */

int gba_cpu_step(struct gba *gba)
{
     struct gba_cpu *cpu = &gba->cpu;

     /*
      * IRQ HLE return detection.
      *
      * With the new implementation, the handler finishes with SUBS PC,LR_irq,#4
      * (or equivalent) which restores SPSR_irq→CPSR and sets PC = intr_pc.
      * At that point r[15] = intr_pc + (thumb_original ? 4 : 8) = bios_irq_hle_return_r15.
      *
      * We no longer use the "PC < BIOS_SIZE" fallback — that caused false
      * positives when the handler called SWIs or accessed BIOS addresses.
      */
     if (gba->bios_irq_hle_active &&
         (cpu->r[15] == gba->bios_irq_hle_return_r15 ||
          cpu->r[15] == gba->bios_irq_hle_return_r15 + 4u))
     {
          cpu->r[0] = gba->bios_irq_hle_regs[0];
          cpu->r[1] = gba->bios_irq_hle_regs[1];
          cpu->r[2] = gba->bios_irq_hle_regs[2];
          cpu->r[3] = gba->bios_irq_hle_regs[3];
          cpu->r[12] = gba->bios_irq_hle_regs[4];
          /* The real BIOS IRQ epilogue restores SPSR_irq->CPSR.  Some no-BIOS
           * handlers return with MOV/BX through SYS mode, so do it explicitly. */
          gba_cpu_set_cpsr(gba, gba->bios_irq_hle_cpsr);
          gba->bios_irq_hle_active = false;
          /* Ensure pipeline is flushed after the mode switch. */
          cpu->pipeline_valid = false;
     }

     /* Wake from halt if IRQ pending */
     if (cpu->halted)
     {
          if (gba->halt_mode == 0 || (gba->irq.ie & gba->irq.if_))
          {
               cpu->halted = false;
               gba->halt_mode = 0;
               if (gba->halt_resume_cycles > 0)
               {
                    gba->timestamp += gba->halt_resume_cycles;
                    gba->halt_resume_cycles = 0;
               }
          }
          else
          {
               return 1;
          }
     }

     /* Service IRQ */
     if (gba_irq_pending(gba) && !(cpu->cpsr & GBA_CPSR_I))
     {
          gba_cpu_trigger_irq(gba);
     }

     bool thumb = (cpu->cpsr & GBA_CPSR_T) != 0;
     if (!gba_debug_before_instr(gba))
          return 0;

     gba_trace_step(gba);

     gba->mem_cycles = 0;

     /*
      * ARM7TDMI pipeline invariant:
      *   r[15] = address_of_current_instruction + 8  (ARM)
      *   r[15] = address_of_current_instruction + 4  (THUMB)
      *
      * We fetch from (r[15] - 8) or (r[15] - 4), then advance r[15] by 4 or 2
      * so the invariant holds for the NEXT instruction as well.
      * Branch handlers must set r[15] = destination + 8/4 to maintain this.
      */

     /* Record trace (address of current instruction) */
     cpu->trace_buf[cpu->trace_head] = cpu->r[15] - (thumb ? 4u : 8u);
     cpu->trace_head = (cpu->trace_head + 1) & (GBA_TRACE_SIZE - 1);

     if (!thumb)
     {
          uint32_t pc = cpu->r[15] - 8;
          if (!cpu->pipeline_valid)
          {
               cpu->pipeline[0] = gba_memory_fetch32(gba, pc, false);
               cpu->pipeline[1] = gba_memory_fetch32(gba, pc + 4, true);
               cpu->pipeline_valid = true;
          }
          uint32_t instr = cpu->pipeline[0];
          uint32_t fetched = gba_memory_fetch32(gba, pc + 8, true);
          cpu->r[15] += 4; /* advance prefetch */
          uint8_t cond = instr >> 28;
          if (!gba_cpu_cond(cpu->cpsr, cond))
          {
               if (cpu->pipeline_valid)
               {
                    cpu->pipeline[0] = cpu->pipeline[1];
                    cpu->pipeline[1] = fetched;
               }
               gba_debug_after_instr(gba, 1);
               return 1;
          }
          int cycles = gba_arm_execute(gba, instr);
          if (cpu->pipeline_valid)
          {
               cpu->pipeline[0] = cpu->pipeline[1];
               cpu->pipeline[1] = fetched;
          }
          cycles += gba->mem_cycles;
          gba_debug_after_instr(gba, cycles);
          return cycles;
     }
     else
     {
          uint32_t pc = cpu->r[15] - 4;
          if (!cpu->pipeline_valid)
          {
               cpu->pipeline[0] = gba_memory_fetch16(gba, pc, false);
               cpu->pipeline[1] = gba_memory_fetch16(gba, pc + 2, true);
               cpu->pipeline_valid = true;
          }
          uint16_t instr = (uint16_t)cpu->pipeline[0];
          uint16_t fetched = gba_memory_fetch16(gba, pc + 4, true);
          cpu->r[15] += 2; /* advance prefetch */
          int cycles = gba_thumb_execute(gba, instr);
          if (cpu->pipeline_valid)
          {
               cpu->pipeline[0] = cpu->pipeline[1];
               cpu->pipeline[1] = fetched;
          }
          cycles += gba->mem_cycles;
          gba_debug_after_instr(gba, cycles);
          return cycles;
     }
}
