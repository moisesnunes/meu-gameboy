#ifndef _GBA_APU_H_
#define _GBA_APU_H_

#include <stdint.h>
#include <stdbool.h>
#include <semaphore.h>

struct gba;

/* Audio sample rate: 32768 Hz (GBA native) */
#define GBA_APU_SAMPLE_RATE 32768
#define GBA_APU_BUF_SAMPLES 2048

/* Legacy GB channel sub-structs (reused pattern from spu.h) */
struct gba_apu_envelope
{
     uint8_t volume;
     uint8_t initial_volume;
     bool increase;
     uint8_t period; /* 0 = disabled */
     uint8_t counter;
};

struct gba_apu_length
{
     uint16_t counter;
     bool enabled;
};

struct gba_apu_sweep
{
     uint8_t period;
     bool negate;
     uint8_t shift;
     uint8_t counter;
     uint16_t shadow_freq;
};

struct gba_apu_divider
{
     uint16_t period;
     uint16_t counter;
};

/* Channel 1: square wave with sweep */
struct gba_apu_ch1
{
     struct gba_apu_sweep sweep;
     struct gba_apu_length length;
     struct gba_apu_envelope env;
     struct gba_apu_divider div;
     uint8_t duty;
     uint8_t duty_pos;
     uint16_t freq;
     bool enabled;
     bool dac_on;
};

/* Channel 2: square wave */
struct gba_apu_ch2
{
     struct gba_apu_length length;
     struct gba_apu_envelope env;
     struct gba_apu_divider div;
     uint8_t duty;
     uint8_t duty_pos;
     uint16_t freq;
     bool enabled;
     bool dac_on;
};

/* Channel 3: wave RAM */
struct gba_apu_ch3
{
     struct gba_apu_length length;
     struct gba_apu_divider div;
     uint8_t wave_ram[32]; /* GBA has 2 banks of 16 bytes */
     uint8_t wave_bank;
     uint8_t wave_pos;
     uint8_t volume_shift; /* 0=mute,1=100%,2=50%,3=25%,4=75% */
     uint16_t freq;
     bool enabled;
     bool dac_on;
     bool bank_mode; /* 0=single 32-byte, 1=dual 16-byte bank */
};

/* Channel 4: noise */
struct gba_apu_ch4
{
     struct gba_apu_length length;
     struct gba_apu_envelope env;
     uint16_t lfsr;
     uint8_t clk_shift;
     uint8_t div_ratio;
     bool width_7; /* true = 7-bit LFSR */
     uint32_t period;
     uint32_t period_cnt;
     bool enabled;
     bool dac_on;
};

/* DMA PCM FIFO channels (A and B) */
struct gba_apu_fifo
{
     int8_t buf[32];
     int head;
     int tail;
     int len;
     uint8_t timer_sel; /* 0=TM0, 1=TM1 */
     bool l_en;
     bool r_en;
     int8_t sample;
};

struct gba_apu
{
     struct gba_apu_ch1 ch1;
     struct gba_apu_ch2 ch2;
     struct gba_apu_ch3 ch3;
     struct gba_apu_ch4 ch4;
     struct gba_apu_fifo fifo_a;
     struct gba_apu_fifo fifo_b;

     /* SOUNDCNT_L: legacy channel volume/panning */
     uint16_t soundcnt_l;
     /* SOUNDCNT_H: DMA/legacy mix control */
     uint16_t soundcnt_h;
     /* SOUNDCNT_X: master enable + channel status */
     uint8_t soundcnt_x;
     /* SOUNDBIAS: output bias + resolution */
     uint16_t soundbias;

     /* Frame sequencer (512Hz steps like GB) */
     uint32_t frame_seq_cycles;
     uint8_t frame_seq_step;

     /* Output sample buffer (stereo, 16-bit) */
     int16_t buf[2][GBA_APU_BUF_SAMPLES * 2];
     int buf_write;
     int buf_read;
     int buf_pos; /* samples written into current buf_write half */
     sem_t buf_free;
     sem_t buf_ready;

     /* Mute flags (frontend only) */
     bool mute_ch1, mute_ch2, mute_ch3, mute_ch4;
     bool mute_fifo_a, mute_fifo_b;

     int32_t next_sample_cycles;
};

void gba_apu_reset(struct gba *gba);
void gba_apu_sync(struct gba *gba);
void gba_apu_write_reg(struct gba *gba, uint32_t addr, uint8_t val);
uint8_t gba_apu_read_reg(struct gba *gba, uint32_t addr);
void gba_apu_fifo_push(struct gba *gba, int fifo, uint32_t word);
void gba_apu_fifo_tick(struct gba *gba, int timer); /* called on timer overflow */

#endif /* _GBA_APU_H_ */
