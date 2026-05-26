#include <string.h>
#include <stdlib.h>
#include "gba.h"

/* Duty cycle waveforms (8 steps, same as GB) */
static const uint8_t duty_table[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, /* 12.5% */
    {1, 0, 0, 0, 0, 0, 0, 1}, /* 25%   */
    {1, 0, 0, 0, 1, 1, 0, 1}, /* 50%   */
    {0, 1, 1, 1, 1, 1, 1, 0}, /* 75%   */
};

/* Period in CPU cycles for square channels: (2048 - freq) * 4 * 4 */
static inline uint32_t ch_square_period(uint16_t freq)
{
    return (uint32_t)(2048 - freq) * 16;
}

/* Period in CPU cycles for CH3 wave: (2048 - freq) * 2 * 4 (samples clocked 2× faster) */
static inline uint32_t ch3_period(uint16_t freq)
{
    return (uint32_t)(2048 - freq) * 8;
}

/* CH4 noise period table */
static const uint32_t ch4_div_table[8] = {8, 16, 32, 48, 64, 80, 96, 112};

static inline uint32_t ch4_period(uint8_t clk_shift, uint8_t div_ratio)
{
    uint32_t base = ch4_div_table[div_ratio & 7];
    return base << (clk_shift & 0xF);
}

void gba_apu_reset(struct gba *gba)
{
    struct gba_apu *apu = &gba->apu;
    memset(apu, 0, sizeof(*apu));

    apu->soundbias = 0x200;
    apu->soundcnt_x = 0x80; /* master enable on by default */

    sem_init(&apu->buf_free, 0, 2);
    sem_init(&apu->buf_ready, 0, 0);

    apu->next_sample_cycles = GBA_CPU_FREQ_HZ / GBA_APU_SAMPLE_RATE;
    gba_sync_next(gba, GBA_SYNC_APU, apu->next_sample_cycles);
}

/* =========================================================================
 * Frame sequencer (512 Hz, 8 steps)
 *  step 0: length
 *  step 1: length
 *  step 2: length + sweep
 *  step 3: length
 *  step 4: length
 *  step 5: length
 *  step 6: length + sweep
 *  step 7: length + envelope
 * ========================================================================= */

static void clock_length(struct gba_apu_length *len, bool *enabled)
{
    if (!len->enabled || !*enabled)
        return;
    if (len->counter > 0) {
        len->counter--;
        if (len->counter == 0)
            *enabled = false;
    }
}

static void clock_envelope(struct gba_apu_envelope *env, bool channel_enabled)
{
    if (!channel_enabled || env->period == 0)
        return;
    if (env->counter > 0)
        env->counter--;
    if (env->counter == 0) {
        env->counter = env->period;
        if (env->increase) {
            if (env->volume < 15) env->volume++;
        } else {
            if (env->volume > 0)  env->volume--;
        }
    }
}

static void clock_sweep(struct gba *gba)
{
    struct gba_apu_ch1 *ch = &gba->apu.ch1;
    if (!ch->enabled || ch->sweep.period == 0)
        return;
    if (ch->sweep.counter > 0)
        ch->sweep.counter--;
    if (ch->sweep.counter == 0) {
        ch->sweep.counter = ch->sweep.period ? ch->sweep.period : 8;
        if (ch->sweep.period > 0) {
            uint16_t delta = ch->sweep.shadow_freq >> ch->sweep.shift;
            uint16_t new_freq = ch->sweep.negate
                ? ch->sweep.shadow_freq - delta
                : ch->sweep.shadow_freq + delta;
            if (new_freq >= 2048) {
                ch->enabled = false;
            } else if (ch->sweep.shift > 0) {
                ch->sweep.shadow_freq = new_freq;
                ch->freq = new_freq;
                ch->div.period = (uint16_t)ch_square_period(new_freq);
                /* overflow check again */
                delta = new_freq >> ch->sweep.shift;
                if (ch->sweep.negate ? 0 : (new_freq + delta) >= 2048)
                    ch->enabled = false;
            }
        }
    }
}

