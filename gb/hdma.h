#ifndef _GB_HDMA_H_
#define _GB_HDMA_H_

struct gb_hdma
{
     /* Endereço fonte atual (avança a cada byte copiado) */
     uint16_t source;
     /* Offset de destino na VRAM (espelhado para 0x8000–0x9FFF) */
     uint16_t destination;
     /* Comprimento restante codificado: valor 0 = 0x10 bytes restantes;
      * valor N = (N + 1) * 0x10 bytes. Ex.: 0x42 → (0x42+1)*0x10 = 670 bytes.
      * Ao concluir, o hardware escreve 0x7F no registrador HDMA5. */
     uint8_t length;
     /* true enquanto o modo H-Blank DMA está ativo (transfere 0x10 bytes por HBlank) */
     bool run_on_hblank;
};

void gb_hdma_start(struct gb *gb, bool hblank);
void gb_hdma_hblank(struct gb *gb);

#endif /* _GB_HDMA_H_ */
