/*
 * gba_compat_test.c — Headless ROM runner para o GBA.
 *
 * Executa uma ROM GBA sem janela, detecta resultados dos test frameworks
 * endrift/gba-tests e emite uma linha de saída TAB-separada compatível
 * com o runner shell (análogo ao compat_test.c GB).
 *
 * Uso:
 *   gba_compat_test [--bios <path>] [--max-cycles N] [--ppm <path>] [--dump-state]
 *                   [--trace <path>] [--trace-limit N] [--trace-after-cycles N]
 *                   [--break-pc ADDR]
 *                   [--break-after-cycles N] [--dump-mem ADDR:LEN]
 *                   [--input KEY:DOWN_FRAME:UP_FRAME]
 *                   [--expect auto|endrift|visual] <rom.gba>
 *
 * Saída (stdout, uma linha):
 *   PASS|FAIL|TIMEOUT|VISUAL<TAB><rom><TAB>cycles=N<TAB>frames=N
 *
 * Exit code:
 *   0  = PASS ou VISUAL
 *   1  = FAIL
 *   124 = TIMEOUT
 *   2  = erro de carga
 */

#include <errno.h>
#include <inttypes.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "gba/gba.h"
#include "gba/gba_disasm.h"

/* ── Configuração padrão ── */

#define DEFAULT_MAX_CYCLES  500000000ULL   /* ~30 s de tempo de jogo */

/* ── Tipo de expectativa ── */

enum gba_expect {
    EXPECT_AUTO,
    EXPECT_ENDRIFT,   /* endrift/gba-tests: resultado em EWRAM / registradores */
    EXPECT_VISUAL,
};

/* ── Contexto do frontend headless ── */

struct compat_ctx {
    uint32_t pixels[GBA_LCD_W * GBA_LCD_H];
    unsigned frames;
};

#define MAX_INPUT_EVENTS 16

struct input_event {
    unsigned key;
    unsigned down_frame;
    unsigned up_frame;
    bool active;
};

/* ── Frontend callbacks headless ── */

