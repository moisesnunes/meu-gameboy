/*
 * compat_test.c - headless compatibility ROM runner.
 *
 * It executes one ROM without touching the user's persistent saves, captures
 * serial output, detects common Blargg tilemap text, checks Mooneye register
 * signatures, and can write a final PPM screenshot for visual tests.
 */

#include <errno.h>
#include <inttypes.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "disasm.h"
#include "gb.h"

#define DEFAULT_MAX_CYCLES 100000000ULL
#define RUN_CHUNK_CYCLES 70224
#define SERIAL_MAX 4096

enum compat_expect
{
     EXPECT_AUTO,
     EXPECT_BLARGG,
     EXPECT_MOONEYE,
     EXPECT_FIB,
     EXPECT_GBMICROTEST,
     EXPECT_VISUAL,
};

enum compat_mode
{
     MODE_AUTO,
     MODE_DMG,
     MODE_GBC,
     MODE_DMG0,
     MODE_MGB,
     MODE_SGB,
     MODE_SGB2,
};

struct compat_ctx
{
     uint32_t pixels[GB_LCD_WIDTH * GB_LCD_HEIGHT];
     unsigned frames;
     char serial[SERIAL_MAX];
     size_t serial_len;
};

static const uint32_t dmg_palette[4] = {
     0xffffffff,
     0xffaaaaaa,
     0xff555555,
     0xff000000,
};

static void draw_line_dmg(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
     struct compat_ctx *ctx = gb->frontend.data;
     if (ly >= GB_LCD_HEIGHT)
          return;

     for (unsigned x = 0; x < GB_LCD_WIDTH; x++)
          ctx->pixels[ly * GB_LCD_WIDTH + x] =
              dmg_palette[line[x].dmg_color & 3];
}

static uint32_t cgb_to_xrgb8888(uint16_t c)
{
     uint32_t r = ((c & 0x1f) << 3) | ((c & 0x1f) >> 2);
     uint32_t g = (((c >> 5) & 0x1f) << 3) | (((c >> 5) & 0x1f) >> 2);
     uint32_t b = (((c >> 10) & 0x1f) << 3) | (((c >> 10) & 0x1f) >> 2);
     return 0xff000000 | (r << 16) | (g << 8) | b;
}

static void draw_line_gbc(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
     struct compat_ctx *ctx = gb->frontend.data;
     if (ly >= GB_LCD_HEIGHT)
          return;

     for (unsigned x = 0; x < GB_LCD_WIDTH; x++)
          ctx->pixels[ly * GB_LCD_WIDTH + x] =
              cgb_to_xrgb8888(line[x].gbc_color);
}

static void flip(struct gb *gb)
{
     struct compat_ctx *ctx = gb->frontend.data;
     ctx->frames++;
}

static void refresh_input(struct gb *gb)
{
     (void)gb;
}

static void destroy_frontend(struct gb *gb)
{
     free(gb->frontend.data);
     gb->frontend.data = NULL;
}

static void serial_tx(struct gb *gb, uint8_t byte)
{
     struct compat_ctx *ctx = gb->frontend.data;
     if (ctx->serial_len + 1 >= sizeof(ctx->serial))
          return;

     ctx->serial[ctx->serial_len++] = (char)byte;
     ctx->serial[ctx->serial_len] = '\0';
}

static void drain_spu_buffers(struct gb *gb)
{
     for (unsigned i = 0; i < GB_SPU_SAMPLE_BUFFER_COUNT; i++)
     {
          struct gb_spu_sample_buffer *buf = &gb->spu.buffers[i];
          if (sem_trywait(&buf->ready) == 0)
               sem_post(&buf->free);
     }
}

static void usage(const char *argv0)
{
     fprintf(stderr,
             "Usage: %s [--mode auto|dmg|gbc|dmg0|mgb|sgb|sgb2] [--expect auto|blargg|mooneye|fib|gbmicrotest|visual]\n"
             "          [--max-cycles N] [--ppm path] [--dump-state]\n"
             "          [--bootrom path] [--trace path] <rom.gb|gbc>\n",
             argv0);
}

