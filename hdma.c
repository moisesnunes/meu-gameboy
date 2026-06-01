#include "gb.h"

static void gb_hdma_copy(struct gb *gb, uint16_t len)
{
     struct gb_hdma *hdma = &gb->hdma;
     uint16_t src = hdma->source;
     uint16_t dst = hdma->destination;

     /* Cada byte copiado consome ~2 T-cycles */
     gb->timestamp += len * 2;

     while (len--)
     {
          /* O destino é sempre espelhado para a VRAM (0x8000–0x9FFF) */
          uint16_t vram_addr = 0x8000U + (dst % 0x2000U);

          uint8_t v = gb_memory_readb(gb, src);
          gb_memory_writeb(gb, vram_addr, v);

          src++;
          dst++;
     }

     hdma->source = src;
     hdma->destination = dst;
}

/* Chamado pela GPU a cada HBlank quando hdma->run_on_hblank é true.
 * Copia um chunk de 16 bytes e decrementa o contador de blocos restantes. */
void gb_hdma_hblank(struct gb *gb)
{
     struct gb_hdma *hdma = &gb->hdma;

     gb_hdma_copy(gb, 0x10);

     if (hdma->length == 0)
     {
          /* Transferência concluída: desativa o modo HBlank e sinaliza fim (0x7F) */
          hdma->run_on_hblank = false;
          hdma->length = 0x7f;
     }
     else
     {
          hdma->length--;
     }
}

void gb_hdma_start(struct gb *gb, bool hblank)
{
     struct gb_hdma *hdma = &gb->hdma;

     if (hblank)
     {
          /* Modo H-Blank DMA: a cópia é feita em chunks de 16 bytes por HBlank.
           * A GPU chama gb_hdma_hblank() a cada Mode 0 até length chegar a zero. */
          gb_gpu_sync(gb);
          hdma->run_on_hblank = true;

          /* Se já estamos em HBlank (Mode 0) no momento da escrita, o hardware
           * transfere o primeiro chunk de 16 bytes imediatamente, sem esperar
           * pela próxima transição Mode 3→0. */
          if (gb->gpu.master_enable && gb_gpu_get_mode(gb) == 0)
          {
               gb_hdma_hblank(gb);
          }
     }
     else
     {
          /* Modo General Purpose DMA: transferência completa em um único passo */
          uint16_t len = (hdma->length + 1) * 0x10;

          gb_hdma_copy(gb, len);

          /* Transferência concluída */
          hdma->run_on_hblank = false;
          hdma->length = 0x7f;
     }
}
