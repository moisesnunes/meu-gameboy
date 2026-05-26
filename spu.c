#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "gb.h"

#define GB_SPU_FRAME_SEQ_PERIOD 8192U
#define GB_SPU_LENGTH_STEP_NR1 64U
#define GB_SPU_LENGTH_STEP_NR2 64U
#define GB_SPU_LENGTH_STEP_NR3 256U
#define GB_SPU_LENGTH_STEP_NR4 64U

void gb_spu_update_sound_amp(struct gb *gb)
{
     struct gb_spu *spu = &gb->spu;
     unsigned sound;
     /* The maximum value a sample can take while summing the raw values */
     unsigned max_amplitude;
     unsigned scaling;

     /* Each sound generates values 4bit unsigned values */
     max_amplitude = 15;
     /* Which can then be amplified up to 8 times by the `output_level` setting
      */
     max_amplitude *= 8;
     /* Finally we sum up to 4 sounds */
     max_amplitude *= 4;

     /* Linear scaling to saturate the output at max amplitude */
     scaling = 0x7fff / max_amplitude;

     for (sound = 0; sound < 4; sound++)
     {
          unsigned channel;

          for (channel = 0; channel < 2; channel++)
          {
               unsigned reg_shift = channel == 0 ? 4 : 0;
               bool enabled = spu->sound_mux & (1 << (sound + reg_shift));
               int16_t amp;

               if (enabled)
               {
                    amp = 1;
                    amp += (spu->output_level >> reg_shift) & 7;
                    amp *= scaling;
               }
               else
               {
                    amp = 0;
               }

               spu->sound_amp[sound][channel] = amp;
          }
     }
}

static void gb_spu_frequency_reload(struct gb_spu_divider *f)
{
     f->counter = 2 * (0x800U - f->offset);
}

static void gb_spu_lfsr_counter_reload(struct gb_spu_nr4 *nr4)
{
     /* The LFSR clock has a divider and a shifter */
     uint8_t div = nr4->lfsr_config & 7;
     uint8_t shift = (nr4->lfsr_config >> 4) + 1;

     if (div == 0)
     {
          nr4->counter = 4;
     }
     else
     {
          nr4->counter = 8 * div;
     }

     nr4->counter <<= shift;
}

void gb_spu_sweep_reload(struct gb_spu_sweep *f, uint8_t conf)
{
     f->shift = conf & 0x7;
     f->subtract = (conf >> 3) & 1;
     f->time = (conf >> 4) & 0x7;
}