static void load_bootrom(struct gb *gb, const char *path)
{
     FILE *f = fopen(path, "rb");
     if (!f) { perror(path); return; }
     fseek(f, 0, SEEK_END);
     long size = ftell(f);
     rewind(f);
     if (size != 0x100 && size != 0x900)
     {
          fprintf(stderr, "Boot ROM inválida: tamanho %ld\n", size);
          fclose(f);
          return;
     }
     uint8_t *buf = malloc((size_t)size);
     if (!buf) { perror("malloc"); fclose(f); return; }
     if (fread(buf, 1, (size_t)size, f) != (size_t)size)
     {
          free(buf); fclose(f); return;
     }
     fclose(f);
     free(gb->bootrom);
     gb->bootrom      = buf;
     gb->bootrom_size = (uint32_t)size;
}

static enum compat_mode parse_mode(const char *s)
{
     if (strcmp(s, "dmg") == 0)
          return MODE_DMG;
     if (strcmp(s, "gbc") == 0)
          return MODE_GBC;
     if (strcmp(s, "dmg0") == 0)
          return MODE_DMG0;
     if (strcmp(s, "mgb") == 0)
          return MODE_MGB;
     if (strcmp(s, "sgb") == 0)
          return MODE_SGB;
     if (strcmp(s, "sgb2") == 0)
          return MODE_SGB2;
     return MODE_AUTO;
}

static enum compat_expect parse_expect(const char *s)
{
     if (strcmp(s, "blargg") == 0 || strcmp(s, "serial") == 0 ||
         strcmp(s, "tilemap") == 0)
          return EXPECT_BLARGG;
     if (strcmp(s, "mooneye") == 0)
          return EXPECT_MOONEYE;
     if (strcmp(s, "fib") == 0 || strcmp(s, "registers") == 0 ||
         strcmp(s, "samesuite") == 0)
          return EXPECT_FIB;
     if (strcmp(s, "gbmicrotest") == 0 || strcmp(s, "ff82") == 0)
          return EXPECT_GBMICROTEST;
     if (strcmp(s, "visual") == 0)
          return EXPECT_VISUAL;
     return EXPECT_AUTO;
}

static bool save_ppm(const char *path, const uint32_t *pixels)
{
     FILE *f = fopen(path, "wb");
     if (!f)
     {
          fprintf(stderr, "compat_test: can't open ppm '%s': %s\n",
                  path, strerror(errno));
          return false;
     }

     fprintf(f, "P6\n%d %d\n255\n", GB_LCD_WIDTH, GB_LCD_HEIGHT);
     for (int i = 0; i < GB_LCD_WIDTH * GB_LCD_HEIGHT; i++)
     {
          uint32_t p = pixels[i];
          uint8_t rgb[3] = {
              (uint8_t)((p >> 16) & 0xff),
              (uint8_t)((p >> 8) & 0xff),
              (uint8_t)(p & 0xff),
          };
          fwrite(rgb, 1, sizeof(rgb), f);
     }

     fclose(f);
     return true;
}

static bool serial_contains(struct compat_ctx *ctx, const char *needle)
{
     return strstr(ctx->serial, needle) != NULL;
}

static void append_tilemap_text(struct gb *gb, uint16_t base,
                                char *out, size_t out_size)
{
     size_t n = strlen(out);

     for (unsigned y = 0; y < 32 && n + 2 < out_size; y++)
     {
          for (unsigned x = 0; x < 32 && n + 2 < out_size; x++)
          {
               uint8_t c = gb_memory_peekb(gb, (uint16_t)(base + y * 32 + x));
               out[n++] = (c >= 0x20 && c <= 0x7e) ? (char)c : ' ';
          }
          out[n++] = '\n';
     }

     out[n] = '\0';
}

