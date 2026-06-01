#include <stdio.h>
#include "gb.h"

void gb_sync_reset(struct gb *gb)
{
     struct gb_sync *sync = &gb->sync;
     unsigned i;

     for (i = 0; i < GB_SYNC_NUM; i++)
     {
          sync->last_sync[i] = 0;
          sync->next_event[i] = 0;
     }

     gb->timestamp = 0;
     sync->first_event = 0;
}

int32_t gb_sync_resync(struct gb *gb, enum gb_sync_token token)
{
     struct gb_sync *sync = &gb->sync;
     int32_t elapsed = gb->timestamp - sync->last_sync[token];

     if (elapsed < 0)
     {
          /* Nunca deveria acontecer — indica rebase ausente ou bug no agendamento */
          fprintf(stderr, "Got negative sync %d for token %u",
                  elapsed, token);
     }

     sync->last_sync[token] = gb->timestamp;

     return elapsed;
}

void gb_sync_next(struct gb *gb, enum gb_sync_token token, int32_t cycles)
{
     struct gb_sync *sync = &gb->sync;
     unsigned i;

     sync->next_event[token] = gb->timestamp + cycles;

     /* Recalcula first_event como o mínimo de todos os next_event */
     sync->first_event = sync->next_event[0];

     for (i = 1; i < GB_SYNC_NUM; i++)
     {
          int32_t e = sync->next_event[i];
          if (e < sync->first_event)
          {
               sync->first_event = e;
          }
     }
}

void gb_sync_check_events(struct gb *gb)
{
     struct gb_sync *sync = &gb->sync;
     int32_t ts = gb->timestamp;

     if (ts >= sync->next_event[GB_SYNC_GPU])
     {
          gb_gpu_sync(gb);
     }

     if (ts >= sync->next_event[GB_SYNC_DMA])
     {
          gb_dma_sync(gb);
     }

     if (ts >= sync->next_event[GB_SYNC_TIMER])
     {
          gb_timer_sync(gb);
     }

     if (ts >= sync->next_event[GB_SYNC_SPU])
     {
          gb_spu_sync(gb);
     }

     if (ts >= sync->next_event[GB_SYNC_CART])
     {
          gb_cart_sync(gb);
     }

     if (ts >= sync->next_event[GB_SYNC_SERIAL])
     {
          gb_serial_sync(gb);
     }
}

/* Subtrai o timestamp atual de todas as datas de last_sync e next_event,
 * prevenindo overflow do int32_t sem perder a relação temporal entre os eventos. */
void gb_sync_rebase(struct gb *gb)
{
     struct gb_sync *sync = &gb->sync;
     unsigned i;

     for (i = 0; i < GB_SYNC_NUM; i++)
     {
          sync->last_sync[i] -= gb->timestamp;
          sync->next_event[i] -= gb->timestamp;
     }

     sync->first_event -= gb->timestamp;
     gb->timestamp = 0;
}
