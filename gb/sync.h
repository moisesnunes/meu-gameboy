#ifndef _GB_SYNC_H_
#define _GB_SYNC_H_

/*
 * Motor de sincronização lazy entre a CPU e os periféricos.
 *
 * A CPU avança livremente incrementando gb->timestamp. Cada periférico
 * (GPU, DMA, Timer, APU, etc.) registra quando precisa ser chamado em
 * next_event[token]. A CPU só chama gb_sync_check_events() quando
 * timestamp >= first_event, minimizando o overhead por instrução.
 *
 * Cada periférico é responsável por reagendar seu próximo evento via
 * gb_sync_next() ao final de cada gb_*_sync().
 */

/* Valor sentinela usado quando um periférico não tem evento agendado.
 * Garante que ele só seja sincronizado a uma frequência muito baixa. */
#define GB_SYNC_NEVER 10000000

/* Identificadores de cada periférico no sistema de sync */
enum gb_sync_token
{
     GB_SYNC_GPU = 0,
     GB_SYNC_DMA,
     GB_SYNC_TIMER,
     GB_SYNC_CART,
     GB_SYNC_SPU,
     GB_SYNC_SERIAL,

     GB_SYNC_NUM /* número total de tokens — usado para dimensionar arrays */
};

struct gb_sync
{
     /* Menor valor em next_event — comparado com timestamp a cada instrução */
     int32_t first_event;
     /* Timestamp no momento da última sincronização de cada periférico */
     int32_t last_sync[GB_SYNC_NUM];
     /* Timestamp em que cada periférico precisa ser sincronizado novamente */
     int32_t next_event[GB_SYNC_NUM];
};

void gb_sync_reset(struct gb *gb);

/* Sincroniza o token e retorna o número de T-cycles desde o último sync */
int32_t gb_sync_resync(struct gb *gb, enum gb_sync_token token);
/* Agenda o próximo evento do token para daqui a `cycles` T-cycles */
void gb_sync_next(struct gb *gb, enum gb_sync_token token, int32_t cycles);
/* Verifica e despacha todos os periféricos cujo next_event foi atingido */
void gb_sync_check_events(struct gb *gb);
/* Rebases todos os timestamps para zero, prevenindo overflow do int32_t */
void gb_sync_rebase(struct gb *gb);

#endif /* _GB_SYNC_H_ */
