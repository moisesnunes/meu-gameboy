#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gba/gba.h"
#include "gba/gba_disasm.h"

int gba_arm_execute(struct gba *gba, uint32_t instr);
int gba_thumb_execute(struct gba *gba, uint16_t instr);

struct reset_probe
{
     int depth;
     int completed;
};

static void lock_reset_probe(void *data)
{
     struct reset_probe *probe = data;
     assert(probe->depth == 0);
     probe->depth = 1;
}

static void unlock_reset_probe(void *data)
{
     struct reset_probe *probe = data;
     assert(probe->depth == 1);
     probe->depth = 0;
     probe->completed++;
}

static void test_peek_does_not_sync_timer(struct gba *gba)
{
     struct gba_timer_channel *timer = &gba->timer.ch[0];

     gba->timestamp = 100;
     gba->sync.last_sync[GBA_SYNC_TIMER] = 40;
     timer->counter = 0x1234;
     timer->cycles_acc = 7;
     timer->enable = true;

     assert(gba_memory_peek16(gba, REG_TM0CNT_L) == 0x1234);
     assert(gba->sync.last_sync[GBA_SYNC_TIMER] == 40);
     assert(timer->counter == 0x1234);
     assert(timer->cycles_acc == 7);
}

static void test_repeated_reset_reinitializes_apu_sync(struct gba *gba)
{
     struct reset_probe probe = {0};
     gba->frontend.data = &probe;
     gba->frontend.lock_reset = lock_reset_probe;
     gba->frontend.unlock_reset = unlock_reset_probe;

     gba_reset(gba);
     assert(gba->apu.sync_initialized);
     assert(sem_trywait(&gba->apu.buf_free) == 0);
     sem_post(&gba->apu.buf_free);

     gba_reset(gba);
     assert(gba->apu.sync_initialized);
     assert(sem_trywait(&gba->apu.buf_free) == 0);
     sem_post(&gba->apu.buf_free);
     assert(sem_trywait(&gba->apu.buf_ready) != 0);
     assert(probe.depth == 0);
     assert(probe.completed == 2);

     gba->frontend.data = NULL;
     gba->frontend.lock_reset = NULL;
     gba->frontend.unlock_reset = NULL;
}

static void test_reset_preserves_debug_pause_state(struct gba *gba)
{
     gba->debug.enabled = true;
     gba->debug.state = GBA_DEBUG_RUNNING;
     gba_reset(gba);
     assert(gba->debug.enabled);
     assert(gba->debug.state == GBA_DEBUG_RUNNING);

     gba->debug.state = GBA_DEBUG_PAUSED;
     gba_reset(gba);
     assert(gba->debug.state == GBA_DEBUG_PAUSED);

     gba->debug.enabled = false;
     gba->debug.state = GBA_DEBUG_RUNNING;
}

static void test_mov_r5_r0(struct gba *gba)
{
     const uint32_t flags = GBA_CPSR_N | GBA_CPSR_Z | GBA_CPSR_C | GBA_CPSR_V;
     char text[64];

     gba->cpu.r[0] = 0x12345678;
     gba->cpu.r[5] = 0;
     gba->cpu.cpsr = GBA_MODE_SYS | GBA_CPSR_T | flags;
     assert(gba_thumb_execute(gba, 0x4605) == 1);
     assert(gba->cpu.r[5] == 0x12345678);
     assert((gba->cpu.cpsr & flags) == flags);

     gba_memory_write16(gba, GBA_IWRAM_BASE, 0x4605);
     gba_disasm(gba, GBA_IWRAM_BASE, 1, text, sizeof(text));
     assert(strcmp(text, "mov r5, r0") == 0);

     gba->cpu.r[5] = 0;
     gba->cpu.cpsr = GBA_MODE_SYS | flags;
     assert(gba_arm_execute(gba, 0xE1A05000) == 1);
     assert(gba->cpu.r[5] == 0x12345678);
     assert((gba->cpu.cpsr & flags) == flags);

     gba_memory_write32(gba, GBA_IWRAM_BASE, 0xE1A05000);
     gba_disasm(gba, GBA_IWRAM_BASE, 0, text, sizeof(text));
     assert(strcmp(text, "mov r5, r0") == 0);
}

