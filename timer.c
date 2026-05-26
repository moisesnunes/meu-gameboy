#include <stdio.h>
#include "gb.h"

static unsigned gb_timer_div_cycles(enum gb_timer_divider divider)
{
    switch (divider)
    {
    case GB_TIMER_DIV_16:
        return 16;
    case GB_TIMER_DIV_64:
        return 64;
    case GB_TIMER_DIV_256:
        return 256;
    case GB_TIMER_DIV_1024:
        return 1024;
    default:
        /* Unreachable */
        die();
        return 1024;
    }
}

static unsigned gb_timer_div_bit(enum gb_timer_divider divider)
{
    switch (divider)
    {
    case GB_TIMER_DIV_16:
        return 3;
    case GB_TIMER_DIV_64:
        return 5;
    case GB_TIMER_DIV_256:
        return 7;
    case GB_TIMER_DIV_1024:
        return 9;
    default:
        die();
        return 9;
    }
}

static int32_t gb_timer_to_timestamp_cycles(uint32_t cpu_cycles,
                                            unsigned speed_scale)
{
    if (cpu_cycles == 0)
    {
        return 0;
    }

    return (int32_t)((cpu_cycles + speed_scale - 1) / speed_scale);
}

static void gb_timer_advance_divider(struct gb_timer *timer,
                                     uint32_t cpu_cycles)
{
    timer->divider_counter = (timer->divider_counter + cpu_cycles) & 0xffff;
}

static bool gb_timer_signal(const struct gb_timer *timer)
{
    unsigned bit = gb_timer_div_bit(timer->divider);

    return timer->started && ((timer->divider_counter >> bit) & 1);
}

static uint32_t gb_timer_cycles_to_falling_edge(const struct gb_timer *timer)
{
    unsigned bit = gb_timer_div_bit(timer->divider);
    uint32_t period = 1U << (bit + 1);
    uint32_t phase = timer->divider_counter & (period - 1);

    return period - phase;
}

static void gb_timer_increment_tima(struct gb *gb)
{
    struct gb_timer *timer = &gb->timer;

    if (timer->counter == 0xff)
    {
        timer->counter = 0;
        timer->reload_pending = true;
        timer->reload_cycles = 0;
    }
    else
    {
        timer->counter++;
    }
}

static bool gb_timer_reload_due_now(struct gb *gb)
{
    struct gb_timer *timer = &gb->timer;
    int32_t elapsed;
    uint32_t elapsed_cpu;
    uint32_t remaining;

    if (!timer->reload_pending)
    {
        return false;
    }

    elapsed = gb->timestamp - gb->sync.last_sync[GB_SYNC_TIMER];
    if (elapsed < 0)
    {
        elapsed = 0;
    }

    elapsed_cpu = (uint32_t)elapsed << gb->double_speed;
    remaining = 4 - timer->reload_cycles;

    return elapsed_cpu >= remaining;
}

static bool gb_timer_reload_at_timestamp(const struct gb *gb)
{
    return gb->timer.reload_just_happened &&
           gb->timer.reload_timestamp == gb->timestamp;
}

static void gb_timer_schedule_next(struct gb *gb, unsigned div,
                                   unsigned speed_scale)
{
    struct gb_timer *timer = &gb->timer;
    uint32_t next;

    if (timer->reload_pending)
    {
        uint32_t remaining = 4 - timer->reload_cycles;

        gb_sync_next(gb, GB_SYNC_TIMER,
                     gb_timer_to_timestamp_cycles(remaining, speed_scale));
        return;
    }

    (void)div;

    if (!timer->started)
    {
        gb_sync_next(gb, GB_SYNC_TIMER, GB_SYNC_NEVER);
        return;
    }

    next = gb_timer_cycles_to_falling_edge(timer);
    gb_sync_next(gb, GB_SYNC_TIMER,
                 gb_timer_to_timestamp_cycles(next, speed_scale));
}

void gb_timer_reset(struct gb *gb)
{
    struct gb_timer *timer = &gb->timer;
    enum gb_hw_model model = gb->gbc ? GB_HW_CGB : gb->hw_model;

    if (gb->bootrom)
    {
        timer->divider_counter = 0;
    }
    else
    {
        switch (model)
        {
        case GB_HW_DMG0:
            timer->divider_counter = 0x0000;
            break;
        case GB_HW_DMG:
        case GB_HW_MGB:
            timer->divider_counter = 0xabcc;
            break;
        case GB_HW_SGB:
        case GB_HW_SGB2:
            timer->divider_counter = 0xd8f8;
            break;
        default: /* GB_HW_CGB */
            timer->divider_counter = 0;
            break;
        }
    }
    timer->counter = 0;
    timer->modulo = 0;
    timer->divider = GB_TIMER_DIV_1024;
    timer->started = false;
    timer->reload_pending = false;
    timer->reload_cycles = 0;
    timer->reload_just_happened = false;
    timer->reload_timestamp = -1;
}