static void frame_seq_clock(struct gba *gba)
{
    struct gba_apu *apu = &gba->apu;
    uint8_t step = apu->frame_seq_step;

    /* Length at every step */
    clock_length(&apu->ch1.length, &apu->ch1.enabled);
    clock_length(&apu->ch2.length, &apu->ch2.enabled);
    clock_length(&apu->ch3.length, &apu->ch3.enabled);
    clock_length(&apu->ch4.length, &apu->ch4.enabled);

    /* Sweep at steps 2 and 6 */
    if (step == 2 || step == 6)
        clock_sweep(gba);

    /* Envelope at step 7 */
    if (step == 7) {
        clock_envelope(&apu->ch1.env, apu->ch1.enabled);
        clock_envelope(&apu->ch2.env, apu->ch2.enabled);
        clock_envelope(&apu->ch4.env, apu->ch4.enabled);
    }

    apu->frame_seq_step = (step + 1) & 7;
}

/* =========================================================================
 * FIFO helpers
 * ========================================================================= */

static void fifo_push_byte(struct gba_apu_fifo *f, int8_t b)
{
    if (f->len < 32) {
        f->buf[(f->head + f->len) & 31] = b;
        f->len++;
    }
}

static int8_t fifo_pop(struct gba_apu_fifo *f)
{
    if (f->len == 0)
        return f->sample;
    int8_t b = f->buf[f->head];
    f->head = (f->head + 1) & 31;
    f->len--;
    f->sample = b;
    return b;
}

void gba_apu_fifo_push(struct gba *gba, int fifo, uint32_t word)
{
    struct gba_apu_fifo *f = (fifo == 0) ? &gba->apu.fifo_a : &gba->apu.fifo_b;
    fifo_push_byte(f, (int8_t)(word & 0xFF));
    fifo_push_byte(f, (int8_t)((word >> 8) & 0xFF));
    fifo_push_byte(f, (int8_t)((word >> 16) & 0xFF));
    fifo_push_byte(f, (int8_t)((word >> 24) & 0xFF));
}

void gba_apu_fifo_tick(struct gba *gba, int timer)
{
    struct gba_apu *apu = &gba->apu;
    if (apu->fifo_a.timer_sel == (uint8_t)timer) {
        apu->fifo_a.sample = fifo_pop(&apu->fifo_a);
        if (apu->fifo_a.len <= 16)
            gba_dma_notify_fifo(gba, 0);
    }
    if (apu->fifo_b.timer_sel == (uint8_t)timer) {
        apu->fifo_b.sample = fifo_pop(&apu->fifo_b);
        if (apu->fifo_b.len <= 16)
            gba_dma_notify_fifo(gba, 1);
    }
}

/* =========================================================================
 * Register write — CH1 (SOUND1CNT_L/H/X = 0x60/62/64)
 * ========================================================================= */

static void ch1_write(struct gba *gba, uint32_t addr, uint8_t val)
{
    struct gba_apu_ch1 *ch = &gba->apu.ch1;
    switch (addr) {
    case REG_SOUND1CNT_L: /* NR10: sweep */
        ch->sweep.period = (val >> 4) & 7;
        ch->sweep.negate = (val >> 3) & 1;
        ch->sweep.shift  = val & 7;
        break;
    case REG_SOUND1CNT_H: /* NR11: duty + length */
        ch->duty = (val >> 6) & 3;
        ch->length.counter = 64 - (val & 0x3F);
        break;
    case REG_SOUND1CNT_H + 1: /* NR12: envelope */
        ch->env.initial_volume = (val >> 4) & 0xF;
        ch->env.increase       = (val >> 3) & 1;
        ch->env.period         = val & 7;
        ch->dac_on             = (val & 0xF8) != 0;
        if (!ch->dac_on)
            ch->enabled = false;
        break;
    case REG_SOUND1CNT_X: /* NR13: freq low */
        ch->freq = (ch->freq & 0x700) | val;
        ch->div.period = (uint16_t)ch_square_period(ch->freq);
        break;
    case REG_SOUND1CNT_X + 1: /* NR14: freq high + trigger + length enable */
        ch->freq = (ch->freq & 0x0FF) | ((uint16_t)(val & 7) << 8);
        ch->div.period    = (uint16_t)ch_square_period(ch->freq);
        ch->length.enabled = (val >> 6) & 1;
        if (val & 0x80) { /* trigger */
            if (ch->dac_on)
                ch->enabled = true;
            if (ch->length.counter == 0)
                ch->length.counter = 64;
            ch->env.volume  = ch->env.initial_volume;
            ch->env.counter = ch->env.period;
            ch->div.counter = ch->div.period;
            ch->duty_pos    = 0;
            ch->sweep.shadow_freq = ch->freq;
            ch->sweep.counter     = ch->sweep.period ? ch->sweep.period : 8;
            if (ch->sweep.period == 0 && ch->sweep.shift == 0)
                ch->sweep.counter = 0;
            /* overflow check on trigger */
            if (ch->sweep.shift > 0) {
                uint16_t delta = ch->sweep.shadow_freq >> ch->sweep.shift;
                if (!ch->sweep.negate && (ch->sweep.shadow_freq + delta) >= 2048)
                    ch->enabled = false;
            }
        }
        break;
    default:
        break;
    }
}