static bool tilemap_contains(struct gb *gb, const char *needle)
{
     char text[2200];
     text[0] = '\0';
     append_tilemap_text(gb, 0x9800, text, sizeof(text));
     append_tilemap_text(gb, 0x9c00, text, sizeof(text));
     return strstr(text, needle) != NULL;
}

static bool blargg_cart_ram_ready(struct gb *gb)
{
     return gb->cart.ram && gb->cart.ram_length >= 5 &&
            gb->cart.ram[1] == 0xde &&
            gb->cart.ram[2] == 0xb0 &&
            gb->cart.ram[3] == 0x61 &&
            gb->cart.ram[0] != 0x80;
}

static bool blargg_cart_ram_contains(struct gb *gb, const char *needle)
{
     size_t needle_len = strlen(needle);
     const uint8_t *haystack;
     size_t haystack_len;

     if (!gb->cart.ram || gb->cart.ram_length <= 4)
          return false;

     haystack = gb->cart.ram + 4;
     haystack_len = gb->cart.ram_length - 4;
     if (needle_len == 0 || needle_len > haystack_len)
          return false;

     for (size_t i = 0; i <= haystack_len - needle_len; i++)
     {
          if (memcmp(haystack + i, needle, needle_len) == 0)
               return true;
     }

     return false;
}

static bool blargg_cart_ram_has_partial_report(struct gb *gb)
{
     bool saw_first_newline = false;
     bool saw_blank_line = false;

     if (!gb->cart.ram || gb->cart.ram_length <= 4 ||
         gb->cart.ram[1] != 0xde || gb->cart.ram[2] != 0xb0 ||
         gb->cart.ram[3] != 0x61 || gb->cart.ram[0] != 0x80)
     {
          return false;
     }

     for (unsigned i = 4; i < gb->cart.ram_length; i++)
     {
          unsigned char c = gb->cart.ram[i];
          if (c == 0)
               return false;
          if (c < 0x20)
          {
               if (saw_first_newline)
                    saw_blank_line = true;
               saw_first_newline = true;
               continue;
          }
          if (saw_blank_line && c != ' ' && c != '\r' && c != '\t')
               return true;
          saw_first_newline = false;
     }

     return false;
}

static bool mooneye_signature_passed(struct gb *gb)
{
     return gb->cpu.b == 0x03 &&
            gb->cpu.c == 0x05 &&
            gb->cpu.d == 0x08 &&
            gb->cpu.e == 0x0d &&
            gb->cpu.h == 0x15 &&
            gb->cpu.l == 0x22;
}

static bool mooneye_signature_failed(struct gb *gb)
{
     return gb->cpu.b == 0x42 &&
            gb->cpu.c == 0x42 &&
            gb->cpu.d == 0x42 &&
            gb->cpu.e == 0x42 &&
            gb->cpu.h == 0x42 &&
            gb->cpu.l == 0x42;
}

static bool gbmicrotest_passed(struct gb *gb)
{
     return gb_memory_peekb(gb, 0xff82) == 0x01;
}

static bool gbmicrotest_failed(struct gb *gb)
{
     return gb_memory_peekb(gb, 0xff82) == 0xff;
}

static bool mooneye_crash_failed(struct gb *gb)
{
     return gb->cpu.pc == 0x0038 &&
            gb_memory_peekb(gb, gb->cpu.pc) == 0xff;
}

static bool blargg_failed(struct gb *gb, struct compat_ctx *ctx)
{
     return serial_contains(ctx, "Failed") ||
            serial_contains(ctx, "FAILED") ||
            (blargg_cart_ram_ready(gb) && gb->cart.ram[0] != 0) ||
            blargg_cart_ram_contains(gb, "Failed") ||
            blargg_cart_ram_contains(gb, "FAILED") ||
            tilemap_contains(gb, "Failed") ||
            tilemap_contains(gb, "FAILED");
}

