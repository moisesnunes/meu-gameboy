#include <string.h>
#include <stdio.h>
#include "gba.h"
#include "gba_disasm.h"

static uint32_t current_pc(struct gba *gba)
{
     return gba_cpu_current_pc(&gba->cpu);
}

void gba_debug_reset(struct gba *gba)
{
     struct gba_debug *dbg = &gba->debug;
     bool enabled = dbg->enabled;
     uint32_t breakpoints[GBA_DEBUG_MAX_BREAKPOINTS];
     bool bp_enabled[GBA_DEBUG_MAX_BREAKPOINTS];
     unsigned n_breakpoints = dbg->n_breakpoints;

     memcpy(breakpoints, dbg->breakpoints, sizeof(breakpoints));
     memcpy(bp_enabled, dbg->bp_enabled, sizeof(bp_enabled));

     memset(dbg, 0, sizeof(*dbg));
     dbg->enabled = enabled;
     dbg->state = enabled ? GBA_DEBUG_PAUSED : GBA_DEBUG_RUNNING;
     dbg->n_breakpoints = n_breakpoints;
     memcpy(dbg->breakpoints, breakpoints, sizeof(breakpoints));
     memcpy(dbg->bp_enabled, bp_enabled, sizeof(bp_enabled));
}

bool gba_debug_before_instr(struct gba *gba)
{
     struct gba_debug *dbg = &gba->debug;
     uint32_t pc;
     unsigned i;

     if (!dbg->enabled)
          return true;
     if (dbg->state == GBA_DEBUG_PAUSED)
          return false;

     pc = current_pc(gba);
     if (dbg->state != GBA_DEBUG_STEPPING)
     {
          for (i = 0; i < dbg->n_breakpoints; i++)
          {
               if (dbg->bp_enabled[i] && dbg->breakpoints[i] == pc)
               {
                    dbg->state = GBA_DEBUG_PAUSED;
                    return false;
               }
          }
     }

     return true;
}

void gba_debug_after_instr(struct gba *gba, int cycles)
{
     struct gba_debug *dbg = &gba->debug;

     dbg->instruction_count++;
     if (cycles > 0)
          dbg->cycle_count += (uint64_t)cycles;

     if (dbg->enabled && dbg->state == GBA_DEBUG_STEPPING)
          dbg->state = GBA_DEBUG_PAUSED;
}

void gba_debug_add_breakpoint(struct gba *gba, uint32_t addr)
{
     struct gba_debug *dbg = &gba->debug;
     unsigned i;

     addr &= ~1U;
     for (i = 0; i < dbg->n_breakpoints; i++)
     {
          if (dbg->breakpoints[i] == addr)
          {
               dbg->bp_enabled[i] = true;
               return;
          }
     }
     if (dbg->n_breakpoints >= GBA_DEBUG_MAX_BREAKPOINTS)
          return;

     dbg->breakpoints[dbg->n_breakpoints] = addr;
     dbg->bp_enabled[dbg->n_breakpoints] = true;
     dbg->n_breakpoints++;
}

void gba_debug_remove_breakpoint(struct gba *gba, unsigned index)
{
     struct gba_debug *dbg = &gba->debug;
     unsigned i;

     if (index >= dbg->n_breakpoints)
          return;
     for (i = index; i + 1 < dbg->n_breakpoints; i++)
     {
          dbg->breakpoints[i] = dbg->breakpoints[i + 1];
          dbg->bp_enabled[i] = dbg->bp_enabled[i + 1];
     }
     dbg->n_breakpoints--;
}

bool gba_debug_has_breakpoint(struct gba *gba, uint32_t addr)
{
     struct gba_debug *dbg = &gba->debug;
     unsigned i;

     addr &= ~1U;
     for (i = 0; i < dbg->n_breakpoints; i++)
     {
          if (dbg->bp_enabled[i] && dbg->breakpoints[i] == addr)
               return true;
     }
     return false;
}

void gba_debug_toggle_breakpoint(struct gba *gba, uint32_t addr)
{
     struct gba_debug *dbg = &gba->debug;
     unsigned i;

     addr &= ~1U;
     for (i = 0; i < dbg->n_breakpoints; i++)
     {
          if (dbg->breakpoints[i] == addr)
          {
               gba_debug_remove_breakpoint(gba, i);
               return;
          }
     }
     gba_debug_add_breakpoint(gba, addr);
}

/* -------------------------------------------------------------------------
 * CPU execution tracer
 * ---------------------------------------------------------------------- */

void gba_trace_step(struct gba *gba)
{
     struct gba_debug *dbg = &gba->debug;
     struct gba_cpu *cpu = &gba->cpu;
     FILE *fp;
     char mnem[64];
     int thumb;
     uint32_t pc;

     if (!dbg->trace_enabled)
          return;
     if (dbg->trace_limit && dbg->instruction_count >= dbg->trace_limit)
          return;

     fp = dbg->trace_fp ? dbg->trace_fp : stderr;
     thumb = (cpu->cpsr & GBA_CPSR_T) != 0;
     pc = gba_cpu_current_pc(cpu);

     gba_disasm(gba, pc, thumb, mnem, sizeof(mnem));

     /* Format:  #instr  PC  CPSR  r0 r1 r2 r3 r4 r5 r6 r7  SP  LR  mnemonic */
     fprintf(fp,
             "%10llu  %08X  cpsr=%08X  "
             "r0=%08X r1=%08X r2=%08X r3=%08X "
             "r4=%08X r5=%08X r6=%08X r7=%08X "
             "sp=%08X lr=%08X  %s\n",
             (unsigned long long)dbg->instruction_count,
             pc, cpu->cpsr,
             cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3],
             cpu->r[4], cpu->r[5], cpu->r[6], cpu->r[7],
             cpu->r[13], cpu->r[14],
             mnem);
}