/* =========================================================================
 * Register write — CH2 (SOUND2CNT_L/H = 0x68/6C)
 * ========================================================================= */

static void ch2_write(struct gba *gba, uint32_t addr, uint8_t val)
{
    struct gba_apu_ch2 *ch = &gba->apu.ch2;
    switch (addr) {
    case REG_SOUND2CNT_L: /* NR21: duty + length */
        ch->duty = (val >> 6) & 3;
        ch->length.counter = 64 - (val & 0x3F);
        break;
    case REG_SOUND2CNT_L + 1: /* NR22: envelope */
        ch->env.initial_volume = (val >> 4) & 0xF;
        ch->env.increase       = (val >> 3) & 1;
        ch->env.period         = val & 7;
        ch->dac_on             = (val & 0xF8) != 0;
        if (!ch->dac_on)
            ch->enabled = false;
        break;
    case REG_SOUND2CNT_H: /* NR23: freq low */
        ch->freq = (ch->freq & 0x700) | val;
        ch->div.period = (uint16_t)ch_square_period(ch->freq);
        break;
    case REG_SOUND2CNT_H + 1: /* NR24: freq high + trigger + length enable */
        ch->freq = (ch->freq & 0x0FF) | ((uint16_t)(val & 7) << 8);
        ch->div.period     = (uint16_t)ch_square_period(ch->freq);
        ch->length.enabled = (val >> 6) & 1;
        if (val & 0x80) { /* trigger */
            if (ch->dac_on)
                ch->enabled = true;
            if (ch->length.counter == 0)
                ch->length.counter = 64;
            ch->env.volume  = ch->env.initial_volume;
            ch->env.counter = ch->env.period;
            ch->div.counter = ch->div.period;
            ch->duty_pos    = 0;
        }
        break;
    default:
        break;
    }
}

/* =========================================================================
 * Register write — CH3 (SOUND3CNT_L/H/X = 0x70/72/74)
 * ========================================================================= */

static void ch3_write(struct gba *gba, uint32_t addr, uint8_t val)
{
    struct gba_apu_ch3 *ch = &gba->apu.ch3;
    switch (addr) {
    case REG_SOUND3CNT_L: /* NR30: DAC on/off + bank mode */
        ch->bank_mode = (val >> 5) & 1;
        ch->wave_bank = (val >> 6) & 1;
        ch->dac_on    = (val >> 7) & 1;
        if (!ch->dac_on)
            ch->enabled = false;
        break;
    case REG_SOUND3CNT_H: /* NR31: length */
        ch->length.counter = 256 - val;
        break;
    case REG_SOUND3CNT_H + 1: /* NR32: volume */
        ch->volume_shift = (val >> 5) & 3;
        /* GBA adds a 75% mode: value 4 → shift 0 then >>2 (approx via shift=2 with flag) */
        /* We'll store raw and use in synthesis */
        break;
    case REG_SOUND3CNT_X: /* NR33: freq low */
        ch->freq = (ch->freq & 0x700) | val;
        ch->div.period = (uint16_t)ch3_period(ch->freq);
        break;
    case REG_SOUND3CNT_X + 1: /* NR34: freq high + trigger + length enable */
        ch->freq = (ch->freq & 0x0FF) | ((uint16_t)(val & 7) << 8);
        ch->div.period     = (uint16_t)ch3_period(ch->freq);
        ch->length.enabled = (val >> 6) & 1;
        if (val & 0x80) { /* trigger */
            if (ch->dac_on)
                ch->enabled = true;
            if (ch->length.counter == 0)
                ch->length.counter = 256;
            ch->wave_pos    = 0;
            ch->div.counter = ch->div.period;
        }
        break;
    default:
        break;
    }
}