static void test_waitcnt_access_descriptor(struct gba *gba)
{
     struct gba_memory_access access = {
          .addr = 0x08000000,
          .width = GBA_MEMORY_ACCESS_16,
          .source = GBA_MEMORY_ACCESS_CPU,
          .kind = GBA_MEMORY_ACCESS_READ,
          .sequential = false,
     };

     gba->waitcnt = 0;
     assert(gba_memory_wait_cycles(gba, &access) == 4);
     access.sequential = true;
     assert(gba_memory_wait_cycles(gba, &access) == 2);

     access.addr = 0x0A000000;
     assert(gba_memory_wait_cycles(gba, &access) == 4);
     access.addr = 0x0C000000;
     assert(gba_memory_wait_cycles(gba, &access) == 8);
     access.addr = 0x0E000000;
     assert(gba_memory_wait_cycles(gba, &access) == 4);
}

static void test_byte_gamepak_waitstates(struct gba *gba)
{
     gba->waitcnt = 0;
     struct gba_memory_access access = {
          .addr = GBA_ROM_BASE,
          .width = GBA_MEMORY_ACCESS_8,
          .source = GBA_MEMORY_ACCESS_CPU,
          .kind = GBA_MEMORY_ACCESS_READ,
          .sequential = false,
     };
     assert(gba_memory_wait_cycles(gba, &access) == 4);
     access.addr = GBA_SRAM_BASE;
     assert(gba_memory_wait_cycles(gba, &access) == 4);
}

static void test_byte_timer_reload_uses_pending_latch(struct gba *gba)
{
     struct gba_timer_channel *timer = &gba->timer.ch[0];

     gba_timer_reset(gba);
     gba->sync.last_sync[GBA_SYNC_TIMER] = gba->timestamp;
     timer->enable = true;
     timer->reload = 0x1234;

     gba_memory_write8(gba, REG_TM0CNT_L, 0x78);
     assert(timer->reload == 0x1234);
     assert(timer->pending_reload);
     assert(timer->reload_pending == 0x1278);

     gba_memory_write8(gba, REG_TM0CNT_L + 1, 0x56);
     assert(timer->reload == 0x1234);
     assert(timer->pending_reload);
     assert(timer->reload_pending == 0x5678);
}

static void test_memory_descriptor_and_ewram_waitstates(struct gba *gba)
{
     const struct gba_memory_block *block;

     block = gba_memory_block_for_addr(GBA_EWRAM_BASE + GBA_EWRAM_SIZE);
     assert(gba_memory_region_for_addr(GBA_EWRAM_BASE) == GBA_MEMORY_REGION_EWRAM);
     assert(block->base == GBA_EWRAM_BASE);
     assert(block->mirror_mask == GBA_EWRAM_SIZE - 1);
     assert(gba_memory_region_for_addr(GBA_VRAM_BASE) == GBA_MEMORY_REGION_VRAM);
     assert(gba_memory_region_for_addr(GBA_SRAM_BASE) == GBA_MEMORY_REGION_SRAM);

     struct gba_memory_access access = {
          .addr = GBA_EWRAM_BASE,
          .width = GBA_MEMORY_ACCESS_16,
          .source = GBA_MEMORY_ACCESS_CPU,
          .kind = GBA_MEMORY_ACCESS_READ,
          .sequential = false,
     };
     assert(gba_memory_wait_cycles(gba, &access) == 2);
     access.width = GBA_MEMORY_ACCESS_32;
     assert(gba_memory_wait_cycles(gba, &access) == 5);
}

static void test_cpu_open_bus(struct gba *gba)
{
     gba->cpu_bus = 0x44332211;
     assert(gba_memory_read8(gba, 0x01000002) == 0x33);
}

