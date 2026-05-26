#ifndef _GBA_TIMER_H_
#define _GBA_TIMER_H_

#include <stdint.h>
#include <stdbool.h>

struct gba;

#define GBA_TIMER_COUNT 4

/* Prescaler values: 1, 64, 256, 1024 cycles */
static const int gba_timer_prescaler[4] = { 1, 64, 256, 1024 };

struct gba_timer_channel {
    uint16_t counter;   /* current counter value (TM?CNT_L read) */
    uint16_t reload;    /* reload value written to TM?CNT_L */
    uint16_t reload_pending;
    /* Control register (TM?CNT_H) fields */
    uint8_t  prescaler; /* 0-3 */
    bool     cascade;   /* increment when previous timer overflows */
    bool     irq_en;    /* fire IRQ on overflow */
    bool     enable;
    bool     pending_reload;
    /* Internal accumulator for sub-cycle counts in non-cascade mode */
    int32_t  cycles_acc;
    int32_t  reload_delay;
};

struct gba_timer {
    struct gba_timer_channel ch[GBA_TIMER_COUNT];
};

void gba_timer_reset(struct gba *gba);
void gba_timer_sync(struct gba *gba);
void gba_timer_write_reload(struct gba *gba, int n, uint16_t val);
void gba_timer_write_ctrl(struct gba *gba, int n, uint16_t val);
uint16_t gba_timer_read_counter(struct gba *gba, int n);

#endif /* _GBA_TIMER_H_ */