/* =========================================================================
 * Register write — CH4 (SOUND4CNT_L/H = 0x78/7C)
 * ========================================================================= */

static void ch4_write(struct gba *gba, uint32_t addr, uint8_t val)
{
    struct gba_apu_ch4 *ch = &gba->apu.ch4;
    switch (addr) {
    case REG_SOUND4CNT_L: /* NR41: length */
        ch->length.counter = 64 - (val & 0x3F);
        break;
    case REG_SOUND4CNT_L + 1: /* NR42: envelope */
        ch->env.initial_volume = (val >> 4) & 0xF;
        ch->env.increase       = (val >> 3) & 1;
        ch->env.period         = val & 7;
        ch->dac_on             = (val & 0xF8) != 0;
        if (!ch->dac_on)
            ch->enabled = false;
        break;
    case REG_SOUND4CNT_H: /* NR43: clock shift + width + div ratio */
        ch->clk_shift  = (val >> 4) & 0xF;
        ch->width_7    = (val >> 3) & 1;
        ch->div_ratio  = val & 7;
        ch->period     = ch4_period(ch->clk_shift, ch->div_ratio);
        break;
    case REG_SOUND4CNT_H + 1: /* NR44: trigger + length enable */
        ch->length.enabled = (val >> 6) & 1;
        if (val & 0x80) { /* trigger */
            if (ch->dac_on)
                ch->enabled = true;
            if (ch->length.counter == 0)
                ch->length.counter = 64;
            ch->env.volume  = ch->env.initial_volume;
            ch->env.counter = ch->env.period;
            ch->lfsr        = 0x7FFF; /* all ones */
            ch->period_cnt  = ch->period;
        }
        break;
    default:
        break;
    }
}

/* =========================================================================
 * Public register I/O
 * ========================================================================= */

