#ifndef GB_H
#define GB_H

#include <stdint.h>
#include <stdbool.h>
#include "debug.h"

/* ---------- IRQ types ---------- */
enum gb_irq_type {
    GB_IRQ_VSYNC    = 0,
    GB_IRQ_LCD_STAT = 1,
    GB_IRQ_TIMER    = 2,
    GB_IRQ_SERIAL   = 3,
    GB_IRQ_INPUT    = 4,
};

/* ---------- CPU state ---------- */
struct gb_cpu {
    bool     irq_enable;
    bool     irq_enable_next;
    uint8_t  irq_enable_delay;
    bool     halted;
    bool     halt_bug;

    uint16_t sp;
    uint16_t pc;

    uint8_t  a, b, c, d, e, h, l;

    /* Flags (stored as individual booleans) */
    bool f_z;   /* Zero        */
    bool f_n;   /* Subtract    */
    bool f_h;   /* Half-carry  */
    bool f_c;   /* Carry       */
};

/* ---------- Sync / event subsystem ---------- */
struct gb_sync {
    int32_t first_event;
};

/* ---------- IRQ controller state ---------- */
struct gb_irq {
    uint8_t irq_enable;  /* IE register  */
    uint8_t irq_flags;   /* IF register  */
};

/* ---------- Top-level emulator state ---------- */
struct gb {
    struct gb_cpu  cpu;
    bool           gbc;
    bool           speed_switch_pending;
    bool           double_speed;
    int32_t        timestamp;
    struct gb_sync sync;
    struct gb_irq  irq;
    struct gb_debug debug;
};

/* ---------- External interfaces (implemented by platform / mock) ---------- */
uint8_t  gb_memory_readb(struct gb *gb, uint16_t addr);
void     gb_memory_writeb(struct gb *gb, uint16_t addr, uint8_t val);
void     gb_sync_check_events(struct gb *gb);
void     gb_sync_rebase(struct gb *gb);
void     gb_timer_sync(struct gb *gb);
void     gb_dma_sync(struct gb *gb);
void     die(void);

/* ---------- CPU interface ---------- */
void     gb_cpu_reset(struct gb *gb);
void     gb_cpu_dump(struct gb *gb);
int32_t  gb_cpu_run_cycles(struct gb *gb, int32_t cycles);

#endif /* GB_H */
