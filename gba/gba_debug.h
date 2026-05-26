#ifndef _GBA_DEBUG_H_
#define _GBA_DEBUG_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define GBA_DEBUG_MAX_BREAKPOINTS 32

struct gba;

enum gba_debug_state
{
     GBA_DEBUG_RUNNING,
     GBA_DEBUG_PAUSED,
     GBA_DEBUG_STEPPING,
};

struct gba_debug
{
     bool enabled;
     enum gba_debug_state state;
     uint64_t instruction_count;
     uint64_t cycle_count;

     uint32_t breakpoints[GBA_DEBUG_MAX_BREAKPOINTS];
     bool bp_enabled[GBA_DEBUG_MAX_BREAKPOINTS];
     unsigned n_breakpoints;

     /* CPU execution tracer */
     bool trace_enabled;
     uint64_t trace_limit;   /* stop tracing after this many instructions (0 = unlimited) */
     FILE *trace_fp;         /* output file (NULL = stderr) */
};

void gba_debug_reset(struct gba *gba);
bool gba_debug_before_instr(struct gba *gba);
void gba_debug_after_instr(struct gba *gba, int cycles);
void gba_debug_add_breakpoint(struct gba *gba, uint32_t addr);
void gba_debug_remove_breakpoint(struct gba *gba, unsigned index);
bool gba_debug_has_breakpoint(struct gba *gba, uint32_t addr);
void gba_debug_toggle_breakpoint(struct gba *gba, uint32_t addr);

/* Tracer — call once per CPU step, before the instruction executes */
void gba_trace_step(struct gba *gba);

#endif /* _GBA_DEBUG_H_ */
