/*
 * gb_mock.c
 *
 * Minimal platform stubs for unit-testing cpu.c in isolation.
 * Provides a flat 64 KB memory array and no-op sync callbacks.
 */

#include "gb.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Flat 64 KB address space                                             */
/* ------------------------------------------------------------------ */

static uint8_t g_memory[0x10000];

/* ------------------------------------------------------------------ */
/* Test helper API (called from test_cpu.c)                             */
/* ------------------------------------------------------------------ */

/**
 * Reset the GB struct and the mock memory to a clean state.
 * CPU starts with PC=0x100, SP=0xFFFE, all registers zero, IRQs off.
 */
void gb_mock_init(struct gb *gb)
{
    memset(gb,       0, sizeof(*gb));
    memset(g_memory, 0, sizeof(g_memory));

    gb->cpu.sp          = 0xfffe;
    gb->cpu.pc          = 0x0100;
    gb->sync.first_event = 0x7fffffff; /* never fire sync events */
    gb->irq.irq_enable  = 0;
    gb->irq.irq_flags   = 0;
}

/**
 * Write `len` bytes starting at `addr` into mock memory.
 * Typically used to place opcodes at the current PC before stepping.
 */
void gb_mock_write(struct gb *gb, uint16_t addr, const uint8_t *bytes, int len)
{
    (void)gb;
    for (int i = 0; i < len; i++) {
        g_memory[(uint16_t)(addr + i)] = bytes[i];
    }
}

/**
 * Direct read from mock memory (without the 4-cycle tick side-effect).
 * Useful for inspecting RAM written by store instructions.
 */
uint8_t gb_mock_peek(uint16_t addr)
{
    return g_memory[addr];
}

/* ------------------------------------------------------------------ */
/* Memory interface (called by cpu.c)                                   */
/* ------------------------------------------------------------------ */

uint8_t gb_memory_readb(struct gb *gb, uint16_t addr)
{
    (void)gb;
    return g_memory[addr];
}

void gb_memory_writeb(struct gb *gb, uint16_t addr, uint8_t val)
{
    (void)gb;
    g_memory[addr] = val;
}

/* ------------------------------------------------------------------ */
/* Sync interface (called by cpu.c)                                     */
/* ------------------------------------------------------------------ */

void gb_sync_check_events(struct gb *gb)
{
    (void)gb;
    /* No peripheral events in unit tests */
}

void gb_sync_rebase(struct gb *gb)
{
    gb->timestamp = 0;
}

void gb_timer_sync(struct gb *gb)
{
    (void)gb;
}

void gb_dma_sync(struct gb *gb)
{
    (void)gb;
}

void gb_debug_before_instr(struct gb *gb)
{
    (void)gb;
}

/* ------------------------------------------------------------------ */
/* Fatal error                                                           */
/* ------------------------------------------------------------------ */

void die(void)
{
    fprintf(stderr, "[mock] die() called – unexpected opcode or CPU fault\n");
    abort();
}
