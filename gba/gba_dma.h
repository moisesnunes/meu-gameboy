#ifndef _GBA_DMA_H_
#define _GBA_DMA_H_

#include <stdint.h>
#include <stdbool.h>

struct gba;

#define GBA_DMA_COUNT 4

enum gba_dma_addr_mode
{
     GBA_DMA_ADDR_INC = 0,
     GBA_DMA_ADDR_DEC = 1,
     GBA_DMA_ADDR_FIXED = 2,
     GBA_DMA_ADDR_RELOAD = 3, /* dest only: reload each repeat */
};

enum gba_dma_timing
{
     GBA_DMA_NOW = 0,
     GBA_DMA_VBLANK = 1,
     GBA_DMA_HBLANK = 2,
     GBA_DMA_SPECIAL = 3, /* audio FIFO or video capture */
};

struct gba_dma_channel
{
     uint32_t src;       /* source address (internal running) */
     uint32_t dst;       /* destination address (internal running) */
     uint32_t src_latch; /* latched on enable */
     uint32_t dst_latch;
     uint32_t data_latch;
     uint16_t count_latch; /* word count latched on enable */
     uint16_t count;       /* remaining words */

     /* DMACNT_H fields */
     enum gba_dma_addr_mode dst_mode;
     enum gba_dma_addr_mode src_mode;
     bool repeat;
     bool word_32;     /* false = 16-bit, true = 32-bit */
     bool gamepak_drq; /* DMA3 only */
     enum gba_dma_timing timing;
     bool irq_en;
     bool enable;

     bool pending; /* transfer triggered, waiting to run */
};

struct gba_dma
{
     struct gba_dma_channel ch[GBA_DMA_COUNT];
};

void gba_dma_reset(struct gba *gba);
void gba_dma_sync(struct gba *gba);
void gba_dma_write_src(struct gba *gba, int n, uint32_t val);
void gba_dma_write_dst(struct gba *gba, int n, uint32_t val);
void gba_dma_write_count(struct gba *gba, int n, uint16_t val);
void gba_dma_write_ctrl(struct gba *gba, int n, uint16_t val);
void gba_dma_notify_vblank(struct gba *gba);
void gba_dma_notify_hblank(struct gba *gba);
void gba_dma_notify_fifo(struct gba *gba, int fifo); /* 0=FIFO_A, 1=FIFO_B */

#endif /* _GBA_DMA_H_ */
