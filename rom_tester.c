/*
 * rom_tester.c — Testa uma ROM em modo headless e salva um screenshot PPM.
 *
 * Uso: rom_tester [--bootrom <path>] [--frames N] [--out <dir>]
 *                  [--timeout N] <rom.gb|gbc>
 *
 * Saída (stdout, uma linha):
 *   LOAD_FAIL <rom>           — gb_cart_load falhou (ROM inválida/desconhecida)
 *   RUN_TIMEOUT <rom>        — não completou os frames dentro do timeout interno
 *   BLANK     <rom> <ppm>    — rodou N frames, frame final todo preto/paleta DMG
 *   OK        <rom> <ppm>    — rodou N frames, há pixels fora da paleta/preto
 *
 * A detecção BLANK vs OK verifica se TODOS os pixels do frame final estão
 * dentro das 4 cores da paleta DMG verde ou são preto puro (0x00000000).
 * Um jogo que realmente renderizou algo terá pixels fora disso (ou cores GBC).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "gb.h"

/* Timeout em segundos: se o rom_tester não terminar em X segundos, SIGALRM
 * seta timed_out e o loop principal encerra — evita travar em ROMs bugadas. */
#define DEFAULT_TIMEOUT_SECONDS 30

static volatile sig_atomic_t timed_out = 0;
static unsigned timeout_seconds = DEFAULT_TIMEOUT_SECONDS;

static void handle_alarm(int sig)
{
    (void)sig;
    timed_out = 1;
}

/* ── Constantes ── */

#define DEFAULT_FRAMES 600

/* Ciclos por frame: 4194304 / 59.7275 ≈ 70224 */
#define CYCLES_PER_FRAME 70224U
#define RUN_CHUNK_CYCLES 2048U

/* Paleta DMG verde — pixels dessas cores são considerados "em branco/padrão" */
static const uint32_t DMG_PALETTE[4] = {
    0xFF75A32C,
    0xFF387A21,
    0xFF255116,
    0xFF12280B,
};

/* ── Contexto headless ── */

struct headless_ctx {
    uint32_t pixels[GB_LCD_WIDTH * GB_LCD_HEIGHT];
    unsigned frame_count;
    /* O SPU precisa de semáforos mesmo em modo headless; drenamos os buffers
     * aqui para evitar que sem_wait() bloqueie indefinidamente. */
};

/* ── Callbacks de vídeo (headless) ── */

static void draw_line_dmg(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
    struct headless_ctx *ctx = gb->frontend.data;
    if (ly >= GB_LCD_HEIGHT) return;
    for (unsigned i = 0; i < GB_LCD_WIDTH; i++)
        ctx->pixels[ly * GB_LCD_WIDTH + i] = DMG_PALETTE[line[i].dmg_color & 3];
}

static void draw_line_gbc(struct gb *gb, unsigned ly,
                          union gb_gpu_color line[GB_LCD_WIDTH])
{
    struct headless_ctx *ctx = gb->frontend.data;
    if (ly >= GB_LCD_HEIGHT) return;
    for (unsigned i = 0; i < GB_LCD_WIDTH; i++) {
        uint16_t c = line[i].gbc_color;
        uint32_t r = ((c & 0x1f) << 3) | ((c & 0x1f) >> 2);
        uint32_t g = (((c >> 5) & 0x1f) << 3) | (((c >> 5) & 0x1f) >> 2);
        uint32_t b = (((c >> 10) & 0x1f) << 3) | (((c >> 10) & 0x1f) >> 2);
        ctx->pixels[ly * GB_LCD_WIDTH + i] = 0xFF000000 | (r << 16) | (g << 8) | b;
    }
}

static void flip(struct gb *gb)
{
    struct headless_ctx *ctx = gb->frontend.data;
    ctx->frame_count++;
}

static void refresh_input(struct gb *gb) { (void)gb; }

static void destroy_frontend(struct gb *gb)
{
    free(gb->frontend.data);
    gb->frontend.data = NULL;
}