static void draw_line(void *data, uint8_t line, const uint16_t *src)
{
    struct compat_ctx *ctx = data;
    if (line >= GBA_LCD_H) return;
    uint32_t *row = ctx->pixels + (unsigned)line * GBA_LCD_W;
    for (unsigned i = 0; i < GBA_LCD_W; i++) {
        uint16_t c = src[i];
        uint32_t r = ((c & 0x001f) << 3) | ((c & 0x001f) >> 2);
        uint32_t g = (((c >> 5) & 0x1f) << 3) | (((c >> 5) & 0x1f) >> 2);
        uint32_t b = (((c >> 10) & 0x1f) << 3) | (((c >> 10) & 0x1f) >> 2);
        row[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}

static void flip(void *data)
{
    struct compat_ctx *ctx = data;
    ctx->frames++;
}

static void refresh_input(void *data) { (void)data; }
static void destroy_frontend(void *data) { free(data); }

/* ── Drena buffers APU para não bloquear semáforos ── */

static void drain_apu(struct gba *gba)
{
    struct gba_apu *apu = &gba->apu;
    if (sem_trywait(&apu->buf_ready) == 0)
        sem_post(&apu->buf_free);
}

static void reset_volatile_backup(struct gba *gba)
{
    struct gba_cart *cart = &gba->cart;
    if (cart->save_file) {
        fclose(cart->save_file);
        cart->save_file = NULL;
    }
    memset(cart->sram, 0xFF, sizeof(cart->sram));
    memset(cart->flash, 0xFF, sizeof(cart->flash));
    memset(&cart->flash_state, 0, sizeof(cart->flash_state));
    memset(cart->eeprom.data, 0xFF, sizeof(cart->eeprom.data));
    cart->eeprom.command = 0;
    cart->eeprom.read_bits_remaining = 0;
    cart->eeprom.read_address = 0;
    cart->eeprom.write_address = 0;
    cart->eeprom.settling_until = 0;
    cart->flash_state.manufacturer = 0x32;
    cart->flash_state.device = 0x1B;
    cart->dirty = false;
}

/* ── Detecção de resultado (endrift/gba-tests) ── */

/*
 * endrift/gba-tests escreve o resultado em EWRAM 0x02000000:
 *   bytes 0-3: magic "GBAT" (0x47 0x42 0x41 0x54)
 *   byte  4:   status — 0x00 = running, 0x01 = pass, 0xFF = fail
 */
static bool endrift_result_ready(struct gba *gba)
{
    const uint8_t *ew = gba->ewram;
    return ew[0] == 0x47 && ew[1] == 0x42 && ew[2] == 0x41 && ew[3] == 0x54
           && ew[4] != 0x00;
}

static bool endrift_passed(struct gba *gba)
{
    return gba->ewram[4] == 0x01;
}

/* ── Detecção visual dos gba-tests-master ── */

struct visual_result {
    bool matched;
    bool passed;
    int failed_test;
};

static const uint32_t test_glyphs[96][2] = {
    {0x00000000,0x00000000},{0x18181818,0x00180018},{0x00003636,0x00000000},{0x367F3636,0x0036367F},
    {0x3C067C18,0x00183E60},{0x1B356600,0x0033566C},{0x6E16361C,0x00DE733B},{0x000C1818,0x00000000},
    {0x0C0C1830,0x0030180C},{0x3030180C,0x000C1830},{0xFF3C6600,0x0000663C},{0x7E181800,0x00001818},
    {0x00000000,0x0C181800},{0x7E000000,0x00000000},{0x00000000,0x00181800},{0x183060C0,0x0003060C},
    {0x7E76663C,0x003C666E},{0x181E1C18,0x00181818},{0x3060663C,0x007E0C18},{0x3860663C,0x003C6660},
    {0x33363C38,0x0030307F},{0x603E067E,0x003C6660},{0x3E060C38,0x003C6666},{0x3060607E,0x00181818},
    {0x3C66663C,0x003C6666},{0x7C66663C,0x001C3060},{0x00181800,0x00181800},{0x00181800,0x0C181800},
    {0x06186000,0x00006018},{0x007E0000,0x0000007E},{0x60180600,0x00000618},{0x3060663C,0x00180018},
    {0x5A5A663C,0x003C067A},{0x7E66663C,0x00666666},{0x3E66663E,0x003E6666},{0x06060C78,0x00780C06},
    {0x6666361E,0x001E3666},{0x1E06067E,0x007E0606},{0x1E06067E,0x00060606},{0x7606663C,0x007C6666},
    {0x7E666666,0x00666666},{0x1818183C,0x003C1818},{0x60606060,0x003C6660},{0x0F1B3363,0x0063331B},
    {0x06060606,0x007E0606},{0x6B7F7763,0x00636363},{0x7B6F6763,0x00636373},{0x6666663C,0x003C6666},
    {0x3E66663E,0x00060606},{0x3333331E,0x007E3B33},{0x3E66663E,0x00666636},{0x3C0E663C,0x003C6670},
    {0x1818187E,0x00181818},{0x66666666,0x003C6666},{0x66666666,0x00183C3C},{0x6B636363,0x0063777F},
    {0x183C66C3,0x00C3663C},{0x183C66C3,0x00181818},{0x0C18307F,0x007F0306},{0x0C0C0C3C,0x003C0C0C},
    {0x180C0603,0x00C06030},{0x3030303C,0x003C3030},{0x00663C18,0x00000000},{0x00000000,0x003F0000},
    {0x00301818,0x00000000},{0x603C0000,0x007C667C},{0x663E0606,0x003E6666},{0x063C0000,0x003C0606},
    {0x667C6060,0x007C6666},{0x663C0000,0x003C067E},{0x0C3E0C38,0x000C0C0C},{0x667C0000,0x3C607C66},
    {0x663E0606,0x00666666},{0x18180018,0x00301818},{0x30300030,0x1E303030},{0x36660606,0x0066361E},
    {0x18181818,0x00301818},{0x7F370000,0x0063636B},{0x663E0000,0x00666666},{0x663C0000,0x003C6666},
    {0x663E0000,0x06063E66},{0x667C0000,0x60607C66},{0x663E0000,0x00060606},{0x063C0000,0x003E603C},
    {0x0C3E0C0C,0x00380C0C},{0x66660000,0x007C6666},{0x66660000,0x00183C66},{0x63630000,0x00367F6B},
    {0x36630000,0x0063361C},{0x66660000,0x0C183C66},{0x307E0000,0x007E0C18},{0x0C181830,0x00301818},
    {0x18181818,0x00181818},{0x3018180C,0x000C1818},{0x003B6E00,0x00000000},{0x00000000,0x00000000},
};

static bool glyph_bit(char ch, unsigned x, unsigned y)
{
    if ((unsigned char)ch < 32 || (unsigned char)ch > 127) return false;
    const uint32_t word = test_glyphs[(unsigned char)ch - 32][y >= 4];
    const unsigned bit = (y & 3) * 8 + x;
    return (word >> bit) & 1U;
}

static bool pixel_is_dark(uint32_t pixel)
{
    unsigned r = (pixel >> 16) & 0xff;
    unsigned g = (pixel >>  8) & 0xff;
    unsigned b =  pixel        & 0xff;
    return r + g + b < 384;
}

static unsigned glyph_mismatches(const struct compat_ctx *ctx, int x, int y,
                                 char ch, bool foreground_dark)
{
    if (x < 0 || y < 0 || x + 8 > GBA_LCD_W || y + 8 > GBA_LCD_H)
        return 64;

    unsigned mismatches = 0;
    for (unsigned gy = 0; gy < 8; gy++) {
        for (unsigned gx = 0; gx < 8; gx++) {
            bool on = glyph_bit(ch, gx, gy);
            bool expected_dark = foreground_dark ? on : !on;
            bool dark = pixel_is_dark(ctx->pixels[(y + gy) * GBA_LCD_W + x + gx]);
            if (dark != expected_dark)
                mismatches++;
        }
    }
    return mismatches;
}

static unsigned text_mismatches(const struct compat_ctx *ctx, int x, int y,
                                const char *text, bool foreground_dark)
{
    unsigned mismatches = 0;
    for (size_t i = 0; text[i]; i++)
        mismatches += glyph_mismatches(ctx, x + (int)i * 8, y, text[i], foreground_dark);
    return mismatches;
}

static unsigned best_text_mismatches(const struct compat_ctx *ctx, int x, int y,
                                     const char *text)
{
    unsigned dark = text_mismatches(ctx, x, y, text, true);
    unsigned light = text_mismatches(ctx, x, y, text, false);
    return dark < light ? dark : light;
}

static unsigned best_text_mismatches_near(const struct compat_ctx *ctx,
                                          int x0, int y0, int x1, int y1,
                                          const char *text)
{
    unsigned best = 0xFFFFFFFFU;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            unsigned score = best_text_mismatches(ctx, x, y, text);
            if (score < best)
                best = score;
        }
    }
    return best;
}

