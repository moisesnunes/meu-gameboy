
#ifndef _GB_IRQ_H_
#define _GB_IRQ_H_

enum gb_irq_token
{
     GB_IRQ_VSYNC = 0,    /* VBlank — fim do último scanline visível (LY 143→144) */
     GB_IRQ_LCD_STAT = 1, /* LCD STAT — condição configurada em 0xFF41 (LYC, modos) */
     GB_IRQ_TIMER = 2,    /* Timer — overflow do registrador TIMA (0xFF05) */
     GB_IRQ_SERIAL = 3,   /* Serial — transferência via link cable concluída */
     GB_IRQ_INPUT = 4,    /* Joypad — borda descendente em botão selecionado */
};

struct gb_irq
{
     /* Registrador IF (0xFF0F): bits pendentes de interrupção (escrita seta o bit) */
     uint8_t irq_flags;
     /* Registrador IE (0xFFFF): máscara de interrupções habilitadas pela ROM */
     uint8_t irq_enable;
};

void gb_irq_reset(struct gb *gb);
void gb_irq_trigger(struct gb *gb, enum gb_irq_token which);

#endif /* _GB_IRQ_H_ */
