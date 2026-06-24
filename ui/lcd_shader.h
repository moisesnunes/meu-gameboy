#ifndef LCD_SHADER_H
#define LCD_SHADER_H

#include <stdbool.h>

/* Preset de shader LCD */
typedef enum {
    LCD_SHADER_NONE       = 0,  /* sem efeito (pass-through) */
    LCD_SHADER_DMG        = 1,  /* ghosting verde clássico do Game Boy DMG */
    LCD_SHADER_GBC        = 2,  /* grade de subpixel RGB do GBC/GBA */
    LCD_SHADER_CRT        = 3,  /* scanlines + curvatura suave estilo CRT */
} lcd_shader_preset;

/* Parâmetros ajustáveis por preset */
typedef struct {
    lcd_shader_preset preset;
    float ghost_strength;   /* [0,1] intensidade do ghosting (DMG)       */
    float subpixel_str;     /* [0,1] intensidade da grade de subpixel    */
    float scanline_str;     /* [0,1] intensidade das scanlines            */
    float brightness;       /* [0.5,2.0] brilho geral                    */
} lcd_shader_params;

/* Inicializa shaders e FBO. Chame uma vez após contexto OpenGL estar ativo. */
bool lcd_shader_init(int src_w, int src_h);

/* Libera todos os recursos OpenGL do módulo. */
void lcd_shader_destroy(void);

/* Processa src_tex através do shader e retorna o ID da textura do FBO resultante.
   Retorna src_tex inalterado se preset == LCD_SHADER_NONE ou se houve erro. */
unsigned int lcd_shader_process(unsigned int src_tex, unsigned int prev_tex, bool prev_valid,
                                const lcd_shader_params *params,
                                int display_w, int display_h);

/* Parâmetros padrão para cada preset */
lcd_shader_params lcd_shader_defaults(lcd_shader_preset preset);

/* Nome legível do preset */
const char *lcd_shader_preset_name(lcd_shader_preset preset);

#endif /* LCD_SHADER_H */
