#ifndef _GB_TIMER_H_
#define _GB_TIMER_H_

/*
 * Seletor de divisor do TIMA (registrador TAC bits 1:0).
 * O divisor é aplicado ao contador interno de 16 bits (DIV interno).
 * TIMA incrementa na borda descendente do bit selecionado desse contador.
 */
enum gb_timer_divider
{
     GB_TIMER_DIV_1024 = 0, /* bit 9  → TIMA a   4096 Hz */
     GB_TIMER_DIV_16   = 1, /* bit 3  → TIMA a 262144 Hz */
     GB_TIMER_DIV_64   = 2, /* bit 5  → TIMA a  65536 Hz */
     GB_TIMER_DIV_256  = 3, /* bit 7  → TIMA a  16384 Hz */
};

struct gb_timer
{
     /* Contador interno de 16 bits avançado a cada T-cycle (registrador DIV = byte alto) */
     uint16_t divider_counter;
     /* Registrador TIMA (0xFF05): incrementado pelo timer */
     uint8_t counter;
     /* Registrador TMA (0xFF06): valor carregado no TIMA após overflow */
     uint8_t modulo;
     /* Divisor selecionado via TAC bits 1:0 */
     enum gb_timer_divider divider;
     /* TAC bit 2: true quando o timer está habilitado */
     bool started;
     /* TIMA estourou; aguarda 4 T-cycles para carregar TMA e disparar IRQ.
      * Durante esta janela, TIMA lê como 0x00 e uma escrita cancela o reload. */
     bool reload_pending;
     /* T-cycles da CPU já decorridos dentro da janela de reload pendente */
     unsigned reload_cycles;
     /* true no ciclo imediatamente após o reload ser aplicado */
     bool reload_just_happened;
     /* Timestamp em que o reload foi aplicado (usado para detectar escrita simultânea) */
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
