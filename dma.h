#ifndef _GB_DMA_H_
#define _GB_DMA_H_

struct gb_dma
{
     bool running;
     
     /* True when FF46 was written while a previous OAM DMA was still active. */
     bool restarting;

     /* True while gb_dma_sync is executing a memory read, preventing the CPU
      * access check in gb_memory_readb from blocking the DMA engine itself. */
     bool syncing;

     /* Source address */
     uint16_t source;

     /* Number of bytes copied so far */
     uint8_t position;

     /* Timestamp ticks until the next byte is copied. */
     uint8_t delay;
};

void gb_dma_reset(struct gb *gb);
void gb_dma_sync(struct gb *gb);
void gb_dma_start(struct gb *gb, uint8_t source);

#endif /* _GB_DMA_H_ */