void gb_spu_reset(struct gb *gb)
{
     struct gb_spu *spu = &gb->spu;
     unsigned i;

     /* Reset audio ring-buffer semaphores to a clean state so that loading a
      * new ROM never leaves sem_wait() blocked on a stale semaphore value. */
     for (i = 0; i < GB_SPU_SAMPLE_BUFFER_COUNT; i++)
     {
          struct gb_spu_sample_buffer *buf = &spu->buffers[i];
          /* Drain any posted-but-unconsumed values before re-initialising. */
          sem_destroy(&buf->free);
          sem_destroy(&buf->ready);
          sem_init(&buf->free,  0, 1);
          sem_init(&buf->ready, 0, 0);
     }
     spu->buffer_index = 0;
     spu->sample_index = 0;

     spu->enable = true;
     spu->frame_seq_counter = GB_SPU_FRAME_SEQ_PERIOD;
     spu->frame_seq_step = 0;
     spu->output_level = 0x77;
     spu->sound_mux = 0xf3;

     gb_spu_update_sound_amp(gb);

     /* NR1 reset */
     spu->nr1.running = gb->hw_model != GB_HW_SGB &&
                        gb->hw_model != GB_HW_SGB2;
     spu->nr1.duration.enable = false;
     spu->nr1.duration.counter = 0;
     spu->nr1.wave.duty_cycle = 2;
     spu->nr1.envelope_config = 0xf3;
     spu->nr1.envelope.value = 0;
     spu->nr1.envelope.step_duration = 0;
     spu->nr1.envelope.counter = 0;
     spu->nr1.envelope.running = false;

     spu->nr1.sweep.divider.offset = 0;
     gb_spu_frequency_reload(&spu->nr1.sweep.divider);
     gb_spu_sweep_reload(&spu->nr1.sweep, 0);
     spu->nr1.sweep.enable = false;
     spu->nr1.sweep.counter = 0;
     spu->nr1.sweep.shadow_offset = 0;
     spu->nr1.sweep.subtract_calculated = false;

     /* NR2 reset */
     spu->nr2.running = false;
     spu->nr2.duration.enable = false;
     spu->nr2.duration.counter = 0;
     spu->nr2.wave.duty_cycle = 0;
     spu->nr2.envelope_config = 0;
     spu->nr2.envelope.value = 0;
     spu->nr2.envelope.step_duration = 0;
     spu->nr2.envelope.counter = 0;
     spu->nr2.envelope.running = false;

     spu->nr2.divider.offset = 0;
     gb_spu_frequency_reload(&spu->nr2.divider);

     /* NR3 reset */
     spu->nr3.enable = false;
     spu->nr3.running = false;
     spu->nr3.duration.enable = false;
     spu->nr3.duration.counter = 0;
     spu->nr3.volume_shift = 0;
     spu->nr3.divider.offset = 0;
     spu->nr3.t1 = 0;
     spu->nr3.index = 0;
     spu->nr3.access_cycles = 0;

     spu->nr3.divider.offset = 0;
     gb_spu_frequency_reload(&spu->nr3.divider);

     /* NR4 reset */
     spu->nr4.running = false;
     spu->nr4.duration.enable = false;
     spu->nr4.duration.counter = 0;
     spu->nr4.envelope_config = 0;
     spu->nr4.envelope.value = 0;
     spu->nr4.envelope.step_duration = 0;
     spu->nr4.envelope.counter = 0;
     spu->nr4.envelope.running = false;
     spu->nr4.lfsr_config = 0;
     spu->nr4.lfsr = 0x7fff;
}

void gb_spu_power_off(struct gb *gb)
{
     struct gb_spu *spu = &gb->spu;
     uint8_t wave_ram[GB_NR3_RAM_SIZE];
     uint16_t len1 = 0;
     uint16_t len2 = 0;
     uint16_t len3 = 0;
     uint16_t len4 = 0;
     bool frontend_mute[4];

     memcpy(wave_ram, spu->nr3.ram, sizeof(wave_ram));
     memcpy(frontend_mute, spu->frontend_mute, sizeof(frontend_mute));

     if (!gb->gbc)
     {
          len1 = spu->nr1.duration.counter;
          len2 = spu->nr2.duration.counter;
          len3 = spu->nr3.duration.counter;
          len4 = spu->nr4.duration.counter;
     }

     spu->enable = false;
     spu->output_level = 0;
     spu->sound_mux = 0;
     spu->sample_period_frac = 0;
     spu->frame_seq_counter = GB_SPU_FRAME_SEQ_PERIOD;
     spu->frame_seq_step = 0;

     memset(&spu->nr1, 0, sizeof(spu->nr1));
     memset(&spu->nr2, 0, sizeof(spu->nr2));
     memset(&spu->nr3, 0, sizeof(spu->nr3));
     memset(&spu->nr4, 0, sizeof(spu->nr4));

     memcpy(spu->nr3.ram, wave_ram, sizeof(wave_ram));
     memcpy(spu->frontend_mute, frontend_mute, sizeof(frontend_mute));

     if (!gb->gbc)
     {
          spu->nr1.duration.counter = len1;
          spu->nr2.duration.counter = len2;
          spu->nr3.duration.counter = len3;
          spu->nr4.duration.counter = len4;
     }

     spu->nr4.lfsr = 0x7fff;
     gb_spu_update_sound_amp(gb);
}