void gba_apu_write_reg(struct gba *gba, uint32_t addr, uint8_t val)
{
    struct gba_apu *apu = &gba->apu;

    /* Wave RAM: two banks of 16 bytes at 0x90–0x9F */
    if (addr >= 0x04000090 && addr <= 0x0400009F) {
        uint8_t bank = apu->ch3.bank_mode ? (apu->ch3.wave_bank ^ 1) : 0;
        uint32_t off = (addr - 0x04000090) + (uint32_t)bank * 16;
        apu->ch3.wave_ram[off & 31] = val;
        return;
    }

    switch (addr) {
    /* CH1 */
    case REG_SOUND1CNT_L:
    case REG_SOUND1CNT_H:
    case REG_SOUND1CNT_H + 1:
    case REG_SOUND1CNT_X:
    case REG_SOUND1CNT_X + 1:
        ch1_write(gba, addr, val);
        break;

    /* CH2 */
    case REG_SOUND2CNT_L:
    case REG_SOUND2CNT_L + 1:
    case REG_SOUND2CNT_H:
    case REG_SOUND2CNT_H + 1:
        ch2_write(gba, addr, val);
        break;

    /* CH3 */
    case REG_SOUND3CNT_L:
    case REG_SOUND3CNT_H:
    case REG_SOUND3CNT_H + 1:
    case REG_SOUND3CNT_X:
    case REG_SOUND3CNT_X + 1:
        ch3_write(gba, addr, val);
        break;

    /* CH4 */
    case REG_SOUND4CNT_L:
    case REG_SOUND4CNT_L + 1:
    case REG_SOUND4CNT_H:
    case REG_SOUND4CNT_H + 1:
        ch4_write(gba, addr, val);
        break;

    /* Master control */
    case REG_SOUNDCNT_X:
        if (val & 0x80)
            apu->soundcnt_x |= 0x80;
        else {
            /* Master off: reset all channels */
            apu->soundcnt_x = 0;
            memset(&apu->ch1, 0, sizeof(apu->ch1));
            memset(&apu->ch2, 0, sizeof(apu->ch2));
            memset(&apu->ch3, 0, sizeof(apu->ch3));
            memset(&apu->ch4, 0, sizeof(apu->ch4));
        }
        break;

    case REG_SOUNDCNT_L:
    case REG_SOUNDCNT_L + 1: {
        uint8_t shift = (addr & 1) ? 8 : 0;
        apu->soundcnt_l = (uint16_t)((apu->soundcnt_l & ~(0xFF << shift)) | (val << shift));
        break;
    }
    case REG_SOUNDCNT_H:
    case REG_SOUNDCNT_H + 1: {
        uint8_t shift = (addr & 1) ? 8 : 0;
        uint16_t old = apu->soundcnt_h;
        apu->soundcnt_h = (uint16_t)((old & ~(0xFF << shift)) | (val << shift));
        /* FIFO reset bits */
        if (addr == REG_SOUNDCNT_H + 1) {
            if (val & 0x08) { /* FIFO A reset */
                memset(&apu->fifo_a, 0, sizeof(apu->fifo_a));
                apu->fifo_a.timer_sel = (apu->soundcnt_h >> 10) & 1;
            }
            if (val & 0x80) { /* FIFO B reset */
                memset(&apu->fifo_b, 0, sizeof(apu->fifo_b));
                apu->fifo_b.timer_sel = (apu->soundcnt_h >> 14) & 1;
            }
        }
        /* Update FIFO timer selection from high byte */
        if (addr == REG_SOUNDCNT_H + 1) {
            apu->fifo_a.timer_sel = (apu->soundcnt_h >> 10) & 1;
            apu->fifo_a.r_en      = (apu->soundcnt_h >> 9) & 1;
            apu->fifo_a.l_en      = (apu->soundcnt_h >> 8) & 1;
            apu->fifo_b.timer_sel = (apu->soundcnt_h >> 14) & 1;
            apu->fifo_b.r_en      = (apu->soundcnt_h >> 13) & 1;
            apu->fifo_b.l_en      = (apu->soundcnt_h >> 12) & 1;
        }
        break;
    }
    case REG_SOUNDBIAS:
    case REG_SOUNDBIAS + 1: {
        uint8_t shift = (addr & 1) ? 8 : 0;
        apu->soundbias = (uint16_t)((apu->soundbias & ~(0xFF << shift)) | (val << shift));
        break;
    }
    case REG_FIFO_A:
    case REG_FIFO_A + 1:
    case REG_FIFO_A + 2:
    case REG_FIFO_A + 3:
        fifo_push_byte(&apu->fifo_a, (int8_t)val);
        break;
    case REG_FIFO_B:
    case REG_FIFO_B + 1:
    case REG_FIFO_B + 2:
    case REG_FIFO_B + 3:
        fifo_push_byte(&apu->fifo_b, (int8_t)val);
        break;
    default:
        break;
    }
}