/* ── Drena buffers SPU prontos para não travar sem_wait ── */

static void drain_spu_buffers(struct gb *gb)
{
    struct gb_spu *spu = &gb->spu;
    for (unsigned i = 0; i < GB_SPU_SAMPLE_BUFFER_COUNT; i++) {
        struct gb_spu_sample_buffer *buf = &spu->buffers[i];
        /* Se o buffer está pronto (sem_trywait == 0), liberamos ele. */
        if (sem_trywait(&buf->ready) == 0)
            sem_post(&buf->free);
    }
}

/* ── Salva PPM ── */

static bool save_ppm(const char *path, const uint32_t *pixels)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return false; }

    fprintf(f, "P6\n%d %d\n255\n", GB_LCD_WIDTH, GB_LCD_HEIGHT);
    for (int i = 0; i < GB_LCD_WIDTH * GB_LCD_HEIGHT; i++) {
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

static bool mkdir_p(const char *path)
{
    char tmp[4096];
    size_t len;

    if (!path || !*path)
        return true;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
            perror(tmp);
            return false;
        }
        *p = '/';
    }

    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
        perror(tmp);
        return false;
    }
    return true;
}

/* ── Verifica se o frame é "em branco" ──
 *
 * Critério: o frame é BLANK se todos os pixels têm a mesma cor
 * (tela uniforme — preto puro, branco DMG, etc.).
 * Um jogo que renderizou algo real terá ao menos 2 cores distintas.
 *
 * Para GBC: qualquer variação entre pixels é suficiente.
 * Para DMG: os 4 níveis de cinza são válidos — variação entre eles = OK.
 */
static bool frame_is_blank(const uint32_t *pixels)
{
    uint32_t first = pixels[0];
    for (int i = 1; i < GB_LCD_WIDTH * GB_LCD_HEIGHT; i++) {
        if (pixels[i] != first)
            return false;
    }
    return true;
}

/* ── Reset do emulador ── */

static void reset_gb(struct gb *gb, const char *rom_path)
{
    gb_cart_unload(gb);
    gb_cart_load(gb, rom_path);

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
    gb->double_speed         = false;
    gb->timestamp            = 0;
    gb->serial_data          = 0x00;
    gb->serial_control       = 0x7e;
    gb->iram_high_bank       = 1;
    gb->vram_high_bank       = false;
    gb->ir_port              = 0;

    memset(gb->iram,  0, sizeof(gb->iram));
    memset(gb->zram,  0, sizeof(gb->zram));
    memset(gb->vram,  0, sizeof(gb->vram));
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

    struct headless_ctx *ctx = gb->frontend.data;
    if (ctx) ctx->frame_count = 0;

    gb_debug_init(gb);
    gb->debug.enabled = false;
    gb->debug.state   = GB_DEBUG_RUNNING;
}

/* ── main ── */