void gb_spu_duration_reload(struct gb_spu_duration *d,
                            unsigned duration_max,
                            uint8_t t1)
{
     d->counter = duration_max + 1 - t1;
}

static bool gb_spu_next_frame_step_clocks_length(struct gb *gb)
{
     return (gb->spu.frame_seq_step & 1) == 0;
}

void gb_spu_duration_set_enable(struct gb *gb,
                                struct gb_spu_duration *d,
                                bool enable,
                                bool *running,
                                bool trigger,
                                unsigned length_max)
{
     if (!d->enable && enable &&
         !gb_spu_next_frame_step_clocks_length(gb) &&
         d->counter)
     {
          d->counter--;
          if (!d->counter)
          {
               if (trigger)
               {
                    d->counter = length_max - 1;
               }
               else
               {
                    *running = false;
               }
          }
     }

     d->enable = enable;
}

static void gb_spu_duration_trigger(struct gb *gb,
                                    struct gb_spu_duration *d,
                                    unsigned length_max)
{
     if (d->counter)
     {
          return;
     }

     d->counter = length_max;
     d->enable = false;
}

/* Clock the length counter. Returns true if the channel should be disabled. */
static bool gb_spu_duration_clock(struct gb_spu_duration *d)
{
     if (d->enable && d->counter)
     {
          d->counter--;
          return d->counter == 0;
     }

     return false;
}

/* Update the frequency counter and return the number of times it ran out */
static unsigned gb_spu_frequency_update(struct gb_spu_divider *f,
                                        unsigned cycles)
{
     unsigned count = 0;

     while (cycles)
     {
          if (f->counter > cycles)
          {
               f->counter -= cycles;
               cycles = 0;
          }
          else
          {
               count++;
               cycles -= f->counter;
               /* Reload counter */
               gb_spu_frequency_reload(f);
          }
     }

     return count;
}

static uint16_t gb_spu_sweep_calculate(struct gb_spu_sweep *s, bool *overflow)
{
     uint16_t delta = s->shadow_offset >> s->shift;
     uint32_t offset;

     if (s->subtract)
     {
          s->subtract_calculated = true;
          offset = s->shadow_offset - delta;
     }
     else
     {
          offset = s->shadow_offset + delta;
     }

     *overflow = offset > 0x7ff;
     return offset;
}

static bool gb_spu_sweep_clock(struct gb_spu_sweep *s)
{
     bool overflow = false;

     if (s->counter)
     {
          s->counter--;
     }

     if (s->counter)
     {
          return false;
     }

     s->counter = s->time ? s->time : 8;

     if (!s->enable || s->time == 0)
     {
          return false;
     }

     uint16_t offset = gb_spu_sweep_calculate(s, &overflow);

     if (overflow)
     {
          return true;
     }

     if (s->shift != 0)
     {
          s->shadow_offset = offset;
          s->divider.offset = offset;
          gb_spu_sweep_calculate(s, &overflow);
     }

     return overflow;
}

static bool gb_spu_sweep_init(struct gb_spu_sweep *s)
{
     bool overflow = false;

     s->shadow_offset = s->divider.offset;
     s->counter = s->time ? s->time : 8;
     s->enable = s->time != 0 || s->shift != 0;
     s->subtract_calculated = false;

     if (s->shift != 0)
     {
          gb_spu_sweep_calculate(s, &overflow);
     }

     return overflow;
}

#define GB_SPU_NPHASES 16
static uint8_t gb_spu_next_wave_sample(struct gb_spu_rectangle_wave *wave,
                                       unsigned phase_steps)
{
     static const uint8_t waveforms[4][GB_SPU_NPHASES / 2] = {
         /* 1/8 */
         {1, 0, 0, 0, 0, 0, 0, 0},
         /* 1/4 */
         {1, 1, 0, 0, 0, 0, 0, 0},
         /* 1/2 */
         {1, 1, 1, 1, 0, 0, 0, 0},
         /* 3/4 */
         {1, 1, 1, 1, 1, 1, 0, 0},
     };

     wave->phase = (wave->phase + phase_steps) % GB_SPU_NPHASES;

     return waveforms[wave->duty_cycle][wave->phase / 2];
}

