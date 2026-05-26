#ifndef _GB_DEBUG_H_
#define _GB_DEBUG_H_

#include <stdint.h>
#include <stdbool.h>

#define GB_DEBUG_MAX_BREAKPOINTS 32
#define GB_DEBUG_MAX_WATCHPOINTS 16

struct gb;

typedef enum {
    GB_DEBUG_RUNNING,   /* emulação rodando normalmente */
    GB_DEBUG_PAUSED,    /* pausado — aguardando comando do usuário */
    GB_DEBUG_STEPPING,  /* executa exatamente uma instrução e volta a PAUSED */
    GB_DEBUG_STEP_OVER, /* executa até a próxima linha após uma CALL */
    GB_DEBUG_STEP_OUT,  /* executa até um RET */
} gb_debug_state;

typedef enum {
    GB_WATCHPOINT_READ = 1,   /* pausa ao ler o endereço */
    GB_WATCHPOINT_WRITE = 2,  /* pausa ao escrever o endereço */
    GB_WATCHPOINT_BOTH = 3,   /* pausa ao ler ou escrever */
} gb_watchpoint_type;

struct gb_watchpoint {
    uint16_t        addr;
    gb_watchpoint_type type;
    bool            enabled;
};

/* ────────────────────────────────────────────────────────── */
/* Hardware Visualization — System Diagram (Milestone 2)       */
/* ────────────────────────────────────────────────────────── */

struct gb_sys_viz {
    /* Fade timers: set to 1.0 by bus activity, decay at ~8/s in the UI */
    float fade_cpu_rom;
    float fade_cpu_wram;
    float fade_cpu_vram;
    float fade_cpu_oam;
    float fade_cpu_io;
    float fade_dma_oam;
    float fade_ppu_vram;
    float fade_irq_cpu;
    float fade_apu;
    /* Last bus values (informational) */
    uint16_t last_bus_addr;
    uint8_t  last_bus_data;
};

/* ────────────────────────────────────────────────────────── */
/* Hardware Visualization — CPU Datapath (Milestone 3)         */
/* ────────────────────────────────────────────────────────── */

struct gb_cpu_viz {
    uint8_t  stage;        /* 0=fetch 1=decode 2=execute 3=irq */
    uint8_t  alu_op;       /* GB_VIZ_ALU_* */
    uint8_t  src;          /* GB_VIZ_REG_* source */
    uint8_t  dst;          /* GB_VIZ_REG_* destination */
    uint16_t addr_bus;
    uint8_t  data_bus;
    bool     bus_write;
    uint8_t  opcode;
    char     mnemonic[24];
    float    activity_fade;

    /* Operands and results captured across instruction boundary */
    uint8_t  alu_a;         /* accumulator value before this instruction */
    uint8_t  alu_b;         /* source operand value before this instruction */
    uint8_t  alu_result;    /* accumulator after previous instruction */
    uint8_t  flags_before;  /* (Z<<3)|(N<<2)|(H<<1)|C before this instruction */
    uint8_t  flags_after;   /* flags after previous instruction */
    uint8_t  flags_changed; /* bitmask: which flags changed last instruction */
    uint8_t  m_cycles;      /* M-cycles consumed by previous instruction */
    int32_t  timestamp_last;/* gb->timestamp at start of previous instruction */
};

/* ALU operation kinds */
#define GB_VIZ_ALU_NONE  0
#define GB_VIZ_ALU_ADD   1
#define GB_VIZ_ALU_SUB   2
#define GB_VIZ_ALU_AND   3
#define GB_VIZ_ALU_OR    4
#define GB_VIZ_ALU_XOR   5
#define GB_VIZ_ALU_CP    6
#define GB_VIZ_ALU_INC   7
#define GB_VIZ_ALU_DEC   8
#define GB_VIZ_ALU_SHIFT 9
#define GB_VIZ_ALU_BIT   10

/* Register identifiers */
#define GB_VIZ_REG_NONE  0
#define GB_VIZ_REG_A     1
#define GB_VIZ_REG_B     2
#define GB_VIZ_REG_C     3
#define GB_VIZ_REG_D     4
#define GB_VIZ_REG_E     5
#define GB_VIZ_REG_H     6
#define GB_VIZ_REG_L     7
#define GB_VIZ_REG_HL    8
#define GB_VIZ_REG_BC    9
#define GB_VIZ_REG_DE    10
#define GB_VIZ_REG_SP    11
#define GB_VIZ_REG_IMM8  12
#define GB_VIZ_REG_IMM16 13
#define GB_VIZ_REG_MEM   14

/* CPU pipeline stages */
#define GB_VIZ_STAGE_FETCH   0
#define GB_VIZ_STAGE_DECODE  1
#define GB_VIZ_STAGE_EXECUTE 2
#define GB_VIZ_STAGE_IRQ     3

struct gb_debug {
    bool            enabled;
    gb_debug_state  state;
    uint64_t        instruction_count;
    uint64_t        cycle_count;

    uint16_t breakpoints[GB_DEBUG_MAX_BREAKPOINTS];
    bool     bp_enabled [GB_DEBUG_MAX_BREAKPOINTS];
    unsigned n_breakpoints;

    /* ── Watchpoints (read/write) ── */
    struct gb_watchpoint watchpoints[GB_DEBUG_MAX_WATCHPOINTS];
    unsigned n_watchpoints;

    /* ── Step over / step out ── */
    uint16_t step_return_addr;  /* endereço de retorno para step_over */
    int      step_depth;        /* profundidade de call stack para step_out */

    /* ── Profiling ── */

    /* Opcode profiler: [0..255] = opcodes normais, [256..511] = CB-prefixados */
    uint32_t opcode_hits[512];

    /* Heatmap de execução: quantas vezes cada endereço foi o PC */
    uint32_t exec_heatmap[65536];

    /* Coverage: 1 bit por endereço — foi executado alguma vez? (8 KiB) */
    uint8_t  exec_coverage[8192];

    /* ── Hardware visualization data ── */
    struct gb_sys_viz sys_viz;
    struct gb_cpu_viz cpu_viz;
};

void     gb_debug_init            (struct gb *gb);
void     gb_debug_before_instr    (struct gb *gb);   /* chamado antes de cada instrução */
void     gb_debug_add_breakpoint  (struct gb *gb, uint16_t addr);
void     gb_debug_remove_breakpoint(struct gb *gb, unsigned index);
bool     gb_debug_has_breakpoint  (struct gb *gb, uint16_t addr);
void     gb_debug_reset_profiler  (struct gb *gb);
bool     gb_debug_trace_enabled   (void);
bool     gb_debug_trace_set_enabled(struct gb *gb, bool enabled, const char *path);

/* Watchpoints */
void     gb_debug_add_watchpoint  (struct gb *gb, uint16_t addr, gb_watchpoint_type type);
void     gb_debug_remove_watchpoint(struct gb *gb, unsigned index);
void     gb_debug_check_watchpoint(struct gb *gb, uint16_t addr, gb_watchpoint_type type);

/* Step over / step out */
void     gb_debug_step_over       (struct gb *gb);
void     gb_debug_step_out        (struct gb *gb);
uint16_t gb_debug_get_next_instr_addr(struct gb *gb);  /* próximo endereço após a instrução atual */

#endif /* _GB_DEBUG_H_ */
