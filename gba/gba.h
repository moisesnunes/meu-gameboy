#ifndef _GBA_GBA_H_
#define _GBA_GBA_H_

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <semaphore.h>

struct gba;

#include "gba_sync.h"
#include "gba_irq.h"
#include "gba_cpu.h"
#include "gba_debug.h"
#include "gba_memory.h"
#include "gba_cart.h"
#include "gba_gpu.h"
#include "gba_input.h"
#include "gba_dma.h"
#include "gba_timer.h"
#include "gba_apu.h"

/* GBA CPU frequency: 16.78 MHz */
#define GBA_CPU_FREQ_HZ  16777216U

/* Frontend callback table (analogous to gb_frontend) */
struct gba_frontend {
    /* Called after each completed scanline with 240 RGB555 pixels */
    void (*draw_line)(void *data, uint8_t line, const uint16_t *pixels);
    /* Called when a full frame is ready to display */
    void (*flip)(void *data);
    /* Called once per frame to poll input */
    void (*refresh_input)(void *data);
    /* Cleanup */
    void (*destroy)(void *data);
    void *data;
};

enum gba_hw_model {
    GBA_HW_AGB,   /* original GBA (AGB-001) */
    GBA_HW_AGS,   /* GBA SP (AGS-001 / AGS-101) */
    GBA_HW_OXY,   /* Game Boy Micro (OXY-001) */
};

struct gba {
    enum gba_hw_model hw_model;

    /* CPU cycle counter — same role as gb.timestamp */
    int32_t timestamp;

    bool quit;

    struct gba_sync     sync;
    struct gba_irq      irq;
    struct gba_cpu      cpu;
    struct gba_debug    debug;
    struct gba_cart     cart;
    struct gba_gpu      gpu;
    struct gba_input    input;
    struct gba_dma      dma;
    struct gba_timer    timer;
    struct gba_apu      apu;
    struct gba_frontend frontend;

    /* BIOS ROM (16KB) — NULL if not loaded, open-bus used instead */
    uint8_t *bios;
    uint32_t bios_size;
    uint32_t bios_open_bus;
    uint32_t bios_open_bus_after_read;
    bool bios_open_bus_has_after_read;
    bool bios_irq_hle_active;
    uint32_t bios_irq_hle_return_r15;
    uint32_t bios_irq_hle_regs[5];
    uint32_t bios_irq_hle_cpsr;
    bool bios_intr_wait_active;
    uint16_t bios_intr_wait_mask;

    /* External Work RAM (256KB) */
    uint8_t ewram[GBA_EWRAM_SIZE];
    /* Internal Work RAM (32KB) */
    uint8_t iwram[GBA_IWRAM_SIZE];
    /* Palette RAM (1KB) */
    uint8_t pram[GBA_PAL_SIZE];
    /* Video RAM (96KB) */
    uint8_t vram[GBA_VRAM_SIZE];
    /* Object Attribute Memory (1KB) */
    uint8_t oam[GBA_OAM_SIZE];

    /* WAITCNT (0x04000204): wait-state configuration */
    uint16_t waitcnt;

    /* Extra cycles charged by the last memory access (ROM/SRAM wait states).
       CPU clears this to 0 before each instruction and adds it to the cycle count. */
    int mem_cycles;

    /* POSTFLG: set to 1 after BIOS POST */
    uint8_t postflg;

    /* Halt mode: 0=running, 1=halted (HALTCNT), 2=stopped */
    uint8_t halt_mode;
    int halt_resume_cycles;
};

struct gba *gba_create(void);
void        gba_destroy(struct gba *gba);
void        gba_reset(struct gba *gba);
bool        gba_load_bios(struct gba *gba, const char *path);
void        gba_run_frame(struct gba *gba);

#endif /* _GBA_GBA_H_ */