static void gb_spu_envelope_reload_counter(struct gb_spu_envelope *e)
{
     e->counter = e->step_duration;
}

/* Reload the envelope config from the register value */
static void gb_spu_envelope_init(struct gb_spu_envelope *e, uint8_t config)
{
     e->value = config >> 4;
     e->increment = (config & 8);
     e->step_duration = config & 7;
     e->running = e->step_duration != 0;

     gb_spu_envelope_reload_counter(e);
}

static bool gb_spu_envelope_dac_enabled(uint8_t config)
{
     return (config & 0xf8) != 0;
}

/* Run the envelope if it's enabled. Reaching volume 0 only stops further
 * volume changes; it does not disable the channel by itself. */
static void gb_spu_envelope_clock(struct gb_spu_envelope *e)
{
     if (!e->running || e->step_duration == 0)
     {
          return;
     }

     if (e->counter)
     {
          e->counter--;
     }

     if (e->counter)
     {
          return;
     }

     gb_spu_envelope_reload_counter(e);

     if (e->increment)
     {
          if (e->value < 0xf)
          {
               e->value++;
          }
          else
          {
               e->running = false;
          }
     }
     else
     {
          if (e->value > 0)
          {
               e->value--;
          }
          else
          {
               e->running = false;
          }
     }
}

static int16_t gb_spu_next_nr1_sample(struct gb *gb, unsigned cycles)
{
     struct gb_spu *spu = &gb->spu;
     uint8_t sample;
     unsigned sound_cycles;

     if (!spu->nr1.running)
     {
          return 0;
     }

     sound_cycles = gb_spu_frequency_update(&spu->nr1.sweep.divider, cycles);

     sample = gb_spu_next_wave_sample(&spu->nr1.wave, sound_cycles);

     return sample ? spu->nr1.envelope.value : -spu->nr1.envelope.value;
}

static int16_t gb_spu_next_nr2_sample(struct gb *gb, unsigned cycles)
{
     struct gb_spu *spu = &gb->spu;
     uint8_t sample;
     unsigned sound_cycles;

     if (!spu->nr2.running)
     {
          return 0;
     }

     sound_cycles = gb_spu_frequency_update(&spu->nr2.divider, cycles);

     sample = gb_spu_next_wave_sample(&spu->nr2.wave, sound_cycles);

     return sample ? spu->nr2.envelope.value : -spu->nr2.envelope.value;
}

static int16_t gb_spu_next_nr3_sample(struct gb *gb, unsigned cycles)
{
     struct gb_spu *spu = &gb->spu;
     uint8_t sample;

     if (!spu->nr3.running)
     {
          return 0;
     }

     if (!gb->gbc)
     {
          /* DMG: process the wave timer cycle-by-cycle to track the access window
           * accurately. Each time the timer fires we open a 2-cycle window; then
           * we count down the remaining cycles in the batch to close it. */
          unsigned remaining = cycles;
          while (remaining)
          {
               unsigned step = remaining;
               if (step > (unsigned)spu->nr3.divider.counter)
                    step = (unsigned)spu->nr3.divider.counter;

               remaining -= step;
               spu->nr3.divider.counter -= step;

               if (spu->nr3.access_cycles > 0)
               {
                    spu->nr3.access_cycles -= (int32_t)step;
                    if (spu->nr3.access_cycles < 0)
                         spu->nr3.access_cycles = 0;
               }

               if (spu->nr3.divider.counter == 0)
               {
                    spu->nr3.index = (spu->nr3.index + 1) % (GB_NR3_RAM_SIZE * 2);
                    gb_spu_frequency_reload(&spu->nr3.divider);
                    spu->nr3.access_cycles = 2;
               }
          }
     }
     else
     {
          unsigned sound_cycles = gb_spu_frequency_update(&spu->nr3.divider, cycles);
          if (sound_cycles > 0)
               spu->nr3.index = (spu->nr3.index + sound_cycles) % (GB_NR3_RAM_SIZE * 2);
     }

     if (spu->nr3.volume_shift == 0)
     {
          /* Sound is muted */
          return 0;
     }

     /* We pack two samples per byte */
     sample = spu->nr3.ram[spu->nr3.index / 2];

     if (spu->nr3.index & 1)
     {
          sample &= 0xf;
     }
     else
     {
          sample >>= 4;
     }

     return ((int16_t)sample * 2 - 15) /
            (1 << (spu->nr3.volume_shift - 1));
}