static int best_digit_at(const struct compat_ctx *ctx, int x, int y,
                         bool foreground_dark, unsigned *best_mismatches)
{
    int best = 0;
    unsigned best_score = 65;
    for (int d = 0; d <= 9; d++) {
        unsigned score = glyph_mismatches(ctx, x, y, (char)('0' + d), foreground_dark);
        if (score < best_score) {
            best_score = score;
            best = d;
        }
    }
    *best_mismatches = best_score;
    return best;
}

static bool read_failed_digits(const struct compat_ctx *ctx, int *test_no)
{
    const int x = 60 + 12 * 8;
    const int y = 76;
    unsigned total_dark = 0;
    unsigned total_light = 0;
    int dark_digits[3];
    int light_digits[3];

    for (int i = 0; i < 3; i++) {
        unsigned score = 0;
        dark_digits[i] = best_digit_at(ctx, x + i * 8, y, true, &score);
        total_dark += score;
        light_digits[i] = best_digit_at(ctx, x + i * 8, y, false, &score);
        total_light += score;
    }

    int *digits = total_dark <= total_light ? dark_digits : light_digits;
    unsigned total = total_dark <= total_light ? total_dark : total_light;
    if (total > 12)
        return false;

    *test_no = digits[0] * 100 + digits[1] * 10 + digits[2];
    return true;
}

