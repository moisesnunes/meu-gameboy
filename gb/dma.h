#ifndef _GB_DMA_H_
#define _GB_DMA_H_

struct gb_dma
{
     bool running;

     /* Verdadeiro quando FF46 foi escrito enquanto um OAM DMA anterior ainda estava ativo. */
     bool restarting;

     /* Verdadeiro enquanto gb_dma_sync está executando uma leitura de memória, impedindo que
      * a verificação de acesso da CPU em gb_memory_readb bloqueie o próprio motor de DMA. */
     bool syncing;

     /* Endereço fonte */
     uint16_t source;

     /* Número de bytes copiados até agora */
     uint8_t position;

     /* Ticks de timestamp até o próximo byte ser copiado. */
     uint8_t delay;
};

void gb_dma_reset(struct gb *gb);
void gb_dma_sync(struct gb *gb);
void gb_dma_start(struct gb *gb, uint8_t source);

#endif /* _GB_DMA_H_ */