static void gb_spu_lfsr_step(struct gb_spu_nr4 *nr4)
{
     /* If true the lfsr only uses 7 bits for the effective register period */
     bool period_7bits = nr4->lfsr_config & 0x8;
     uint16_t shifted;
     uint16_t carry;

     shifted = nr4->lfsr >> 1;
     carry = (nr4->lfsr ^ shifted) & 1;

     nr4->lfsr = shifted;
     nr4->lfsr |= carry << 14;

     if (period_7bits)
     {
          /* Carry is also copied to bit 6 */
          nr4->lfsr &= ~(1U << 6);
          nr4->lfsr |= carry << 6;
     }
}

static int16_t gb_spu_next_nr4_sample(struct gb *gb, unsigned cycles)
{
     struct gb_spu *spu = &gb->spu;
     uint8_t sample;

     if (!spu->nr4.running)
     {
          return 0;
     }

     while (cycles)
     {
          if (spu->nr4.counter > cycles)
          {
               spu->nr4.counter -= cycles;
               cycles = 0;
          }
          else
          {
               cycles -= spu->nr4.counter;
               gb_spu_lfsr_counter_reload(&spu->nr4);
               gb_spu_lfsr_step(&spu->nr4);
          }
     }

     sample = spu->nr4.lfsr & 1;

     return sample ? spu->nr4.envelope.value : -spu->nr4.envelope.value;
}

/* Send a pair of left/right samples to the frontend */
static void gb_spu_send_sample_to_frontend(struct gb *gb,
                                           int16_t sample_l, int16_t sample_r)
{
     struct gb_spu *spu = &gb->spu;
     struct gb_spu_sample_buffer *buf;

     buf = &spu->buffers[spu->buffer_index];

     if (spu->sample_index == 0)
     {
          /* We're about to fill the first sample, make sure that the
           * buffer is free. If it's not this will pause the thread until
           * the frontend frees it, effectively synchronizing us with audio
           */
          sem_wait(&buf->free);
     }

     buf->samples[spu->sample_index][0] = sample_l;
     buf->samples[spu->sample_index][1] = sample_r;

     spu->sample_index++;
     if (spu->sample_index == GB_SPU_SAMPLE_BUFFER_LENGTH)
     {
          /* We're done with this buffer */
          sem_post(&buf->ready);
          /* Move on to the next one */
          spu->buffer_index = (spu->buffer_index + 1) % GB_SPU_SAMPLE_BUFFER_COUNT;
          spu->sample_index = 0;
     }
}

static void gb_spu_frame_sequencer_update(struct gb *gb, unsigned cycles);

