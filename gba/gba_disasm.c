#include <stdio.h>
#include "gba.h"
#include "gba_disasm.h"

static const char *cond_name(unsigned cond)
{
     static const char *names[16] = {
         "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
         "hi", "ls", "ge", "lt", "gt", "le", "", "nv"};
     return names[cond & 15];
}

static const char *dp_name(unsigned op)
{
     static const char *names[16] = {
         "and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
         "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"};
     return names[op & 15];
}

unsigned gba_disasm_len(struct gba *gba, uint32_t addr, int thumb)
{
     (void)gba;
     (void)addr;
     return thumb ? 2u : 4u;
}

static void disasm_thumb(struct gba *gba, uint32_t addr, uint16_t op,
                         char *out, size_t out_len)
{
     unsigned hi = op >> 8;

     if ((hi & 0xE0) == 0x20)
     {
          static const char *names[4] = {"mov", "cmp", "add", "sub"};
          unsigned kind = (op >> 11) & 3;
          unsigned rd = (op >> 8) & 7;
          unsigned imm = op & 0xff;
          snprintf(out, out_len, "%s r%u, #0x%02x", names[kind], rd, imm);
          return;
     }

     if ((hi & 0xF8) == 0x48)
     {
          unsigned rd = (op >> 8) & 7;
          uint32_t pc = (addr + 4) & ~3u;
          uint32_t target = pc + (uint32_t)(op & 0xff) * 4u;
          snprintf(out, out_len, "ldr r%u, [pc, #0x%03x] ; [%08x]",
                   rd, (op & 0xff) * 4u, target);
          return;
     }

     if ((hi & 0xF0) == 0x60 || (hi & 0xF0) == 0x70)
     {
          unsigned load = (op >> 11) & 1;
          unsigned byte = (op >> 12) & 1;
          unsigned off = (op >> 6) & 0x1f;
          unsigned rb = (op >> 3) & 7;
          unsigned rd = op & 7;
          snprintf(out, out_len, "%s%s r%u, [r%u, #0x%x]",
                   load ? "ldr" : "str", byte ? "b" : "",
                   rd, rb, byte ? off : off * 4u);
          return;
     }

     if ((hi & 0xF0) == 0xD0 && ((op >> 8) & 0xf) != 0xf)
     {
          unsigned cond = (op >> 8) & 0xf;
          int32_t rel = (int32_t)(int8_t)(op & 0xff) * 2;
          snprintf(out, out_len, "b%s %08x", cond_name(cond), addr + 4 + rel);
          return;
     }

     if ((hi & 0xF8) == 0xE0)
     {
          int32_t rel = ((int32_t)((op & 0x7ff) << 21) >> 20);
          snprintf(out, out_len, "b %08x", addr + 4 + rel);
          return;
     }

     if ((hi & 0xF0) == 0xF0)
     {
          snprintf(out, out_len, ((op >> 11) & 1) ? "bl low #0x%03x" : "bl high #0x%03x",
                   op & 0x7ff);
          return;
     }

     if ((hi & 0xFC) == 0x44 && ((op >> 8) & 3) == 3)
     {
          unsigned rm = ((op >> 3) & 7) | (((op >> 6) & 1) << 3);
          snprintf(out, out_len, "bx r%u", rm);
          return;
     }

     if (hi == 0xDF)
     {
          snprintf(out, out_len, "swi #0x%02x", op & 0xff);
          return;
     }

     snprintf(out, out_len, ".thumb 0x%04x", op);
}

static void disasm_arm(struct gba *gba, uint32_t addr, uint32_t op,
                       char *out, size_t out_len)
{
     unsigned cond = op >> 28;
     unsigned hi = (op >> 20) & 0xff;
     unsigned lo4 = (op >> 4) & 0xf;

     if ((hi & 0xE0) == 0xA0)
     {
          int32_t rel = (int32_t)(op << 8) >> 6;
          snprintf(out, out_len, "b%s%s %08x",
                   ((op >> 24) & 1) ? "l" : "", cond_name(cond),
                   addr + 8 + rel);
          return;
     }

     if ((hi == 0x12 || hi == 0x16) && lo4 == 1)
     {
          snprintf(out, out_len, "%s%s r%u", hi == 0x16 ? "blx" : "bx",
                   cond_name(cond), op & 0xf);
          return;
     }

     if ((hi & 0xC0) == 0x40)
     {
          unsigned load = (op >> 20) & 1;
          unsigned byte = (op >> 22) & 1;
          unsigned rn = (op >> 16) & 0xf;
          unsigned rd = (op >> 12) & 0xf;
          unsigned imm = op & 0xfff;
          snprintf(out, out_len, "%s%s%s r%u, [r%u, #0x%x]",
                   load ? "ldr" : "str", cond_name(cond), byte ? "b" : "",
                   rd, rn, imm);
          return;
     }

     if ((hi & 0xC0) == 0)
     {
          unsigned dp = (op >> 21) & 0xf;
          unsigned rd = (op >> 12) & 0xf;
          unsigned rn = (op >> 16) & 0xf;
          if ((op >> 25) & 1)
          {
               unsigned imm = op & 0xff;
               unsigned rot = ((op >> 8) & 0xf) * 2;
               uint32_t val = rot ? ((imm >> rot) | (imm << (32 - rot))) : imm;
               if (dp == 13 || dp == 15)
                    snprintf(out, out_len, "%s%s r%u, #0x%x", dp_name(dp), cond_name(cond), rd, val);
               else
                    snprintf(out, out_len, "%s%s r%u, r%u, #0x%x", dp_name(dp), cond_name(cond), rd, rn, val);
          }
          else
          {
               unsigned rm = op & 0xf;
               if (dp == 13 || dp == 15)
                    snprintf(out, out_len, "%s%s r%u, r%u", dp_name(dp), cond_name(cond), rd, rm);
               else
                    snprintf(out, out_len, "%s%s r%u, r%u, r%u", dp_name(dp), cond_name(cond), rd, rn, rm);
          }
          return;
     }

     if ((hi & 0xF0) == 0xF0)
     {
          snprintf(out, out_len, "swi%s #0x%06x", cond_name(cond), op & 0x00ffffff);
          return;
     }

     snprintf(out, out_len, ".arm 0x%08x", op);
}

void gba_disasm(struct gba *gba, uint32_t addr, int thumb, char *out, size_t out_len)
{
     if (thumb)
     {
          uint16_t op = gba_memory_peek16(gba, addr);
          disasm_thumb(gba, addr, op, out, out_len);
     }
     else
     {
          uint32_t op = gba_memory_peek32(gba, addr);
          disasm_arm(gba, addr, op, out, out_len);
     }
}