static bool blargg_passed(struct gb *gb, struct compat_ctx *ctx)
{
     return serial_contains(ctx, "Passed") ||
            serial_contains(ctx, "PASSED") ||
            (blargg_cart_ram_ready(gb) && gb->cart.ram[0] == 0) ||
            blargg_cart_ram_contains(gb, "Passed") ||
            blargg_cart_ram_contains(gb, "PASSED") ||
            tilemap_contains(gb, "Passed") ||
            tilemap_contains(gb, "PASSED");
}

static void print_serial_escaped(struct compat_ctx *ctx)
{
     putchar('"');
     for (size_t i = 0; i < ctx->serial_len; i++)
     {
          unsigned char c = (unsigned char)ctx->serial[i];
          if (c == '\\' || c == '"')
          {
               putchar('\\');
               putchar(c);
          }
          else if (c == '\n')
               fputs("\\n", stdout);
          else if (c == '\r')
               fputs("\\r", stdout);
          else if (c == '\t')
               fputs("\\t", stdout);
          else if (c >= 0x20 && c <= 0x7e)
               putchar(c);
          else
               printf("\\x%02x", c);
     }
     putchar('"');
}

static void reset_gb(struct gb *gb, const char *rom_path, enum compat_mode mode)
{
     gb_cart_unload(gb);
     gb_cart_load(gb, rom_path);

     if (mode == MODE_DMG)
     {
          gb->gbc = false;
          gb->hw_model = GB_HW_DMG;
     }
     else if (mode == MODE_GBC)
     {
          gb->gbc = true;
          gb->hw_model = GB_HW_CGB;
     }
     else if (mode == MODE_DMG0)
     {
          gb->gbc = false;
          gb->hw_model = GB_HW_DMG0;
     }
     else if (mode == MODE_MGB)
     {
          gb->gbc = false;
          gb->hw_model = GB_HW_MGB;
     }
     else if (mode == MODE_SGB)
     {
          gb->gbc = false;
          gb->hw_model = GB_HW_SGB;
     }
     else if (mode == MODE_SGB2)
     {
          gb->gbc = false;
          gb->hw_model = GB_HW_SGB2;
     }
     else /* MODE_AUTO */
     {
          /* Detect hardware model from ROM filename suffix for hw-specific tests */
          bool is_gbc = gb->cart.rom && gb->cart.rom_length >= 0x144 &&
                        (gb->cart.rom[0x143] == 0x80 || gb->cart.rom[0x143] == 0xc0);
          if (strstr(rom_path, "-dmg0") || strstr(rom_path, "_dmg0"))
          {
               gb->gbc = false;
               gb->hw_model = GB_HW_DMG0;
          }
          else if (strstr(rom_path, "-mgb") || strstr(rom_path, "_mgb"))
          {
               gb->gbc = false;
               gb->hw_model = GB_HW_MGB;
          }
          else if (strstr(rom_path, "-sgb2") || strstr(rom_path, "_sgb2"))
          {
               gb->gbc = false;
               gb->hw_model = GB_HW_SGB2;
          }
          else if (strstr(rom_path, "-sgb") || strstr(rom_path, "_sgb"))
          {
               gb->gbc = false;
               gb->hw_model = GB_HW_SGB;
          }
          else if (strstr(rom_path, "2-S.gb"))
          {
               gb->gbc = false;
               gb->hw_model = GB_HW_SGB2;
          }
          else if (strstr(rom_path, "-S.gb"))
          {
               gb->gbc = false;
               gb->hw_model = GB_HW_SGB;
          }
          else if (is_gbc)
          {
               gb->gbc = true;
               gb->hw_model = GB_HW_CGB;
          }
          else
          {
               gb->gbc = false;
               gb->hw_model = GB_HW_DMG;
          }
     }

     if (gb->cart.ram && gb->cart.ram_length > 0)
          memset(gb->cart.ram, 0, gb->cart.ram_length);
     if (gb->cart.has_eeprom)
          memset(gb->cart.mbc7.eeprom, 0xff, sizeof(gb->cart.mbc7.eeprom));
     if (gb->cart.has_rtc)
          gb_rtc_init(gb);
     free(gb->cart.save_file);
     gb->cart.save_file = NULL;
     gb->cart.dirty_ram = false;

     gb->speed_switch_pending = false;
     gb->double_speed = false;
     gb->timestamp = 0;
     gb->serial_data = 0x00;
     gb->serial_control = 0x7e;
     gb->serial_tx = serial_tx;
     gb->iram_high_bank = 1;
     gb->vram_high_bank = false;
     gb->ir_port = 0;

     memset(gb->iram, 0xff, sizeof(gb->iram));
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

     gb_debug_init(gb);
     gb->debug.enabled = false;
     gb->debug.state = GB_DEBUG_RUNNING;
}