static void test_unused_bios_region_uses_open_bus(struct gba *gba)
{
     gba->cpu_bus = 0x44332211;
     gba->bios_open_bus = 0xDDCCBBAA;

     assert(gba_memory_read8(gba, 0x00004000) == 0x11);
     assert(gba_memory_read16(gba, 0x00004002) == 0x4433);
     assert(gba_memory_read32(gba, 0x00004000) == 0x44332211);
     assert(gba_memory_peek8(gba, 0x00004001) == 0x22);
     assert(gba_memory_peek16(gba, 0x00004002) == 0x4433);
     assert(gba_memory_peek32(gba, 0x00004000) == 0x44332211);
}

static void test_dma_byte_register_access(struct gba *gba)
{
     gba_memory_write8(gba, REG_DMA3SAD, 0x78);
     gba_memory_write8(gba, REG_DMA3SAD + 1, 0x56);
     gba_memory_write8(gba, REG_DMA3SAD + 2, 0x34);
     gba_memory_write8(gba, REG_DMA3SAD + 3, 0x12);
     gba_memory_write8(gba, REG_DMA3DAD, 0xEF);
     gba_memory_write8(gba, REG_DMA3DAD + 1, 0xCD);
     gba_memory_write8(gba, REG_DMA3DAD + 2, 0xAB);
     gba_memory_write8(gba, REG_DMA3DAD + 3, 0x09);
     gba_memory_write8(gba, REG_DMA3CNT_L, 0xEF);
     gba_memory_write8(gba, REG_DMA3CNT_L + 1, 0xBE);

     assert(gba->dma.ch[3].src_latch == 0x02345678);
     assert(gba->dma.ch[3].dst_latch == 0x09ABCDEF);
     assert(gba->dma.ch[3].count_latch == 0xBEEF);

     gba_dma_write_ctrl(gba, 3, 0x7FE0);
     assert(gba_memory_read8(gba, REG_DMA3CNT_H) == 0xE0);
     assert(gba_memory_read8(gba, REG_DMA3CNT_H + 1) == 0x7F);
}

static void test_dma_invalid_source_uses_dma_latch(struct gba *gba)
{
     struct gba_dma_channel *dma = &gba->dma.ch[0];

     gba_dma_reset(gba);
     gba->cpu_bus = 0;
     dma->data_latch = 0xA1B2C3D4;
     gba_dma_write_src(gba, 0, 0x00000000);
     gba_dma_write_dst(gba, 0, GBA_EWRAM_BASE);
     gba_dma_write_count(gba, 0, 1);
     gba_dma_write_ctrl(gba, 0, 0x8400); /* immediate, 32-bit */

     assert(gba_memory_read32(gba, GBA_EWRAM_BASE) == 0xA1B2C3D4);
     assert(gba->cpu_bus == 0xA1B2C3D4);
     assert(gba_memory_read32(gba, 0x01000000) == 0xA1B2C3D4);
}

static void test_peek_does_not_consume_eeprom(struct gba *gba)
{
     struct gba_cart_eeprom *eeprom = &gba->cart.eeprom;

     gba->cart.backup_type = GBA_BACKUP_EEPROM_512;
     eeprom->command = 4;
     eeprom->read_bits_remaining = 64;
     eeprom->read_address = 0;
     eeprom->data[0] = 0x80;

     assert(gba_memory_peek8(gba, 0x0D000000) == 1);
     assert(gba_memory_peek16(gba, 0x0D000000) == 1);
     assert(eeprom->command == 4);
     assert(eeprom->read_bits_remaining == 64);
     assert(eeprom->read_address == 0);
}

static void test_eeprom_ignores_word_writes(struct gba *gba)
{
     struct gba_cart_eeprom *eeprom = &gba->cart.eeprom;

     gba->cart.backup_type = GBA_BACKUP_EEPROM_512;
     eeprom->command = 0;
     eeprom->read_bits_remaining = 0;
     eeprom->write_address = 0;

     gba_memory_write32(gba, 0x0D000000, 0x00000001);

     assert(eeprom->command == 0);
     assert(eeprom->read_bits_remaining == 0);
     assert(eeprom->write_address == 0);
}