static struct visual_result classify_visual_result(const struct compat_ctx *ctx)
{
    struct visual_result out = {.failed_test = -1};
    const unsigned pass_score = best_text_mismatches(ctx, 56, 76, "All tests passed");
    const unsigned fail_score = best_text_mismatches(ctx, 60, 76, "Failed test ");
    const unsigned fuzzarm_pass_score =
        best_text_mismatches_near(ctx, 0, 0, 16, 16, "End of testing");
    const unsigned fuzzarm_fail_score =
        best_text_mismatches_near(ctx, 0, 0, 16, 16, "Failed test ");
    const unsigned hwtest_pass_score =
        best_text_mismatches_near(ctx, 0, 0, 32, 80, "congratulations!");
    const unsigned hwtest_fail_score =
        best_text_mismatches_near(ctx, 0, 0, 160, 120, "FAIL");

    if (pass_score <= 16) {
        out.matched = true;
        out.passed = true;
        return out;
    }

    if (fuzzarm_pass_score <= 16) {
        out.matched = true;
        out.passed = true;
        return out;
    }

    if (hwtest_pass_score <= 20) {
        out.matched = true;
        out.passed = true;
        return out;
    }

    if (fail_score <= 12 && read_failed_digits(ctx, &out.failed_test)) {
        out.matched = true;
        out.passed = false;
        return out;
    }

    if (fuzzarm_fail_score <= 12) {
        out.matched = true;
        out.passed = false;
        return out;
    }

    if (hwtest_fail_score <= 8) {
        out.matched = true;
        out.passed = false;
    }
    return out;
}

/* ── PPM screenshot ── */

static bool save_ppm(const char *path, const uint32_t *pixels)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "gba_compat_test: cannot open '%s': %s\n",
                path, strerror(errno));
        return false;
    }
    fprintf(f, "P6\n%d %d\n255\n", GBA_LCD_W, GBA_LCD_H);
    for (int i = 0; i < GBA_LCD_W * GBA_LCD_H; i++) {
        uint32_t p = pixels[i];
        uint8_t rgb[3] = {
            (uint8_t)((p >> 16) & 0xff),
            (uint8_t)((p >>  8) & 0xff),
            (uint8_t)( p        & 0xff),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    return true;
}

/* ── Uso ── */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Uso: %s [--bios <path>] [--max-cycles N] [--ppm <path>]\n"
            "        [--trace <path>] [--trace-limit N] [--trace-after-cycles N]\n"
            "        [--break-pc ADDR]\n"
            "        [--break-after-cycles N] [--dump-state] [--dump-mem ADDR:LEN]\n"
            "        [--input KEY:DOWN_FRAME:UP_FRAME]\n"
            "        [--expect auto|endrift|visual] <rom.gba>\n",
            argv0);
}

static bool parse_dump_mem(const char *s, uint32_t *addr, uint32_t *len)
{
    char *end = NULL;
    unsigned long a = strtoul(s, &end, 0);
    if (end == s || *end != ':')
        return false;

    char *len_start = end + 1;
    unsigned long n = strtoul(len_start, &end, 0);
    if (end == len_start || *end != '\0' || n > 0x10000UL)
        return false;

    *addr = (uint32_t)a;
    *len = (uint32_t)n;
    return true;
}