int main(int argc, char **argv)
{
    const char *rom_file     = NULL;
    const char *bootrom_file = NULL;
    const char *out_dir      = ".";
    unsigned    frames       = DEFAULT_FRAMES;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bootrom") == 0 && i + 1 < argc)
            bootrom_file = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out_dir = argv[++i];
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout_seconds = (unsigned)atoi(argv[++i]);
        else if (!rom_file)
            rom_file = argv[i];
        else {
            fprintf(stderr, "Uso: rom_tester [--bootrom <p>] [--frames N] [--out <dir>] [--timeout N] <rom>\n");
            return EXIT_FAILURE;
        }
    }

    if (!rom_file) {
        fprintf(stderr, "Uso: rom_tester [--bootrom <p>] [--frames N] [--out <dir>] [--timeout N] <rom>\n");
        return EXIT_FAILURE;
    }

    if (!mkdir_p(out_dir)) {
        fprintf(stderr, "rom_tester: failed to create output dir '%s'\n", out_dir);
        return EXIT_FAILURE;
    }

    /* Inicializa emulador */
    struct gb *gb = calloc(1, sizeof(*gb));
    if (!gb) { perror("calloc"); return EXIT_FAILURE; }

    struct headless_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { perror("calloc"); free(gb); return EXIT_FAILURE; }

    gb->frontend.data        = ctx;
    gb->frontend.draw_line_dmg = draw_line_dmg;
    gb->frontend.draw_line_gbc = draw_line_gbc;
    gb->frontend.flip          = flip;
    gb->frontend.refresh_input = refresh_input;
    gb->frontend.destroy       = destroy_frontend;

    /* Boot ROM opcional */
    if (bootrom_file) {
        FILE *f = fopen(bootrom_file, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f); rewind(f);
            if (sz == 0x100 || sz == 0x900) {
                gb->bootrom = malloc((size_t)sz);
                if (gb->bootrom) {
                    if (fread(gb->bootrom, 1, (size_t)sz, f) == (size_t)sz)
                        gb->bootrom_size = (uint32_t)sz;
                    else { free(gb->bootrom); gb->bootrom = NULL; }
                }
            }
            fclose(f);
        }
    }

    /* Instala timeout via SIGALRM */
    signal(SIGALRM, handle_alarm);
    alarm(timeout_seconds);

    /* Carrega ROM */
    reset_gb(gb, rom_file);

    if (!gb->cart.rom) {
        printf("LOAD_FAIL\t%s\n", rom_file);
        free(gb->bootrom);
        destroy_frontend(gb);
        free(gb);
        return 0;
    }

    /* Extrai nome base da ROM para o PPM */
    const char *base = rom_file;
    for (const char *p = rom_file; *p; p++)
        if (*p == '/') base = p + 1;

    char ppm_path[4096];
    /* Remove extensão do nome base para o arquivo PPM */
    char base_noext[512];
    snprintf(base_noext, sizeof(base_noext), "%s", base);
    char *dot = strrchr(base_noext, '.');
    if (dot) *dot = '\0';
    snprintf(ppm_path, sizeof(ppm_path), "%s/%s.ppm", out_dir, base_noext);

    /* Executa N frames em modo headless (sem throttle — vai o mais rápido possível).
     * Rodar em blocos menores torna o timeout e o dreno de áudio responsivos mesmo
     * quando uma ROM deixa o emulador preso dentro de um frame. */
    unsigned target_frames = frames;
    while (ctx->frame_count < target_frames && !gb->quit && !timed_out) {
        unsigned frame_cycles = 0;
        unsigned frame_start = ctx->frame_count;

        while (frame_cycles < CYCLES_PER_FRAME &&
               ctx->frame_count == frame_start &&
               !gb->quit && !timed_out) {
            unsigned step = CYCLES_PER_FRAME - frame_cycles;
            if (step > RUN_CHUNK_CYCLES)
                step = RUN_CHUNK_CYCLES;
            gb_cpu_run_cycles(gb, step);
            frame_cycles += step;
            drain_spu_buffers(gb);
        }

        /* A tela pode estar desligada ou sem flips; ainda assim avançamos por
         * frame lógico para que o timeout continue sendo o limite de segurança. */
        if (ctx->frame_count == frame_start)
            ctx->frame_count++;
        drain_spu_buffers(gb);
    }
    alarm(0); /* cancela alarme se terminou antes do timeout */

    /* ROMs que não completaram os frames dentro do timeout interno. */
    if (timed_out) {
        printf("RUN_TIMEOUT\t%s\n", rom_file);
        gb_cart_unload(gb);
        free(gb->bootrom);
        destroy_frontend(gb);
        free(gb);
        return 0;
    }

    /* Salva screenshot */
    save_ppm(ppm_path, ctx->pixels);

    /* Classifica resultado */
    bool blank = frame_is_blank(ctx->pixels);

    if (blank)
        printf("BLANK\t%s\t%s\n", rom_file, ppm_path);
    else
        printf("OK\t%s\t%s\n", rom_file, ppm_path);

    /* Cleanup */
    gb_cart_unload(gb);
    free(gb->bootrom);
    destroy_frontend(gb);
    free(gb);
    return 0;
}
