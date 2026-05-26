#ifndef _GBA_SYNC_H_
#define _GBA_SYNC_H_

#include <stdint.h>

#define GBA_SYNC_NEVER 20000000

struct gba;

enum gba_sync_token
{
     GBA_SYNC_GPU = 0,
     GBA_SYNC_APU = 1,
     GBA_SYNC_DMA = 2,
     GBA_SYNC_TIMER = 3,
     GBA_SYNC_CART = 4,

     GBA_SYNC_NUM
};

struct gba_sync
{
     int32_t first_event;
     int32_t last_sync[GBA_SYNC_NUM];
     int32_t next_event[GBA_SYNC_NUM];
};

void gba_sync_reset(struct gba *gba);
int32_t gba_sync_resync(struct gba *gba, enum gba_sync_token token);
void gba_sync_next(struct gba *gba, enum gba_sync_token token, int32_t cycles);
void gba_sync_check_events(struct gba *gba);
void gba_sync_rebase(struct gba *gba);

#endif /* _GBA_SYNC_H_ */