uint8_t gba_apu_read_reg(struct gba *gba, uint32_t addr)
{
    struct gba_apu *apu = &gba->apu;

    /* Wave RAM */
    if (addr >= 0x04000090 && addr <= 0x0400009F) {
        uint8_t bank = apu->ch3.bank_mode ? (apu->ch3.wave_bank ^ 1) : 0;
        uint32_t off = (addr - 0x04000090) + (uint32_t)bank * 16;
        return apu->ch3.wave_ram[off & 31];
    }

    switch (addr) {
    /* CH1 */
    case REG_SOUND1CNT_L:
        return (uint8_t)((apu->ch1.sweep.period << 4) |
                         (apu->ch1.sweep.negate << 3) |
                          apu->ch1.sweep.shift);
    case REG_SOUND1CNT_H:
        return (uint8_t)((apu->ch1.duty << 6) | 0x3F); /* lower 6 write-only */
    case REG_SOUND1CNT_H + 1:
        return (uint8_t)((apu->ch1.env.initial_volume << 4) |
                         (apu->ch1.env.increase << 3) |
                          apu->ch1.env.period);
    case REG_SOUND1CNT_X:
        return 0; /* write-only */
    case REG_SOUND1CNT_X + 1:
        return (uint8_t)((apu->ch1.length.enabled << 6));

    /* CH2 */
    case REG_SOUND2CNT_L:
        return (uint8_t)((apu->ch2.duty << 6) | 0x3F);
    case REG_SOUND2CNT_L + 1:
        return (uint8_t)((apu->ch2.env.initial_volume << 4) |
                         (apu->ch2.env.increase << 3) |
                          apu->ch2.env.period);
    case REG_SOUND2CNT_H:
        return 0;
    case REG_SOUND2CNT_H + 1:
        return (uint8_t)((apu->ch2.length.enabled << 6));

    /* CH3 */
    case REG_SOUND3CNT_L:
        return (uint8_t)((apu->ch3.bank_mode << 5) |
                         (apu->ch3.wave_bank << 6) |
                         (apu->ch3.dac_on << 7));
    case REG_SOUND3CNT_H:
        return 0xFF; /* write-only */
    case REG_SOUND3CNT_H + 1:
        return (uint8_t)((apu->ch3.volume_shift << 5));
    case REG_SOUND3CNT_X:
        return 0;
    case REG_SOUND3CNT_X + 1:
        return (uint8_t)((apu->ch3.length.enabled << 6));

    /* CH4 */
    case REG_SOUND4CNT_L:
        return 0xFF;
    case REG_SOUND4CNT_L + 1:
        return (uint8_t)((apu->ch4.env.initial_volume << 4) |
                         (apu->ch4.env.increase << 3) |
                          apu->ch4.env.period);
    case REG_SOUND4CNT_H:
        return (uint8_t)((apu->ch4.clk_shift << 4) |
                         (apu->ch4.width_7 << 3) |
                          apu->ch4.div_ratio);
    case REG_SOUND4CNT_H + 1:
        return (uint8_t)((apu->ch4.length.enabled << 6));

    /* Master */
    case REG_SOUNDCNT_X:
        return (uint8_t)(apu->soundcnt_x |
                         (apu->ch1.enabled ? 1 : 0) |
                         (apu->ch2.enabled ? 2 : 0) |
                         (apu->ch3.enabled ? 4 : 0) |
                         (apu->ch4.enabled ? 8 : 0));
    case REG_SOUNDCNT_H:
        return apu->soundcnt_h & 0xFF;
    case REG_SOUNDCNT_H + 1:
        return apu->soundcnt_h >> 8;
    case REG_SOUNDCNT_L:
        return apu->soundcnt_l & 0xFF;
    case REG_SOUNDCNT_L + 1:
        return apu->soundcnt_l >> 8;
    case REG_SOUNDBIAS:
        return apu->soundbias & 0xFF;
    case REG_SOUNDBIAS + 1:
        return apu->soundbias >> 8;
    default:
        return 0;
    }
}

/* =========================================================================
 * Per-sample synthesis helpers
 * ========================================================================= */

/* Advance the frequency divider by one sample period (cycles_per_sample cycles).
 * Returns number of times the waveform step advanced. */
static void ch_square_advance(struct gba_apu_divider *div, uint8_t *duty_pos,
                               uint32_t cycles)
{
    if (div->period == 0) return;
    div->counter += (uint16_t)(cycles & 0xFFFF);
    while (div->counter >= div->period) {
        div->counter -= div->period;
        *duty_pos = (*duty_pos + 1) & 7;
    }
}

static void ch3_advance(struct gba_apu_ch3 *ch, uint32_t cycles)
{
    if (ch->div.period == 0) return;
    ch->div.counter += (uint16_t)(cycles & 0xFFFF);
    while (ch->div.counter >= ch->div.period) {
        ch->div.counter -= ch->div.period;
        /* advance sample position: each position = one 4-bit nibble */
        ch->wave_pos = (ch->wave_pos + 1) & (ch->bank_mode ? 31 : 63);
    }
}