static bool parse_key_name(const char *name, unsigned *key)
{
    static const struct {
        const char *name;
        unsigned key;
    } keys[] = {
        {"A", GBA_KEY_A},
        {"B", GBA_KEY_B},
        {"SELECT", GBA_KEY_SELECT},
        {"START", GBA_KEY_START},
        {"RIGHT", GBA_KEY_RIGHT},
        {"LEFT", GBA_KEY_LEFT},
        {"UP", GBA_KEY_UP},
        {"DOWN", GBA_KEY_DOWN},
        {"R", GBA_KEY_R},
        {"L", GBA_KEY_L},
    };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        if (strcasecmp(name, keys[i].name) == 0) {
            *key = keys[i].key;
            return true;
        }
    }
    return false;
}

static bool parse_input_event(const char *spec, struct input_event *event)
{
    char buf[64];
    char *key_name;
    char *down;
    char *up;

    if (strlen(spec) >= sizeof(buf))
        return false;
    strcpy(buf, spec);

    key_name = strtok(buf, ":");
    down = strtok(NULL, ":");
    up = strtok(NULL, ":");
    if (!key_name || !down || !up || strtok(NULL, ":"))
        return false;

    if (!parse_key_name(key_name, &event->key))
        return false;
    event->down_frame = (unsigned)strtoul(down, NULL, 10);
    event->up_frame = (unsigned)strtoul(up, NULL, 10);
    event->active = false;
    return event->up_frame > event->down_frame;
}

static void apply_input_events(struct gba *gba, struct input_event *events,
                               unsigned count, unsigned frame)
{
    for (unsigned i = 0; i < count; i++) {
        if (!events[i].active && frame >= events[i].down_frame &&
            frame < events[i].up_frame) {
            gba_input_set(gba, events[i].key, true);
            events[i].active = true;
        } else if (events[i].active && frame >= events[i].up_frame) {
            gba_input_set(gba, events[i].key, false);
            events[i].active = false;
        }
    }
}

/* ── main ── */

