#ifndef _GBA_CPU_H_
#define _GBA_CPU_H_

#include <stdint.h>
#include <stdbool.h>

struct gba;

/* ARM7TDMI operating modes */
enum gba_cpu_mode {
    GBA_MODE_USR = 0x10,
    GBA_MODE_FIQ = 0x11,
    GBA_MODE_IRQ = 0x12,
    GBA_MODE_SVC = 0x13,
    GBA_MODE_ABT = 0x17,
    GBA_MODE_UND = 0x1B,
    GBA_MODE_SYS = 0x1F,
};

/* CPSR/SPSR flag bits */
#define GBA_CPSR_N  (1U << 31)  /* Negative */
#define GBA_CPSR_Z  (1U << 30)  /* Zero */
#define GBA_CPSR_C  (1U << 29)  /* Carry */
#define GBA_CPSR_V  (1U << 28)  /* oVerflow */
#define GBA_CPSR_I  (1U << 7)   /* IRQ disable */
#define GBA_CPSR_F  (1U << 6)   /* FIQ disable */
#define GBA_CPSR_T  (1U << 5)   /* THUMB state */
#define GBA_CPSR_M  (0x1FU)     /* Mode mask */

/* Banked register indices */
#define GBA_SP      13
#define GBA_LR      14
#define GBA_PC      15

#define GBA_TRACE_SIZE 64

struct gba_cpu {
    /* General-purpose registers (current mode view) */
    uint32_t r[16];

    /* Program Status Registers */
    uint32_t cpsr;
    uint32_t spsr; /* current mode SPSR (USR/SYS have no real SPSR) */

    /* Banked registers per mode */
    uint32_t r8_usr,  r9_usr,  r10_usr, r11_usr, r12_usr;
    uint32_t r13_usr, r14_usr;
    uint32_t r13_svc, r14_svc, spsr_svc;
    uint32_t r13_abt, r14_abt, spsr_abt;
    uint32_t r13_irq, r14_irq, spsr_irq;
    uint32_t r13_und, r14_und, spsr_und;
    uint32_t r8_fiq,  r9_fiq,  r10_fiq, r11_fiq, r12_fiq;
    uint32_t r13_fiq, r14_fiq, spsr_fiq;

    /* Pipeline prefetch buffer */
    uint32_t pipeline[2];   /* [0]=decoded, [1]=fetched */
    bool     pipeline_valid;

    /* Halt/stop */
    bool halted;
    bool stopped;

    /* Interrupt pending (set by gba_irq when IME+IE+IF fire) */
    bool irq_line;

    /* Debug trace ring buffer */
    uint32_t trace_buf[GBA_TRACE_SIZE];
    uint8_t  trace_head;
};

static inline uint32_t gba_cpu_current_pc(const struct gba_cpu *cpu)
{
    return cpu->r[GBA_PC] - ((cpu->cpsr & GBA_CPSR_T) ? 4u : 8u);
}

void     gba_cpu_reset(struct gba *gba);
int      gba_cpu_step(struct gba *gba);  /* returns cycles consumed */
void     gba_cpu_switch_mode(struct gba *gba, enum gba_cpu_mode new_mode);
void     gba_cpu_set_cpsr(struct gba *gba, uint32_t new_cpsr);
uint32_t gba_cpu_get_spsr(struct gba *gba);
void     gba_cpu_set_spsr(struct gba *gba, uint32_t val);
void     gba_cpu_trigger_irq(struct gba *gba);
bool     gba_cpu_handle_swi(struct gba *gba, uint32_t comment);

/* Used by memory for data-abort / undefined exceptions */
void     gba_cpu_exception(struct gba *gba, uint32_t vector, enum gba_cpu_mode mode);

/* Condition code evaluation */
static inline bool gba_cpu_cond(uint32_t cpsr, uint8_t cond)
{
    bool n = (cpsr >> 31) & 1;
    bool z = (cpsr >> 30) & 1;
    bool c = (cpsr >> 29) & 1;
    bool v = (cpsr >> 28) & 1;
    switch (cond) {
    case 0x0: return z;               /* EQ */
    case 0x1: return !z;              /* NE */
    case 0x2: return c;               /* CS */
    case 0x3: return !c;              /* CC */
    case 0x4: return n;               /* MI */
    case 0x5: return !n;              /* PL */
    case 0x6: return v;               /* VS */
    case 0x7: return !v;              /* VC */
    case 0x8: return c && !z;         /* HI */
    case 0x9: return !c || z;         /* LS */
    case 0xA: return n == v;          /* GE */
    case 0xB: return n != v;          /* LT */
    case 0xC: return !z && (n == v);  /* GT */
    case 0xD: return z || (n != v);   /* LE */
    case 0xE: return true;            /* AL */
    default:  return false;           /* NV (never) */
    }
}

#endif /* _GBA_CPU_H_ */