int main(int argc, char **argv)
{
     const char *rom_path = NULL;
     const char *ppm_path = NULL;
     const char *trace_path = NULL;
     const char *bootrom_path = NULL;
     enum compat_mode mode = MODE_AUTO;
     enum compat_expect expect = EXPECT_AUTO;
     uint64_t max_cycles = DEFAULT_MAX_CYCLES;
     bool dump_state = false;

     for (int i = 1; i < argc; i++)
     {
          if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc)
               mode = parse_mode(argv[++i]);
          else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc)
               expect = parse_expect(argv[++i]);
          else if (strcmp(argv[i], "--max-cycles") == 0 && i + 1 < argc)
               max_cycles = strtoull(argv[++i], NULL, 10);
          else if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc)
               ppm_path = argv[++i];
          else if (strcmp(argv[i], "--dump-state") == 0)
               dump_state = true;
          else if (strcmp(argv[i], "--bootrom") == 0 && i + 1 < argc)
               bootrom_path = argv[++i];
          else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc)
               trace_path = argv[++i];
          else if (!rom_path)
               rom_path = argv[i];
          else
          {
               usage(argv[0]);
               return 2;
          }
     }

     if (!rom_path)
     {
          usage(argv[0]);
          return 2;
     }

     struct gb *gb = calloc(1, sizeof(*gb));
     struct compat_ctx *ctx = calloc(1, sizeof(*ctx));
     if (!gb || !ctx)
     {
          perror("calloc");
          free(ctx);
          free(gb);
          return 2;
     }

     gb->frontend.data = ctx;
     gb->frontend.draw_line_dmg = draw_line_dmg;
     gb->frontend.draw_line_gbc = draw_line_gbc;
     gb->frontend.flip = flip;
     gb->frontend.refresh_input = refresh_input;
     gb->frontend.destroy = destroy_frontend;

     if (bootrom_path)
          load_bootrom(gb, bootrom_path);
     reset_gb(gb, rom_path, mode);
     if (trace_path)
          gb_debug_trace_set_enabled(gb, true, trace_path);
     if (!gb->cart.rom)
     {
          printf("LOAD_FAIL\t%s\n", rom_path);
          destroy_frontend(gb);
          free(gb);
          return 2;
     }

     const uint64_t target_cycles = max_cycles;
     uint64_t cycles = 0;
     const char *result = NULL;

     while (cycles < target_cycles && !gb->quit)
     {
          int32_t chunk = RUN_CHUNK_CYCLES;
          if (target_cycles - cycles < (uint64_t)chunk)
               chunk = (int32_t)(target_cycles - cycles);

          gb_cpu_run_cycles(gb, chunk);
          drain_spu_buffers(gb);
          cycles += (uint32_t)chunk;

          if (expect != EXPECT_MOONEYE && expect != EXPECT_VISUAL &&
              blargg_failed(gb, ctx))
          {
               result = "FAIL";
               break;
          }
          if (expect == EXPECT_GBMICROTEST && gbmicrotest_failed(gb))
          {
               result = "FAIL";
               break;
          }
          if (expect != EXPECT_BLARGG && expect != EXPECT_VISUAL &&
              mooneye_signature_failed(gb))
          {
               result = "FAIL";
               break;
          }
          if (expect != EXPECT_BLARGG && expect != EXPECT_VISUAL &&
              mooneye_crash_failed(gb))
          {
               result = "FAIL";
               break;
          }
          if (expect != EXPECT_MOONEYE && expect != EXPECT_VISUAL &&
              blargg_passed(gb, ctx))
          {
               result = "PASS";
               break;
          }
          if (expect == EXPECT_GBMICROTEST && gbmicrotest_passed(gb))
          {
               result = "PASS";
               break;
          }
          if (expect != EXPECT_BLARGG && expect != EXPECT_VISUAL &&
              mooneye_signature_passed(gb))
          {
               result = "PASS";
               break;
          }
     }

     if (!result)
          result = (expect == EXPECT_VISUAL) ? "VISUAL" : "TIMEOUT";

     if (strcmp(result, "TIMEOUT") == 0 && expect != EXPECT_MOONEYE &&
         blargg_cart_ram_has_partial_report(gb))
     {
          result = "FAIL";
     }

     if (ppm_path && !save_ppm(ppm_path, ctx->pixels) && strcmp(result, "PASS") == 0)
          result = "FAIL";

     printf("%s\t%s\tcycles=%" PRIu64 "\tframes=%u\tserial=",
            result, rom_path, cycles, ctx->frames);
     print_serial_escaped(ctx);
     if (ppm_path)
          printf("\tppm=%s", ppm_path);
     putchar('\n');

     if (dump_state)
     {
          fprintf(stderr,
                  "PC=%04x SP=%04x AF=%02x%c%c%c%c BC=%02x%02x DE=%02x%02x HL=%02x%02x LY=%02x\n",
                  gb->cpu.pc, gb->cpu.sp, gb->cpu.a,
                  gb->cpu.f_z ? 'Z' : '-',
                  gb->cpu.f_n ? 'N' : '-',
                  gb->cpu.f_h ? 'H' : '-',
                  gb->cpu.f_c ? 'C' : '-',
                  gb->cpu.b, gb->cpu.c, gb->cpu.d, gb->cpu.e,
                  gb->cpu.h, gb->cpu.l, gb->gpu.ly);
          fprintf(stderr, "hram FF80-FF9F=");
          for (unsigned i = 0; i < 0x20; i++)
          {
               fprintf(stderr, "%s%02x", i ? " " : "", gb->zram[i]);
          }
          fputc('\n', stderr);
          if (gb->cart.ram && gb->cart.ram_length >= 16)
          {
               fprintf(stderr,
                       "cart_ram A000=%02x magic=%02x %02x %02x text=\"",
                       gb->cart.ram[0], gb->cart.ram[1],
                       gb->cart.ram[2], gb->cart.ram[3]);
               for (unsigned i = 4; i < gb->cart.ram_length && i < 84; i++)
               {
                    unsigned char c = gb->cart.ram[i];
                    if (c == 0)
                         break;
                    fputc((c >= 0x20 && c <= 0x7e) ? c : '.', stderr);
               }
               fputs("\"\n", stderr);
          }
          fprintf(stderr, "last %d PCs:\n", GB_CPU_TRACE_SIZE);
          for (int i = 0; i < GB_CPU_TRACE_SIZE; i++)
          {
               unsigned slot = (gb->cpu.trace_head + i) % GB_CPU_TRACE_SIZE;
               uint16_t pc = gb->cpu.trace_buf[slot];
               char text[64];
               gb_disasm(gb, pc, text, sizeof(text));
               fprintf(stderr, "  %04x  %s\n", pc, text);
          }
     }

     gb_cart_unload(gb);
     destroy_frontend(gb);
     free(gb);

     if (strcmp(result, "PASS") == 0 || strcmp(result, "VISUAL") == 0)
          return 0;
     if (strcmp(result, "TIMEOUT") == 0)
          return 124;
     return 1;
}