static void gb_spu_advance(struct gb *gb, unsigned cycles, bool send_sample)
{
     struct gb_spu *spu = &gb->spu;
     unsigned sound;
     int16_t sound_samples[4];
     int32_t sample_l = 0;
     int32_t sample_r = 0;

     gb_spu_frame_sequencer_update(gb, cycles);

     sound_samples[0] = gb_spu_next_nr1_sample(gb, cycles);
     sound_samples[1] = gb_spu_next_nr2_sample(gb, cycles);
     sound_samples[2] = gb_spu_next_nr3_sample(gb, cycles);
     sound_samples[3] = gb_spu_next_nr4_sample(gb, cycles);

     if (!send_sample)
     {
          return;
     }

     for (sound = 0; sound < 4; sound++)
     {
          if (spu->frontend_mute[sound])
          {
               continue;
          }

          sample_l += sound_samples[sound] * spu->sound_amp[sound][0];
          sample_r += sound_samples[sound] * spu->sound_amp[sound][1];
     }

     if (sample_l > INT16_MAX)
     {
          sample_l = INT16_MAX;
     }
     else if (sample_l < INT16_MIN)
     {
          sample_l = INT16_MIN;
     }

     if (sample_r > INT16_MAX)
     {
          sample_r = INT16_MAX;
     }
     else if (sample_r < INT16_MIN)
     {
          sample_r = INT16_MIN;
     }

     gb_spu_send_sample_to_frontend(gb, sample_l, sample_r);
}

static void gb_spu_frame_sequencer_clock(struct gb *gb)
{
     struct gb_spu *spu = &gb->spu;

     switch (spu->frame_seq_step)
     {
     case 0:
     case 4:
          if (gb_spu_duration_clock(&spu->nr1.duration))
               spu->nr1.running = false;
          if (gb_spu_duration_clock(&spu->nr2.duration))
               spu->nr2.running = false;
          if (gb_spu_duration_clock(&spu->nr3.duration))
               spu->nr3.running = false;
          if (gb_spu_duration_clock(&spu->nr4.duration))
               spu->nr4.running = false;
          break;

     case 2:
     case 6:
          if (gb_spu_duration_clock(&spu->nr1.duration))
               spu->nr1.running = false;
          if (gb_spu_duration_clock(&spu->nr2.duration))
               spu->nr2.running = false;
          if (gb_spu_duration_clock(&spu->nr3.duration))
               spu->nr3.running = false;
          if (gb_spu_duration_clock(&spu->nr4.duration))
               spu->nr4.running = false;

          if (gb_spu_sweep_clock(&spu->nr1.sweep))
               spu->nr1.running = false;
          break;

     case 7:
          gb_spu_envelope_clock(&spu->nr1.envelope);
          gb_spu_envelope_clock(&spu->nr2.envelope);
          gb_spu_envelope_clock(&spu->nr4.envelope);
          break;

     default:
          break;
     }

     spu->frame_seq_step = (spu->frame_seq_step + 1) & 7;
}

static void gb_spu_frame_sequencer_update(struct gb *gb, unsigned cycles)
{
     struct gb_spu *spu = &gb->spu;

     while (cycles)
     {
          if (spu->frame_seq_counter > cycles)
          {
               spu->frame_seq_counter -= cycles;
               return;
          }

          cycles -= spu->frame_seq_counter;
          spu->frame_seq_counter = GB_SPU_FRAME_SEQ_PERIOD;
          gb_spu_frame_sequencer_clock(gb);
     }
}

void gb_spu_div_reset(struct gb *gb, uint16_t old_divider)
{
     struct gb_spu *spu = &gb->spu;

     /* Em double-speed o DIV avança 2× mais rápido; o falling edge que aciona
      * o frame sequencer muda do bit 12 para o bit 13 do divider interno. */
     uint16_t div_bit = gb->double_speed ? 0x2000 : 0x1000;
     if (spu->enable && (old_divider & div_bit))
     {
          gb_spu_frame_sequencer_clock(gb);
     }

     spu->frame_seq_counter = GB_SPU_FRAME_SEQ_PERIOD;
}

