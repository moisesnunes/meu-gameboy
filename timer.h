#ifndef _GB_TIMER_H_
#define _GB_TIMER_H_

enum gb_timer_divider
{
    /* Timer frequency: 4096Hz */
    GB_TIMER_DIV_1024 = 0,
    /* Timer frequency: 262144Hz */
    GB_TIMER_DIV_16 = 1,
    /* Timer frequency: 65535Hz */
    GB_TIMER_DIV_64 = 2,
    /* Timer frequency: 16384Hz */
    GB_TIMER_DIV_256 = 3,
};

struct gb_timer
{
    uint16_t divider_counter;
    uint8_t counter;
    uint8_t modulo;
    enum gb_timer_divider divider;
    bool started;
    /* TIMA overflowed; waiting 4 T-cycles before loading TMA and firing IRQ */
    bool reload_pending;
    /* CPU T-cycles already elapsed inside that delayed reload window */
    unsigned reload_cycles;
    bool reload_just_happened;
    int32_t reload_timestamp;
};

void gb_timer_reset(struct gb *gb);
void gb_timer_sync(struct gb *gb);
void gb_timer_reset_divider(struct gb *gb);
void gb_timer_write_counter(struct gb *gb, uint8_t value);
void gb_timer_write_modulo(struct gb *gb, uint8_t value);
void gb_timer_set_config(struct gb *gb, uint8_t config);
uint8_t gb_timer_get_config(struct gb *gb);

#endif /* _GB_TIMER_H_ */
