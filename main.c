#include <string.h>
#include <stdio.h>
#include <SDL3/SDL.h>
#include "gb.h"
#include "sdl.h"
#include "debug.h"
#include "debug_ui.h"

/* Carrega o arquivo de boot ROM em gb->bootrom (NULL se falhar) */
static void load_bootrom(struct gb *gb, const char *path)
{
     FILE *f = fopen(path, "rb");
     if (!f) { perror(path); return; }

     fseek(f, 0, SEEK_END);
     long size = ftell(f);
     rewind(f);

     if (size != 0x100 && size != 0x900)
     {
          fprintf(stderr, "Boot ROM inválida: tamanho %ld (esperado 256 ou 2304 bytes)\n", size);
          fclose(f);
          return;
     }

     uint8_t *buf = malloc((size_t)size);
     if (!buf) { perror("malloc"); fclose(f); return; }

     if (fread(buf, 1, (size_t)size, f) != (size_t)size)
     {
          fprintf(stderr, "Erro ao ler boot ROM\n");
          free(buf);
          fclose(f);
          return;
     }
     fclose(f);

     free(gb->bootrom);
     gb->bootrom      = buf;
     gb->bootrom_size = (uint32_t)size;
     printf("Boot ROM carregada: %s (%ld bytes)\n", path, size);
}

/* Carrega um ROM e reseta todo o estado do emulador */
static void load_rom(struct gb *gb, const char *path)
{
     gb_cart_unload(gb);
     gb_cart_load(gb, path);

     gb->speed_switch_pending = false;
     gb->double_speed = false;
     gb->timestamp = 0;
     gb->serial_data = 0x00;
     gb->serial_control = 0x7e;
     gb->iram_high_bank = 1;
     gb->vram_high_bank = false;
     gb->ir_port = 0;
     gb->cgb_reg_ff72 = 0;
     gb->cgb_reg_ff73 = 0;
     gb->cgb_reg_ff75 = 0;

     memset(gb->iram, 0, sizeof(gb->iram));
     memset(gb->zram, 0, sizeof(gb->zram));
     memset(gb->vram, 0, sizeof(gb->vram));
     memset(&gb->hdma, 0, sizeof(gb->hdma));
     gb->hdma.length = 0x7f;

     gb_sync_reset(gb);
     gb_irq_reset(gb);
     gb_cpu_reset(gb);
     gb_gpu_reset(gb);
     gb_input_reset(gb);
     gb_dma_reset(gb);
     gb_timer_reset(gb);
     gb_spu_reset(gb);
     gb_sdl_reset_audio_buffer(gb);
     gb_debug_init(gb);
}

int main(int argc, char **argv)
{
     const char *rom_file     = NULL;
     const char *bootrom_file = NULL;
     bool        debug_flag   = false;

     for (int i = 1; i < argc; i++)
     {
          if (strcmp(argv[i], "--debug") == 0)
               debug_flag = true;
          else if (strcmp(argv[i], "--bootrom") == 0 && i + 1 < argc)
               bootrom_file = argv[++i];
          else if (!rom_file)
               rom_file = argv[i];
     }

     struct gb *gb = calloc(1, sizeof(*gb));
     if (!gb) { perror("calloc"); return EXIT_FAILURE; }

     /* Inicialização base dos semáforos de áudio; gb_spu_reset fará destroy+init
      * a cada ROM carregada para garantir estado limpo. */
     for (unsigned i = 0; i < GB_SPU_SAMPLE_BUFFER_COUNT; i++)
     {
          struct gb_spu_sample_buffer *buf = &gb->spu.buffers[i];
          sem_init(&buf->free,  0, 1);
          sem_init(&buf->ready, 0, 0);
     }

     gb_sdl_frontend_init(gb);
     debug_ui_init(gb);

     if (bootrom_file)
          load_bootrom(gb, bootrom_file);

     if (rom_file)
          load_rom(gb, rom_file);

     gb->debug.enabled = debug_flag;
     gb->debug.hw_trace.enabled = debug_flag; /* HW Trace on by default when debug mode is on */
     if (rom_file)
     {
          bool pause_on_load = gb->debug.enabled && debug_ui_start_paused();
          gb->debug.state = pause_on_load ? GB_DEBUG_PAUSED : GB_DEBUG_RUNNING;
     }

     gb->quit        = false;
     gb->double_speed = false;
     gb->speed_switch_pending = false;

     uint64_t last_ns = SDL_GetTicksNS();

     while (!gb->quit)
     {
          SDL_Event e;
          while (SDL_PollEvent(&e))
          {
               gb_sdl_process_event(gb, &e);
               debug_ui_process_event(gb, &e);
          }

          /* Verifica se o usuário escolheu uma ROM via file dialog ou drag-and-drop */
          const char *pending = debug_ui_pending_rom();
          if (pending)
          {
               load_rom(gb, pending);
               debug_ui_clear_pending_rom(pending);
               if (debug_ui_start_paused())
               {
                    gb->debug.enabled = true;
                    gb->debug.state = GB_DEBUG_PAUSED;
               }
               else
               {
                    gb->debug.state = GB_DEBUG_RUNNING;
               }
               last_ns = SDL_GetTicksNS(); /* reinicia o timer ao trocar de ROM */
          }

          debug_ui_render(gb);

          /* Só avança a emulação se há ROM carregada e não está pausada */
          if (gb->cart.rom &&
              !(gb->debug.enabled && gb->debug.state == GB_DEBUG_PAUSED))
          {
               uint64_t now_ns  = SDL_GetTicksNS();
               uint64_t elapsed = now_ns - last_ns;
               last_ns = now_ns;

               /* Limita a 50 ms para evitar spiral-of-death após perda de foco */
               if (elapsed > 50000000ULL) elapsed = 50000000ULL;

               float ff = debug_ui_get_speed_multiplier();
               int32_t cycles = (int32_t)((float)(elapsed * (uint64_t)GB_CPU_FREQ_HZ / 1000000000ULL) * ff);
               if (cycles > 0)
                    gb_cpu_run_cycles(gb, cycles);
          }
          else
          {
               SDL_Delay(8); /* evita spin-loop sem ROM ou pausado */
               last_ns = SDL_GetTicksNS();
          }
     }

     debug_ui_destroy(gb);
     gb->frontend.destroy(gb);
     gb_cart_unload(gb);
     free(gb->bootrom);
     free(gb);

     return 0;
}
