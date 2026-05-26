#include <stdio.h>
#include "gba.h"

void gba_sync_reset(struct gba *gba)
{
     struct gba_sync *sync = &gba->sync;
     unsigned i;

     for (i = 0; i < GBA_SYNC_NUM; i++)
     {
          sync->last_sync[i] = 0;
          sync->next_event[i] = GBA_SYNC_NEVER;
     }

     gba->timestamp = 0;
     sync->first_event = GBA_SYNC_NEVER;
}

int32_t gba_sync_resync(struct gba *gba, enum gba_sync_token token)
{
     struct gba_sync *sync = &gba->sync;
     int32_t elapsed = gba->timestamp - sync->last_sync[token];

     if (elapsed < 0)
          fprintf(stderr, "GBA: negative sync %d for token %u\n", elapsed, token);

     sync->last_sync[token] = gba->timestamp;
     return elapsed;
}

void gba_sync_next(struct gba *gba, enum gba_sync_token token, int32_t cycles)
{
     struct gba_sync *sync = &gba->sync;
     unsigned i;

     sync->next_event[token] = gba->timestamp + cycles;

     sync->first_event = sync->next_event[0];
     for (i = 1; i < GBA_SYNC_NUM; i++)
     {
          int32_t e = sync->next_event[i];
          if (e < sync->first_event)
               sync->first_event = e;
     }
}

void gba_sync_check_events(struct gba *gba)
{
     struct gba_sync *sync = &gba->sync;
     int32_t ts = gba->timestamp;

     if (ts >= sync->next_event[GBA_SYNC_GPU])
          gba_gpu_sync(gba);

     if (ts >= sync->next_event[GBA_SYNC_DMA])
          gba_dma_sync(gba);

     if (ts >= sync->next_event[GBA_SYNC_TIMER])
          gba_timer_sync(gba);

     if (ts >= sync->next_event[GBA_SYNC_APU])
          gba_apu_sync(gba);

     if (ts >= sync->next_event[GBA_SYNC_CART])
          gba_cart_sync(gba);
}

void gba_sync_rebase(struct gba *gba)
{
     struct gba_sync *sync = &gba->sync;
     unsigned i;

     for (i = 0; i < GBA_SYNC_NUM; i++)
     {
          sync->last_sync[i] -= gba->timestamp;
          sync->next_event[i] -= gba->timestamp;
     }

     sync->first_event -= gba->timestamp;
     gba->timestamp = 0;
}
