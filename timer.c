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
          die();
          return 1024;
     }
}

/* Retorna o índice do bit do divider_counter que aciona o TIMA.
 * TIMA incrementa na borda descendente deste bit. */
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

/* Converte T-cycles da CPU em timestamp cycles, dividindo pelo fator de
 * velocidade (1 no DMG, 2 no double-speed). Arredonda para cima para não
 * adiantar eventos. */
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

/* Retorna true se o sinal que aciona o TIMA está atualmente alto.
 * O TIMA incrementa quando este sinal cai de 1 para 0 (borda descendente). */
static bool gb_timer_signal(const struct gb_timer *timer)
{
     unsigned bit = gb_timer_div_bit(timer->divider);

     return timer->started && ((timer->divider_counter >> bit) & 1);
}

/* Retorna quantos T-cycles da CPU faltam até a próxima borda descendente
 * do bit selecionado no divider_counter. */
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

/* Verifica se a janela de 4 T-cycles de reload do TMA já terminou sem um sync
 * completo. Usado em gb_timer_write_modulo para decidir se aplica o novo valor
 * imediatamente ao TIMA. */
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

/* Retorna true se o reload de TMA→TIMA ocorreu exatamente neste timestamp.
 * Escritas em TIMA neste ciclo são ignoradas pelo hardware. */
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
          /* Boot ROM executa do zero; divider começa em 0 */
          timer->divider_counter = 0;
     }
     else
     {
          /* Valores do divider_counter no momento em que o boot ROM termina,
           * medidos por hardware real para cada modelo. Permitem pular o boot ROM
           * sem perder a fase correta do timer. */
          switch (model)
          {
          case GB_HW_DMG0:
               timer->divider_counter = 0x1830;
               break;
          case GB_HW_DMG:
          case GB_HW_MGB:
               timer->divider_counter = 0xabcc;
               break;
          case GB_HW_SGB:
               timer->divider_counter = 0xd860;
               break;
          case GB_HW_SGB2:
               timer->divider_counter = 0xd850;
               break;
          case GB_HW_CGB0:
               timer->divider_counter = 0x2884;
               break;
          default: /* GB_HW_CGB */
               timer->divider_counter = 0x2678;
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
          /* O hardware atrasa o reload TMA→TIMA em 4 T-cycles após o overflow.
           * Nesta janela TIMA lê como 0x00; uma escrita em TIMA cancela o reload. */
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
               gb_debug_hw_trace_timer_ovf(gb, timer->modulo);
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

/* Chamado quando a CPU escreve em DIV (0xFF04). Zerar o DIV pode criar uma
 * borda descendente artificial no bit selecionado, incrementando TIMA. */
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

/* Escrita em TIMA (0xFF05).
 * Escrita no mesmo ciclo em que o reload de TMA é aplicado é ignorada pelo hardware.
 * Escrita durante a janela de pendência (antes dos 4 T-cycles) cancela o reload. */
void gb_timer_write_counter(struct gb *gb, uint8_t value)
{
     bool was_reloading = gb->timer.reload_pending;
     bool reload_at_write = gb_timer_reload_at_timestamp(gb);

     gb_timer_sync(gb);

     if (reload_at_write ||
         (!was_reloading && gb_timer_reload_at_timestamp(gb)))
     {
          /* Escrita no ciclo exato do reload: ignorada; não propagar o estado. */
          return;
     }

     gb->timer.reload_pending = false;
     gb->timer.reload_cycles = 0;
     gb->timer.counter = value;
     gb_timer_sync(gb);
}

/* Escrita em TMA (0xFF06).
 * Se o reload já está pendente ou acaba de ocorrer, o novo valor de TMA é
 * aplicado imediatamente ao TIMA também (comportamento do hardware real). */
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

/* Escrita em TAC (0xFF07). Mudar o divisor ou desabilitar o timer pode
 * criar uma borda descendente no bit selecionado, incrementando TIMA. */
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

/* Leitura de TAC (0xFF07). Bits 7:3 leem sempre como 1 no hardware real. */
uint8_t gb_timer_get_config(struct gb *gb)
{
     struct gb_timer *timer = &gb->timer;
     uint8_t cfg = 0xf8; /* bits superiores sempre 1 */

     cfg |= timer->divider;

     if (timer->started)
     {
          cfg |= 4;
     }

     return cfg;
}