void gb_timer_sync(struct gb *gb)
{
    struct gb_timer *timer = &gb->timer;
    int32_t elapsed = gb_sync_resync(gb, GB_SYNC_TIMER);
    uint32_t elapsed_cpu = (uint32_t)elapsed << gb->double_speed;
    unsigned speed_scale = 1U << gb->double_speed;
    unsigned div = gb_timer_div_cycles(timer->divider);

    timer->reload_just_happened = false;

    for (;;)
    {
        /* Hardware delays TMA->TIMA reload by 4 T-cycles after overflow.
         * During this window TIMA reads as 0x00; a write to TIMA cancels it. */
        if (timer->reload_pending)
        {
            uint32_t remaining = 4 - timer->reload_cycles;

            if (elapsed_cpu < remaining)
            {
                gb_timer_advance_divider(timer, elapsed_cpu);
                timer->reload_cycles += elapsed_cpu;
                gb_timer_schedule_next(gb, div, speed_scale);
                return;
            }

            gb_timer_advance_divider(timer, remaining);
            elapsed_cpu -= remaining;
            timer->reload_pending = false;
            timer->reload_cycles = 0;
            timer->counter = timer->modulo;
            timer->reload_just_happened = true;
            timer->reload_timestamp = gb->timestamp -
                                      gb_timer_to_timestamp_cycles(elapsed_cpu,
                                                                   speed_scale);
            gb_irq_trigger(gb, GB_IRQ_TIMER);
        }

        if (elapsed_cpu == 0)
        {
            gb_timer_schedule_next(gb, div, speed_scale);
            return;
        }

        if (!timer->started)
        {
            gb_timer_advance_divider(timer, elapsed_cpu);
            gb_timer_schedule_next(gb, div, speed_scale);
            return;
        }

        {
            uint32_t to_edge = gb_timer_cycles_to_falling_edge(timer);

            if (elapsed_cpu < to_edge)
            {
                gb_timer_advance_divider(timer, elapsed_cpu);
                gb_timer_schedule_next(gb, div, speed_scale);
                return;
            }

            gb_timer_advance_divider(timer, to_edge);
            elapsed_cpu -= to_edge;
            gb_timer_increment_tima(gb);
        }
    }
}

void gb_timer_reset_divider(struct gb *gb)
{
    bool old_signal;

    gb_timer_sync(gb);
    old_signal = gb_timer_signal(&gb->timer);
    gb->timer.divider_counter = 0;
    if (old_signal && !gb_timer_signal(&gb->timer))
    {
        gb_timer_increment_tima(gb);
    }
    gb_timer_sync(gb);
}

void gb_timer_write_counter(struct gb *gb, uint8_t value)
{
    bool was_reloading = gb->timer.reload_pending;
    bool reload_at_write = gb_timer_reload_at_timestamp(gb);

    gb_timer_sync(gb);

    if (reload_at_write ||
        (!was_reloading && gb_timer_reload_at_timestamp(gb)))
    {
        /* TIMA writes in the cycle where the delayed reload is applied are
         * ignored; later writes must not inherit this state. */
        return;
    }

    gb->timer.reload_pending = false;
    gb->timer.reload_cycles = 0;
    gb->timer.counter = value;
    gb_timer_sync(gb);
}

void gb_timer_write_modulo(struct gb *gb, uint8_t value)
{
    bool reload_due = gb_timer_reload_due_now(gb);
    bool reload_at_write = gb_timer_reload_at_timestamp(gb);

    gb_timer_sync(gb);
    gb->timer.modulo = value;
    if (reload_due || reload_at_write || gb_timer_reload_at_timestamp(gb))
    {
        gb->timer.counter = value;
    }
    gb_timer_sync(gb);
}

void gb_timer_set_config(struct gb *gb, uint8_t config)
{
    struct gb_timer *timer = &gb->timer;
    bool old_signal;

    gb_timer_sync(gb);

    old_signal = gb_timer_signal(timer);
    timer->started = config & 4;
    timer->divider = config & 3;
    if (old_signal && !gb_timer_signal(timer))
    {
        gb_timer_increment_tima(gb);
    }

    gb_timer_sync(gb);
}

uint8_t gb_timer_get_config(struct gb *gb)
{
    struct gb_timer *timer = &gb->timer;
    uint8_t cfg = 0xf8;

    cfg |= timer->divider;

    if (timer->started)
    {
        cfg |= 4;
    }

    return cfg;
}