static void ch4_advance(struct gba_apu_ch4 *ch, uint32_t cycles)
{
    if (ch->period == 0) return;
    if (ch->period_cnt <= cycles) {
        uint32_t steps = (cycles - ch->period_cnt) / ch->period + 1;
        ch->period_cnt = ch->period - ((cycles - ch->period_cnt) % ch->period);
        /* clock LFSR 'steps' times */
        for (uint32_t i = 0; i < steps; i++) {
            uint16_t bit = (ch->lfsr ^ (ch->lfsr >> 1)) & 1;
            ch->lfsr = (ch->lfsr >> 1) | (bit << 14);
            if (ch->width_7)
                ch->lfsr = (ch->lfsr & ~0x40u) | (bit << 6);
        }
    } else {
        ch->period_cnt -= cycles;
    }
}

/* CH3 volume shifts: 0=mute, 1=100%, 2=50%, 3=25%, +0x80=75% (GBA-only) */
static int16_t ch3_sample(struct gba_apu *apu)
{
    struct gba_apu_ch3 *ch = &apu->ch3;
    /* nibble from wave RAM */
    uint32_t byte_idx = ch->wave_pos >> 1;
    uint8_t byte = ch->wave_ram[byte_idx & 31];
    uint8_t nibble = (ch->wave_pos & 1) ? (byte & 0xF) : (byte >> 4);
    int16_t s = (int16_t)(nibble * 512) - 0x1000; /* centre around 0 */
    switch (ch->volume_shift & 3) {
    case 0: return 0;
    case 1: return s;
    case 2: return s >> 1;
    case 3: return s >> 2;
    default: return 0;
    }
}

/* =========================================================================
 * Synthesis + output
 * ========================================================================= */

static void apu_push_sample(struct gba *gba, int16_t left, int16_t right)
{
    struct gba_apu *apu = &gba->apu;
    int w   = apu->buf_write;
    int pos = apu->buf_pos;

    apu->buf[w][pos * 2 + 0] = left;
    apu->buf[w][pos * 2 + 1] = right;
    pos++;

    if (pos >= GBA_APU_BUF_SAMPLES) {
        if (sem_trywait(&apu->buf_free) == 0) {
            sem_post(&apu->buf_ready);
            apu->buf_write ^= 1;
        }
        pos = 0;
    }
    apu->buf_pos = pos;
}

