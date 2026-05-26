#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "gba.h"

struct gba *gba_create(void)
{
     struct gba *gba = calloc(1, sizeof(*gba));
     if (!gba)
          return NULL;
     gba->hw_model = GBA_HW_AGB;
     return gba;
}

void gba_destroy(struct gba *gba)
{
     if (!gba)
          return;
     gba_cart_unload(gba);
     free(gba->bios);
     free(gba);
}

bool gba_load_bios(struct gba *gba, const char *path)
{
     FILE *f = fopen(path, "rb");
     if (!f)
     {
          perror(path);
          return false;
     }

     fseek(f, 0, SEEK_END);
     long size = ftell(f);
     fseek(f, 0, SEEK_SET);

     if (size != (long)GBA_BIOS_SIZE)
     {
          fprintf(stderr, "GBA BIOS: expected %u bytes, got %ld\n",
                  GBA_BIOS_SIZE, size);
          fclose(f);
          return false;
     }

     free(gba->bios);
     gba->bios = malloc(GBA_BIOS_SIZE);
     if (!gba->bios)
     {
          fclose(f);
          return false;
     }
     fread(gba->bios, 1, GBA_BIOS_SIZE, f);
     fclose(f);
     return true;
}

void gba_reset(struct gba *gba)
{
     gba->timestamp = 0;
     gba->quit = false;
     gba->halt_mode = 0;
     gba->halt_resume_cycles = 0;
     gba->postflg = 0;
     gba->waitcnt = 0;
     gba->bios_open_bus = 0;
     gba->bios_open_bus_after_read = 0;
     gba->bios_open_bus_has_after_read = false;
     gba->bios_irq_hle_active = false;
     gba->bios_irq_hle_return_r15 = 0;
     memset(gba->bios_irq_hle_regs, 0, sizeof(gba->bios_irq_hle_regs));
     gba->bios_irq_hle_cpsr = 0;
     gba->bios_intr_wait_active = false;
     gba->bios_intr_wait_mask = 0;

     gba_memory_reset(gba);
     gba_sync_reset(gba);
     gba_irq_reset(gba);
     gba_cpu_reset(gba);
     gba_debug_reset(gba);
     gba_gpu_reset(gba);
     gba_apu_reset(gba);
     gba_dma_reset(gba);
     gba_timer_reset(gba);
     gba_input_reset(gba);

     /* If no BIOS, skip to cartridge entrypoint */
     if (!gba->bios)
     {
          struct gba_cpu *cpu = &gba->cpu;
          /* Simulate post-BIOS state: SYS mode, ROM entry at 0x08000000 */
          cpu->cpsr = (uint32_t)GBA_MODE_SYS;
          cpu->r[13] = cpu->r13_usr = 0x03007F00;
          cpu->r13_irq = 0x03007FA0;
          cpu->r13_svc = 0x03007FE0;
          /* r[15] invariant: r[15] = rom_entry + 8 */
          cpu->r[15] = 0x08000000 + 8;
          gba->postflg = 1;
          gba->bios_open_bus = 0xE129F000;

          /* Mirror the hardware state the BIOS leaves behind */
          gba->irq.ime = 1;
          /* WAITCNT: WS0 first=3 (N=4 seq=1), WS1 first=3, WS2 first=3, prefetch on */
          gba->waitcnt = 0x4317;
          /* APU master enable */
          gba->apu.soundcnt_x = 0x80;

          /*
           * BG2/BG3 affine matrix identity: the BIOS writes PA=0x0100, PD=0x0100
           * (1.0 in 8.8 fixed-point) so rotated/scaled BGs render at correct size.
           * Without this, games using mode 1/2 get a collapsed image.
           */
          gba->gpu.bg[2].pa = 0x0100;
          gba->gpu.bg[2].pd = 0x0100;
          gba->gpu.bg[3].pa = 0x0100;
          gba->gpu.bg[3].pd = 0x0100;

          /*
           * The BIOS copies its own header bytes to IWRAM at 0x03007FF0.
           * Some games read 0x03007FF0 to confirm BIOS ran; write a non-zero
           * sentinel so they don't assume the system is in a broken state.
           * The real BIOS leaves 0x00 there, so we just leave it zeroed.
           * What matters more: 0x03007FFC = interrupt handler pointer (0 = no handler).
           */
     }
}

void gba_run_frame(struct gba *gba)
{
     /* Run until GPU signals end of VBlank (one full frame = 280896 cycles) */
     int32_t frame_end = gba->timestamp + GBA_CYCLES_PER_FRAME;

     while (!gba->quit && gba->timestamp < frame_end)
     {
          int32_t next = gba->sync.first_event;
          if (next > frame_end)
               next = frame_end;

          /* Run CPU until next event or end of frame */
          while (!gba->quit && gba->timestamp < next)
          {
               int cycles;

               if (gba->debug.enabled && gba->debug.state == GBA_DEBUG_PAUSED)
                    break;

               if (gba->halt_mode && !(gba->irq.ie & gba->irq.if_))
               {
                    /* Halted: jump to next event */
                    gba->timestamp = next;
                    break;
               }

               cycles = gba_cpu_step(gba);
               if (cycles <= 0)
                    break;
               gba->timestamp += cycles;
          }

          if (gba->debug.enabled && gba->debug.state == GBA_DEBUG_PAUSED)
               break;

          /* Process pending events */
          gba_sync_check_events(gba);

          /* Prevent timestamp overflow */
          if (gba->timestamp > 0x70000000)
               gba_sync_rebase(gba);
     }
}
