#ifndef _GB_FRONTEND_H_
#define _GB_FRONTEND_H_

/*
 * Interface entre o núcleo do emulador e o frontend de exibição.
 * O núcleo chama esses callbacks sem conhecer os detalhes de renderização;
 * cada frontend (SDL, ImGui, headless, etc.) preenche os ponteiros ao inicializar.
 * Todos os ponteiros são obrigatórios, exceto onde indicado.
 */
struct gb_frontend
{
     /* Renderiza uma linha horizontal no modo DMG (paleta de 4 tons).
      * Chamado pela PPU a cada scanline visível (ly 0..143). */
     void (*draw_line_dmg)(struct gb *gb, unsigned ly,
                           union gb_gpu_color col[GB_LCD_WIDTH]);

     /* Renderiza uma linha horizontal no modo GBC (cor de 15 bits).
      * Chamado no lugar de draw_line_dmg quando o cartucho é GBC. */
     void (*draw_line_gbc)(struct gb *gb, unsigned ly,
                           union gb_gpu_color col[GB_LCD_WIDTH]);

     /* Chamado ao fim de cada frame (após a linha 143), indicando que o
      * buffer está completo e pronto para ser apresentado na tela. */
     void (*flip)(struct gb *gb);

     /* Lê o estado atual dos botões e atualiza gb_input.
      * Chamado pelo núcleo antes de processar interrupções de joypad. */
     void (*refresh_input)(struct gb *gb);

     /* Libera todos os recursos do frontend. Chamado quando gb->quit é true
      * ou quando a ROM é descarregada definitivamente. */
     void (*destroy)(struct gb *gb);

     /* Ponteiro opaco para dados privados do frontend (contexto SDL, janela
      * ImGui, etc.). O núcleo nunca acessa este campo diretamente. */
     void *data;
};

#endif /* _GB_FRONTEND_H_ */