int main(int argc, char **argv)
{
    const char  *rom_path  = NULL;
    const char  *bios_path = NULL;
    const char  *ppm_path  = NULL;
    const char  *trace_path = NULL;
    FILE        *trace_fp = NULL;
    uint64_t     max_cycles = DEFAULT_MAX_CYCLES;
    uint64_t     trace_limit = 0;
    uint64_t     trace_after_cycles = 0;
    enum gba_expect expect  = EXPECT_AUTO;
    bool         dump_state = false;
    bool         debug_enabled = false;
    bool         breakpoints_armed = false;
    uint64_t     break_after_cycles = 0;
    bool         dump_mem = false;
    uint32_t     dump_mem_addr = 0;
    uint32_t     dump_mem_len = 0;
    uint32_t     breakpoints[GBA_DEBUG_MAX_BREAKPOINTS];
    unsigned     breakpoint_count = 0;
    struct input_event input_events[MAX_INPUT_EVENTS];
    unsigned input_event_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bios") == 0 && i + 1 < argc)
            bios_path = argv[++i];
        else if (strcmp(argv[i], "--max-cycles") == 0 && i + 1 < argc)
            max_cycles = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--ppm") == 0 && i + 1 < argc)
            ppm_path = argv[++i];
        else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            trace_path = argv[++i];
            debug_enabled = true;
        }
        else if (strcmp(argv[i], "--trace-limit") == 0 && i + 1 < argc) {
            trace_limit = strtoull(argv[++i], NULL, 10);
            debug_enabled = true;
        }
        else if (strcmp(argv[i], "--trace-after-cycles") == 0 && i + 1 < argc) {
            trace_after_cycles = strtoull(argv[++i], NULL, 10);
            debug_enabled = true;
        }
        else if (strcmp(argv[i], "--break-pc") == 0 && i + 1 < argc) {
            if (breakpoint_count >= GBA_DEBUG_MAX_BREAKPOINTS) {
                usage(argv[0]);
                return 2;
            }
            breakpoints[breakpoint_count++] = (uint32_t)strtoul(argv[++i], NULL, 16);
            debug_enabled = true;
        }
        else if (strcmp(argv[i], "--break-after-cycles") == 0 && i + 1 < argc) {
            break_after_cycles = strtoull(argv[++i], NULL, 10);
            debug_enabled = true;
        }
        else if (strcmp(argv[i], "--dump-state") == 0)
            dump_state = true;
        else if (strcmp(argv[i], "--dump-mem") == 0 && i + 1 < argc) {
            if (!parse_dump_mem(argv[++i], &dump_mem_addr, &dump_mem_len)) {
                usage(argv[0]);
                return 2;
            }
            dump_state = true;
            dump_mem = true;
        }
        else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            if (input_event_count >= MAX_INPUT_EVENTS ||
                !parse_input_event(argv[++i], &input_events[input_event_count++])) {
                usage(argv[0]);
                return 2;
            }
        }
        else if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
            const char *s = argv[++i];
            if (strcmp(s, "endrift") == 0)
                expect = EXPECT_ENDRIFT;
            else if (strcmp(s, "visual") == 0)
                expect = EXPECT_VISUAL;
            else
                expect = EXPECT_AUTO;
        } else if (!rom_path)
            rom_path = argv[i];
        else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!rom_path) { usage(argv[0]); return 2; }

    if (trace_path) {
        trace_fp = fopen(trace_path, "w");
        if (!trace_fp) {
            fprintf(stderr, "gba_compat_test: cannot open trace '%s': %s\n",
                    trace_path, strerror(errno));
            return 2;
        }
    }

    /* ── Cria e inicializa GBA ── */

    struct gba *gba = gba_create();
    if (!gba) {
        if (trace_fp)
            fclose(trace_fp);
        perror("gba_create");
        return 2;
    }

    struct compat_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        if (trace_fp)
            fclose(trace_fp);
        perror("calloc");
        gba_destroy(gba);
        return 2;
    }

    gba->frontend.data          = ctx;
    gba->frontend.draw_line     = draw_line;
    gba->frontend.flip          = flip;
    gba->frontend.refresh_input = refresh_input;
    gba->frontend.destroy       = destroy_frontend;

    if (bios_path)
        gba_load_bios(gba, bios_path);

    if (!gba_cart_load(gba, rom_path)) {
        printf("LOAD_FAIL\t%s\n", rom_path);
        if (trace_fp)
            fclose(trace_fp);
        destroy_frontend(ctx);
        gba_destroy(gba);
        return 2;
    }
    reset_volatile_backup(gba);

    gba_reset(gba);

    if (debug_enabled) {
        gba->debug.enabled = true;
        gba->debug.state = GBA_DEBUG_RUNNING;
        gba->debug.trace_enabled =
            trace_after_cycles == 0 && (trace_path != NULL || trace_limit != 0);
        gba->debug.trace_limit = trace_limit;
        gba->debug.trace_fp = trace_fp;
        if (break_after_cycles == 0) {
            for (unsigned i = 0; i < breakpoint_count; i++)
                gba_debug_add_breakpoint(gba, breakpoints[i]);
            breakpoints_armed = true;
        }
    }

    /* ── Loop principal ── */

    uint64_t    cycles = 0;
    const char *result = NULL;
    struct visual_result visual = {0};

    while (cycles < max_cycles && !gba->quit) {
        int32_t before = gba->timestamp;
        apply_input_events(gba, input_events, input_event_count, ctx->frames);
        if (debug_enabled && !gba->debug.trace_enabled && trace_after_cycles != 0 &&
            (uint64_t)gba->timestamp >= trace_after_cycles)
            gba->debug.trace_enabled = trace_path != NULL || trace_limit != 0;
        if (debug_enabled && !breakpoints_armed &&
            (uint64_t)gba->timestamp >= break_after_cycles) {
            for (unsigned i = 0; i < breakpoint_count; i++)
                gba_debug_add_breakpoint(gba, breakpoints[i]);
            breakpoints_armed = true;
        }
        gba_run_frame(gba);
        drain_apu(gba);
        cycles = (uint64_t)gba->timestamp;
        if (gba->timestamp == before && !gba->quit) {
            result = (gba->debug.enabled && gba->debug.state == GBA_DEBUG_PAUSED)
                         ? "BREAK"
                         : "TIMEOUT";
            break;
        }

        /* Verifica resultado endrift */
        if (expect != EXPECT_VISUAL && endrift_result_ready(gba)) {
            result = endrift_passed(gba) ? "PASS" : "FAIL";
            break;
        }
    }

    if (!result && expect == EXPECT_VISUAL) {
        visual = classify_visual_result(ctx);
        if (visual.matched)
            result = visual.passed ? "PASS" : "FAIL";
    }

    if (!result)
        result = (expect == EXPECT_VISUAL) ? "VISUAL" : "TIMEOUT";

    if (trace_fp) {
        fclose(trace_fp);
        gba->debug.trace_fp = NULL;
    }

    /* ── Gera PPM se pedido ── */

    if (ppm_path && !save_ppm(ppm_path, ctx->pixels) &&
        strcmp(result, "PASS") == 0)
        result = "FAIL";

    printf("%s\t%s\tcycles=%" PRIu64 "\tframes=%u",
           result, rom_path, cycles, ctx->frames);
    if (ppm_path)
        printf("\tppm=%s", ppm_path);
    if (visual.matched) {
        if (visual.passed)
            printf("\tvisual=pass_text");
        else {
            printf("\tvisual=fail_text");
            if (visual.failed_test >= 0)
                printf("\ttest=%03d", visual.failed_test);
        }
    }
    putchar('\n');

    if (dump_state) {
        uint32_t pc = gba_cpu_current_pc(&gba->cpu);
        uint8_t trace_head = gba->cpu.trace_head;
        fprintf(stderr,
                "STATE pc=%08x r15=%08x cpsr=%08x "
                "r0=%08x r1=%08x r2=%08x r3=%08x "
                "r4=%08x r5=%08x r6=%08x r7=%08x "
                "r8=%08x r9=%08x r10=%08x r11=%08x "
                "r12=%08x sp=%08x lr=%08x "
                "timestamp=%d first_event=%d "
                "vcount=%u vblank=%u hblank=%u debug_enabled=%u debug_state=%d "
                "instr=%llu pram0=%02x%02x pram1=%02x%02x dispcnt=%02x%02x "
                "ie=%04x if=%04x ime=%u halt=%u cpu_halted=%u "
                "bios_irq=%u bios_irq_ret=%08x bios_irq_cpsr=%08x "
                "irq_handler=%08x "
                "tm0_counter=%04x tm0_reload=%04x tm0_ctrl=%02x tm0_acc=%d "
                "tm0_pending=%u tm0_reload_delay=%d\n",
                pc, gba->cpu.r[15], gba->cpu.cpsr,
                gba->cpu.r[0], gba->cpu.r[1], gba->cpu.r[2], gba->cpu.r[3],
                gba->cpu.r[4], gba->cpu.r[5], gba->cpu.r[6], gba->cpu.r[7],
                gba->cpu.r[8], gba->cpu.r[9], gba->cpu.r[10], gba->cpu.r[11],
                gba->cpu.r[12], gba->cpu.r[13], gba->cpu.r[14],
                gba->timestamp,
                gba->sync.first_event, gba->gpu.vcount, gba->gpu.vblank,
                gba->gpu.hblank, gba->debug.enabled, gba->debug.state,
                (unsigned long long)gba->debug.instruction_count,
                gba->pram[1], gba->pram[0], gba->pram[3], gba->pram[2],
                gba_memory_peek8(gba, REG_DISPCNT + 1),
                gba_memory_peek8(gba, REG_DISPCNT),
                gba->irq.ie, gba->irq.if_, gba->irq.ime,
                gba->halt_mode, gba->cpu.halted,
                gba->bios_irq_hle_active,
                gba->bios_irq_hle_return_r15,
                gba->bios_irq_hle_cpsr,
                gba_memory_peek32(gba, 0x03007FFC),
                gba->timer.ch[0].counter,
                gba->timer.ch[0].reload,
                (unsigned)(gba->timer.ch[0].prescaler |
                           (gba->timer.ch[0].cascade << 2) |
                           (gba->timer.ch[0].irq_en << 6) |
                           (gba->timer.ch[0].enable << 7)),
                gba->timer.ch[0].cycles_acc,
                gba->timer.ch[0].pending_reload,
                gba->timer.ch[0].reload_delay);
        fputs("TRACE", stderr);
        for (unsigned i = 0; i < GBA_TRACE_SIZE; i++) {
            uint8_t idx = (uint8_t)((trace_head + i) & (GBA_TRACE_SIZE - 1));
            fprintf(stderr, " %08x", gba->cpu.trace_buf[idx]);
        }
        fputc('\n', stderr);
        fputs("TRACE_DISASM\n", stderr);
        for (unsigned i = 0; i < GBA_TRACE_SIZE; i++) {
            uint8_t idx = (uint8_t)((trace_head + i) & (GBA_TRACE_SIZE - 1));
            uint8_t next_idx = (uint8_t)((trace_head + i + 1) & (GBA_TRACE_SIZE - 1));
            uint32_t trace_pc = gba->cpu.trace_buf[idx];
            uint32_t next_pc = gba->cpu.trace_buf[next_idx];
            bool trace_thumb = (next_pc == trace_pc + 2) ||
                               ((trace_pc & 0xFE000000U) == 0x02000000U &&
                                (next_pc & 0xFE000000U) == 0x02000000U &&
                                next_pc != trace_pc + 4);
            char buf[128];
            gba_disasm(gba, trace_pc, trace_thumb, buf, sizeof(buf));
            fprintf(stderr, "  %08x  %-5s %s\n",
                    trace_pc, trace_thumb ? "THUMB" : "ARM", buf);
        }
        /* Disassemble IRQ handler from IWRAM */
        {
            uint32_t handler = gba_memory_peek32(gba, 0x03007FFC);
            if (handler >= 0x03000000 && handler < 0x03008000) {
                fprintf(stderr, "HANDLER_DISASM @%08X (ARM):\n", handler);
                uint32_t pc = handler;
                for (int i = 0; i < 24 && pc < handler + 96; i++) {
                    char buf[128];
                    gba_disasm(gba, pc, 0, buf, sizeof(buf));
                    uint32_t w = gba_memory_peek32(gba, pc);
                    fprintf(stderr, "  %08X: %08X  %s\n", pc, w, buf);
                    unsigned len = gba_disasm_len(gba, pc, 0);
                    pc += len ? len : 4;
                }
            }
        }
        if (dump_mem) {
            fprintf(stderr, "MEMDUMP addr=%08x len=%u\n", dump_mem_addr, dump_mem_len);
            for (uint32_t off = 0; off < dump_mem_len; off += 16) {
                uint32_t chunk = dump_mem_len - off;
                if (chunk > 16)
                    chunk = 16;

                fprintf(stderr, "  %08x:", dump_mem_addr + off);
                for (uint32_t i = 0; i < chunk; i++)
                    fprintf(stderr, " %02x", gba_memory_peek8(gba, dump_mem_addr + off + i));
                fputc('\n', stderr);
            }
        }
    }

    /* ── Cleanup ── */

    gba_cart_unload(gba);
    destroy_frontend(ctx);
    gba->frontend.data = NULL;
    gba_destroy(gba);

    if (strcmp(result, "PASS") == 0 || strcmp(result, "VISUAL") == 0 ||
        strcmp(result, "BREAK") == 0) return 0;
    if (strcmp(result, "TIMEOUT") == 0) return 124;
    return 1;
}