void gb_spu_sync(struct gb *gb)
{
     struct gb_spu *spu = &gb->spu;
     int32_t elapsed = gb_sync_resync(gb, GB_SYNC_SPU);
     int32_t remaining;
     uint8_t frac;
     int32_t next_sync;

     remaining = elapsed;
     frac = spu->sample_period_frac;
     while (remaining > 0)
     {
          int32_t step = GB_SPU_SAMPLE_RATE_DIVISOR - frac;

          if (step > remaining)
          {
               step = remaining;
          }

          remaining -= step;
          frac += step;

          if (frac == GB_SPU_SAMPLE_RATE_DIVISOR)
          {
               gb_spu_advance(gb, step, true);
               frac = 0;
          }
          else
          {
               gb_spu_advance(gb, step, false);
          }
     }

     spu->sample_period_frac = frac;

     /* Schedule a sync to fill the current buffer */
     next_sync = (GB_SPU_SAMPLE_BUFFER_LENGTH - spu->sample_index) *
                 GB_SPU_SAMPLE_RATE_DIVISOR;
     next_sync -= frac;
     gb_sync_next(gb, GB_SYNC_SPU, next_sync);
}

void gb_spu_nr1_start(struct gb *gb)
{
     struct gb_spu *spu = &gb->spu;
     bool sweep_overflow;

     gb_spu_frequency_reload(&spu->nr1.sweep.divider);
     gb_spu_envelope_init(&spu->nr1.envelope, spu->nr1.envelope_config);
     gb_spu_duration_trigger(gb, &spu->nr1.duration, GB_SPU_LENGTH_STEP_NR1);

     sweep_overflow = gb_spu_sweep_init(&spu->nr1.sweep);
     spu->nr1.running = gb_spu_envelope_dac_enabled(spu->nr1.envelope_config) &&
                        !sweep_overflow;
}

void gb_spu_nr2_start(struct gb *gb)
{
     struct gb_spu *spu = &gb->spu;

     gb_spu_frequency_reload(&spu->nr2.divider);
     gb_spu_envelope_init(&spu->nr2.envelope, spu->nr2.envelope_config);
     gb_spu_duration_trigger(gb, &spu->nr2.duration, GB_SPU_LENGTH_STEP_NR2);

     spu->nr2.running = gb_spu_envelope_dac_enabled(spu->nr2.envelope_config);
}

void gb_spu_nr3_start(struct gb *gb)
{
     struct gb_spu *spu = &gb->spu;

     /* DMG corruption: re-triggering while CH3 is playing corrupts wave RAM.
      * The next byte to be read (at index+1) determines which bytes are
      * corrupted into the start of wave RAM. Matches blargg DMG-B behavior. */
     if (!gb->gbc && spu->nr3.running && spu->nr3.divider.counter <= 2)
     {
          unsigned offset = ((spu->nr3.index + 1) >> 1) & 0xf;
          if (offset < 4)
               spu->nr3.ram[0] = spu->nr3.ram[offset];
          else
               memcpy(spu->nr3.ram, spu->nr3.ram + (offset & ~3u), 4);
     }

     spu->nr3.index = 0;
     gb_spu_duration_trigger(gb, &spu->nr3.duration, GB_SPU_LENGTH_STEP_NR3);
     gb_spu_frequency_reload(&spu->nr3.divider);
     /* Hardware: wave timer has 3 extra 2MHz-clock cycles of delay on trigger */
     spu->nr3.divider.counter += 6;
     spu->nr3.access_cycles = 0;

     if (!spu->nr3.enable)
     {
          spu->nr3.running = false;
          return;
     }

     spu->nr3.running = true;
}

void gb_spu_nr4_start(struct gb *gb)
{
     struct gb_spu *spu = &gb->spu;

     gb_spu_envelope_init(&spu->nr4.envelope, spu->nr4.envelope_config);
     gb_spu_duration_trigger(gb, &spu->nr4.duration, GB_SPU_LENGTH_STEP_NR4);
     spu->nr4.lfsr = 0x7fff;
     gb_spu_lfsr_counter_reload(&spu->nr4);
     spu->nr4.running = gb_spu_envelope_dac_enabled(spu->nr4.envelope_config);
}