static void test_memory_map_mirrors_and_byte_writes(struct gba *gba)
{
     gba_memory_write8(gba, GBA_EWRAM_BASE + GBA_EWRAM_SIZE + 5, 0xA5);
     assert(gba_memory_read8(gba, GBA_EWRAM_BASE + 5) == 0xA5);

     gba_memory_write8(gba, GBA_IWRAM_BASE + GBA_IWRAM_SIZE + 7, 0x5A);
     assert(gba_memory_read8(gba, GBA_IWRAM_BASE + 7) == 0x5A);

     gba_memory_write8(gba, GBA_PAL_BASE + 1, 0x3C);
     assert(gba_memory_read16(gba, GBA_PAL_BASE) == 0x3C3C);

     gba->gpu.bg_mode = 0;
     gba_memory_write8(gba, GBA_VRAM_BASE + 1, 0x96);
     assert(gba_memory_read16(gba, GBA_VRAM_BASE) == 0x9696);
     gba_memory_write16(gba, GBA_VRAM_BASE + 0x18000, 0x55AA);
     assert(gba_memory_read16(gba, GBA_VRAM_BASE + 0x10000) == 0x55AA);

     gba->gpu.bg_mode = 3;
     gba->vram[0x10000] = 0;
     gba->vram[0x10001] = 0;
     gba_memory_write16(gba, GBA_VRAM_BASE + 0x18000, 0xBEEF);
     assert(gba_memory_read16(gba, GBA_VRAM_BASE + 0x18000) == 0);

     gba->oam[0] = 0x12;
     gba->oam[1] = 0x34;
     gba_memory_write8(gba, GBA_OAM_BASE, 0xFF);
     assert(gba_memory_read16(gba, GBA_OAM_BASE) == 0x3412);
}

static void test_sram_access_widths(struct gba *gba)
{
     gba->cart.backup_type = GBA_BACKUP_SRAM;

     gba_memory_write16(gba, GBA_SRAM_BASE, 0xA1B2);
     assert(gba_memory_read8(gba, GBA_SRAM_BASE) == 0xB2);
     assert(gba_memory_read16(gba, GBA_SRAM_BASE) == 0xB2B2);

     gba_memory_write16(gba, GBA_SRAM_BASE + 1, 0xC3D4);
     assert(gba_memory_read8(gba, GBA_SRAM_BASE + 1) == 0xC3);

     gba_memory_write32(gba, GBA_SRAM_BASE + 3, 0x11223344);
     assert(gba_memory_read8(gba, GBA_SRAM_BASE + 3) == 0x11);
     assert(gba_memory_read32(gba, GBA_SRAM_BASE + 3) == 0x11111111);
}

int main(void)
{
     struct gba *gba = gba_create();
     assert(gba);
     gba_reset(gba);
     test_repeated_reset_reinitializes_apu_sync(gba);
     test_reset_preserves_debug_pause_state(gba);
     test_mov_r5_r0(gba);

     test_peek_does_not_sync_timer(gba);
     test_waitcnt_access_descriptor(gba);
     test_byte_gamepak_waitstates(gba);
     test_byte_timer_reload_uses_pending_latch(gba);
     test_memory_descriptor_and_ewram_waitstates(gba);
     test_cpu_open_bus(gba);
     test_unused_bios_region_uses_open_bus(gba);
     test_dma_byte_register_access(gba);
     test_dma_invalid_source_uses_dma_latch(gba);
     test_peek_does_not_consume_eeprom(gba);
     test_eeprom_ignores_word_writes(gba);
     test_memory_map_mirrors_and_byte_writes(gba);
     test_sram_access_widths(gba);

     gba_destroy(gba);
     puts("gba_memory_test: PASS");
     return 0;
}