void gba_apu_sync(struct gba *gba)
{
    struct gba_apu *apu = &gba->apu;
    int32_t elapsed = gba_sync_resync(gba, GBA_SYNC_APU);
    int32_t cycles_per_sample = GBA_CPU_FREQ_HZ / GBA_APU_SAMPLE_RATE;

    /* Frame sequencer at 512 Hz */
    int32_t cycles_per_fs = GBA_CPU_FREQ_HZ / 512;
    apu->frame_seq_cycles += elapsed;
    while (apu->frame_seq_cycles >= cycles_per_fs) {
        apu->frame_seq_cycles -= cycles_per_fs;
        frame_seq_clock(gba);
    }

    /* Generate samples */
    apu->next_sample_cycles -= elapsed;
    while (apu->next_sample_cycles <= 0) {
        apu->next_sample_cycles += cycles_per_sample;

        /* Advance waveform positions */
        uint32_t cyc = (uint32_t)cycles_per_sample;
        if (apu->ch1.enabled) ch_square_advance(&apu->ch1.div, &apu->ch1.duty_pos, cyc);
        if (apu->ch2.enabled) ch_square_advance(&apu->ch2.div, &apu->ch2.duty_pos, cyc);
        if (apu->ch3.enabled) ch3_advance(&apu->ch3, cyc);
        if (apu->ch4.enabled) ch4_advance(&apu->ch4, cyc);

        /* Master enable check */
        bool master_on = (apu->soundcnt_x & 0x80) != 0;

        /* CH1 */
        int16_t s1 = 0;
        if (master_on && apu->ch1.enabled && apu->ch1.dac_on && !apu->mute_ch1)
            s1 = duty_table[apu->ch1.duty][apu->ch1.duty_pos]
                ? (int16_t)(apu->ch1.env.volume * 512) : 0;

        /* CH2 */
        int16_t s2 = 0;
        if (master_on && apu->ch2.enabled && apu->ch2.dac_on && !apu->mute_ch2)
            s2 = duty_table[apu->ch2.duty][apu->ch2.duty_pos]
                ? (int16_t)(apu->ch2.env.volume * 512) : 0;

        /* CH3 */
        int16_t s3 = 0;
        if (master_on && apu->ch3.enabled && apu->ch3.dac_on && !apu->mute_ch3)
            s3 = ch3_sample(apu);

        /* CH4 */
        int16_t s4 = 0;
        if (master_on && apu->ch4.enabled && apu->ch4.dac_on && !apu->mute_ch4)
            s4 = (apu->ch4.lfsr & 1) ? (int16_t)(apu->ch4.env.volume * 512) : 0;

        /* SOUNDCNT_L: per-channel volume and stereo panning
         * bits 2-0: right volume, 6-4: left volume
         * bits 8-11: right enable (CH1-4), bits 12-15: left enable (CH1-4) */
        uint8_t vol_r = apu->soundcnt_l & 7;
        uint8_t vol_l = (apu->soundcnt_l >> 4) & 7;
        uint8_t r_en  = (apu->soundcnt_l >> 8) & 0xF;
        uint8_t l_en  = (apu->soundcnt_l >> 12) & 0xF;

        int32_t left_psg  = 0;
        int32_t right_psg = 0;
        if (r_en & 1) right_psg += s1;
        if (r_en & 2) right_psg += s2;
        if (r_en & 4) right_psg += s3;
        if (r_en & 8) right_psg += s4;
        if (l_en & 1) left_psg  += s1;
        if (l_en & 2) left_psg  += s2;
        if (l_en & 4) left_psg  += s3;
        if (l_en & 8) left_psg  += s4;

        /* PSG master volume: 0-7 maps to ×1/8 … ×8/8 */
        right_psg = right_psg * (vol_r + 1) / 8;
        left_psg  = left_psg  * (vol_l + 1) / 8;

        /* SOUNDCNT_H PSG mix volume: bits 1-0: 0=25%, 1=50%, 2=100% */
        int psg_vol_shift = 2 - (apu->soundcnt_h & 3);
        if (psg_vol_shift > 0) {
            right_psg >>= psg_vol_shift;
            left_psg  >>= psg_vol_shift;
        }

        /* DMA FIFO channels */
        int vol_a = (apu->soundcnt_h >> 2) & 1 ? 1 : 0; /* 0=×1, 1=×2 */
        int vol_b = (apu->soundcnt_h >> 3) & 1 ? 1 : 0;

        int16_t fa = !apu->mute_fifo_a ? apu->fifo_a.sample : 0;
        int16_t fb = !apu->mute_fifo_b ? apu->fifo_b.sample : 0;

        /* Scale: int8 → int16, ×1 or ×2 */
        int32_t fa_s = (int32_t)fa * 64 * (1 << vol_a);
        int32_t fb_s = (int32_t)fb * 64 * (1 << vol_b);

        int32_t out_l = left_psg;
        int32_t out_r = right_psg;

        if (apu->fifo_a.l_en) out_l += fa_s;
        if (apu->fifo_a.r_en) out_r += fa_s;
        if (apu->fifo_b.l_en) out_l += fb_s;
        if (apu->fifo_b.r_en) out_r += fb_s;

        /* Clamp */
        int16_t ol = (int16_t)(out_l > 32767 ? 32767 : out_l < -32768 ? -32768 : out_l);
        int16_t or_ = (int16_t)(out_r > 32767 ? 32767 : out_r < -32768 ? -32768 : out_r);

        apu_push_sample(gba, ol, or_);
    }

    gba_sync_next(gba, GBA_SYNC_APU,
        apu->next_sample_cycles > 0 ? apu->next_sample_cycles : cycles_per_sample);
}
