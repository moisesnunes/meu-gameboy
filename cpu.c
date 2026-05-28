/*
 * cpu.c - Emulação da CPU do Game Boy (Sharp LR35902)
 *
 * Este arquivo implementa o ciclo de execução da CPU do Game Boy.
 * A CPU do Game Boy é baseada no processador Z80/8080 da Intel, operando
 * a 4.194304 MHz. A execução funciona em ciclos de fetch-decode-execute:
 *   1. Lê o opcode apontado pelo PC (Program Counter)
 *   2. Incrementa o PC
 *   3. Despacha para a função de instrução correspondente
 *
 * Registradores da CPU (LR35902):
 *   - A  : acumulador de 8 bits (operações aritméticas/lógicas)
 *   - F  : registrador de flags (Z, N, H, C nos bits 7-4; bits 3-0 sempre 0)
 *   - B, C, D, E, H, L : registradores de propósito geral de 8 bits
 *   - BC, DE, HL : pares de registradores de 16 bits (B=MSB, C=LSB, etc.)
 *   - SP : Stack Pointer de 16 bits (pilha cresce para baixo)
 *   - PC : Program Counter de 16 bits (endereço da próxima instrução)
 *
 * Flags (registrador F):
 *   - Z (Zero)       : setado se o resultado da operação é 0
 *   - N (Subtract)   : setado se a última operação foi uma subtração
 *   - H (Half Carry) : carry do nibble baixo para o nibble alto (bit 3→4)
 *   - C (Carry)      : carry ou borrow na operação de 8/16 bits
 *
 * Mapa de memória resumido do Game Boy:
 *   0x0000–0x7FFF : ROM do cartucho (bancos 0 e 1+)
 *   0x8000–0x9FFF : VRAM (Video RAM)
 *   0xA000–0xBFFF : RAM externa do cartucho
 *   0xC000–0xDFFF : WRAM (Work RAM interna)
 *   0xE000–0xFDFF : Echo RAM (espelho de 0xC000–0xDDFF)
 *   0xFE00–0xFE9F : OAM (Object Attribute Memory / sprites)
 *   0xFEA0–0xFEFF : Área não utilizável
 *   0xFF00–0xFF7F : Registradores de I/O (hardware registers)
 *   0xFF80–0xFFFE : HRAM (High RAM / Zero Page)
 *   0xFFFF        : Registrador IE (Interrupt Enable)
 */

#include <stdio.h>
#include <assert.h>
#include "gb.h"
#include "debug.h"
#include "disasm.h"

/*
 * gb_cpu_reset - Inicializa o estado da CPU para o ponto de entrada padrão.
 *
 * No hardware real, o Game Boy executa uma ROM de boot interna (0x0000–0x00FF)
 * que inicializa o hardware e exibe o logo da Nintendo. Como o boot ROM ainda
 * não é emulado, o PC é definido diretamente para 0x0100, que é o ponto de
 * entrada padrão de qualquer cartucho de Game Boy.
 *
 * Após o boot ROM real, os registradores ficam em estados conhecidos:
 *   A=0x01, F=0xB0 (Z=1,N=0,H=1,C=1), BC=0x0013, DE=0x00D8,
 *   HL=0x014D, SP=0xFFFE, PC=0x0100
 * Esta função faz uma inicialização mínima até que o boot ROM seja emulado.
 */
void gb_cpu_reset(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->irq_enable = false;
     cpu->irq_enable_next = false;
     cpu->irq_enable_delay = 0;

     cpu->halted = false;
     cpu->stopped = false;
     cpu->halt_bug = false;

     cpu->sp = 0xfffe;
     cpu->a = 0;
     cpu->b = 0;
     cpu->c = 0;
     cpu->d = 0;
     cpu->e = 0;
     cpu->h = 0;
     cpu->l = 0;

     if (gb->bootrom)
     {
          /* Boot ROM presente: CPU começa do zero, a ROM inicializa tudo */
          cpu->a = 0;
          cpu->b = 0;
          cpu->c = 0;
          cpu->d = 0;
          cpu->e = 0;
          cpu->h = 0;
          cpu->l = 0;
          cpu->f_z = false;
          cpu->f_n = false;
          cpu->f_h = false;
          cpu->f_c = false;
          cpu->pc = 0x0000;
          gb->bootrom_mapped = true;
     }
     else
     {
          /* Sem boot ROM: simula o estado dos registradores ao final da boot ROM real.
           * gb->gbc overrides hw_model for backward compatibility. */
          enum gb_hw_model model = gb->gbc ? GB_HW_CGB : gb->hw_model;
          switch (model)
          {
          case GB_HW_DMG0:
               /* DMG revision 0: F=0x00 (no flags set), B=0xFF */
               cpu->a = 0x01; cpu->b = 0xff; cpu->c = 0x13;
               cpu->d = 0x00; cpu->e = 0xc1; cpu->h = 0x84; cpu->l = 0x03;
               cpu->f_z = false; cpu->f_n = false; cpu->f_h = false; cpu->f_c = false;
               break;
          case GB_HW_MGB:
               /* Game Boy Pocket: A=0xFF F=0xB0 */
               cpu->a = 0xff; cpu->b = 0x00; cpu->c = 0x13;
               cpu->d = 0x00; cpu->e = 0xd8; cpu->h = 0x01; cpu->l = 0x4d;
               cpu->f_z = true; cpu->f_n = false; cpu->f_h = true; cpu->f_c = true;
               break;
          case GB_HW_SGB:
               /* Super Game Boy: C=0x14, no flags */
               cpu->a = 0x01; cpu->b = 0x00; cpu->c = 0x14;
               cpu->d = 0x00; cpu->e = 0x00; cpu->h = 0xc0; cpu->l = 0x60;
               cpu->f_z = false; cpu->f_n = false; cpu->f_h = false; cpu->f_c = false;
               break;
          case GB_HW_SGB2:
               /* Super Game Boy 2: A=0xFF C=0x14, no flags */
               cpu->a = 0xff; cpu->b = 0x00; cpu->c = 0x14;
               cpu->d = 0x00; cpu->e = 0x00; cpu->h = 0xc0; cpu->l = 0x60;
               cpu->f_z = false; cpu->f_n = false; cpu->f_h = false; cpu->f_c = false;
               break;
          case GB_HW_CGB0:
          case GB_HW_CGB:
               cpu->a = 0x11; cpu->b = 0x00; cpu->c = 0x00;
               cpu->d = 0x00; cpu->e = 0x08; cpu->h = 0x00; cpu->l = 0x7c;
               cpu->f_z = true; cpu->f_n = false; cpu->f_h = false; cpu->f_c = false;
               break;
          default: /* GB_HW_DMG */
               cpu->a = 0x01; cpu->b = 0x00; cpu->c = 0x13;
               cpu->d = 0x00; cpu->e = 0xd8; cpu->h = 0x01; cpu->l = 0x4d;
               cpu->f_z = true; cpu->f_n = false; cpu->f_h = true; cpu->f_c = true;
               break;
          }
          cpu->pc = 0x0100;
          gb->bootrom_mapped = false;
     }
}

static inline void gb_cpu_clock_tick(struct gb *gb, int32_t cycles)
{
     gb->timestamp += cycles >> gb->double_speed;

     if (gb->timestamp >= gb->sync.first_event)
     {
          /* Há um sync de dispositivo pendente */
          gb_sync_check_events(gb);
     }
}

/*
 * gb_cpu_readb - Lê um byte da memória e avança o timestamp em 4 ciclos.
 *
 * Toda leitura de memória pela CPU consome 4 ciclos de clock. Esta função
 * centraliza essa contabilidade, garantindo que o timestamp fique sincronizado.
 */
static uint8_t gb_cpu_readb(struct gb *gb, uint16_t addr)
{
     uint8_t b = gb_memory_readb(gb, addr);

     gb->debug.cpu_viz.addr_bus      = addr;
     gb->debug.cpu_viz.data_bus      = b;
     gb->debug.cpu_viz.bus_write     = false;
     gb->debug.cpu_viz.activity_fade = 1.0f;

     gb_cpu_clock_tick(gb, 4);

     return b;
}

/*
 * gb_cpu_writeb - Escreve um byte na memória e avança o timestamp em 4 ciclos.
 *
 * Simétrico de gb_cpu_readb: toda escrita de memória pela CPU consome 4 ciclos.
 */
static void gb_cpu_writeb(struct gb *gb, uint16_t addr, uint8_t val)
{
     gb_memory_writeb(gb, addr, val);

     gb->debug.cpu_viz.addr_bus      = addr;
     gb->debug.cpu_viz.data_bus      = val;
     gb->debug.cpu_viz.bus_write     = true;
     gb->debug.cpu_viz.activity_fade = 1.0f;

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_cpu_bc - Retorna o valor do par de registradores BC como uint16_t.
 *
 * B é o byte alto (MSB) e C é o byte baixo (LSB).
 * Os registradores são promovidos para 16 bits antes do deslocamento
 * para evitar comportamento indefinido em C.
 */
static uint16_t gb_cpu_bc(struct gb *gb)
{
     uint16_t b = gb->cpu.b;
     uint16_t c = gb->cpu.c;

     return (b << 8) | c;
}

/*
 * gb_cpu_set_bc - Armazena um valor de 16 bits no par de registradores BC.
 *
 * O byte alto (bits 15–8) vai para B e o byte baixo (bits 7–0) vai para C.
 */
static void gb_cpu_set_bc(struct gb *gb, uint16_t v)
{
     gb->cpu.b = v >> 8;
     gb->cpu.c = v & 0xff;
}

/*
 * gb_cpu_bc - Retorna o valor do par de registradores BC como uint16_t.
 *
 * B é o byte alto (MSB) e C é o byte baixo (LSB).
 * Os registradores são promovidos para 16 bits antes da operação
 * de deslocamento para evitar comportamento indefinido em C.
 */
static uint16_t gb_cpu_de(struct gb *gb)
{
     uint16_t d = gb->cpu.d;
     uint16_t e = gb->cpu.e;

     return (d << 8) | e;
}

/*
 * gb_cpu_set_de - Armazena um valor de 16 bits no par de registradores DE.
 *
 * O byte alto (bits 15–8) vai para D e o byte baixo (bits 7–0) vai para E.
 */
static void gb_cpu_set_de(struct gb *gb, uint16_t v)

{
     gb->cpu.d = v >> 8;
     gb->cpu.e = v & 0xff;
}

/*
 * gb_cpu_hl - Retorna o valor do par de registradores HL como uint16_t.
 *
 * H é o byte alto (MSB) e L é o byte baixo (LSB).
 * HL é o par de registradores mais utilizado como ponteiro de memória
 * nas instruções de load/store indireto do LR35902.
 */
static uint16_t gb_cpu_hl(struct gb *gb)
{
     uint16_t h = gb->cpu.h;
     uint16_t l = gb->cpu.l;

     return (h << 8) | l;
}

/*
 * gb_cpu_set_hl - Armazena um valor de 16 bits no par de registradores HL.
 *
 * O byte alto (bits 15–8) vai para H e o byte baixo (bits 7–0) vai para L.
 */
static void gb_cpu_set_hl(struct gb *gb, uint16_t v)
{
     gb->cpu.h = v >> 8;
     gb->cpu.l = v & 0xff;
}

/*
 * gb_cpu_dump - Imprime o estado atual da CPU na saída de erro padrão.
 *
 * Exibe o valor do PC com os três bytes seguintes na memória (útil para
 * inspecionar o opcode atual e seus operandos), o valor do SP e o
 * registrador A. Chamada antes de cada instrução por gb_cpu_run_instruction
 * para facilitar a depuração da execução passo a passo.
 */
void gb_cpu_dump(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     fprintf(stderr, "Flags: %c %c %c %c  IME: %d\n",
             cpu->f_z ? 'Z' : '-',
             cpu->f_n ? 'N' : '-',
             cpu->f_h ? 'H' : '-',
             cpu->f_c ? 'C' : '-',
             cpu->irq_enable);
     fprintf(stderr, "PC: 0x%04x [%02x %02x %02x]\n",
             cpu->pc,
             gb_memory_readb(gb, cpu->pc),
             gb_memory_readb(gb, cpu->pc + 1),
             gb_memory_readb(gb, cpu->pc + 2));
     fprintf(stderr, "SP: 0x%04x\n", cpu->sp);
     fprintf(stderr, "A : 0x%02x\n", cpu->a);
     fprintf(stderr, "B : 0x%02x  C : 0x%02x  BC : 0x%04x\n",
             cpu->b, cpu->c, gb_cpu_bc(gb));
     fprintf(stderr, "D : 0x%02x  E : 0x%02x  DE : 0x%04x\n",
             cpu->d, cpu->e, gb_cpu_de(gb));
     fprintf(stderr, "H : 0x%02x  L : 0x%02x  HL : 0x%04x\n",
             cpu->h, cpu->l, gb_cpu_hl(gb));
     fprintf(stderr, "\n");
}

/*
 * gb_run_load_pc - Atualiza o Program Counter (PC) para um novo endereço.
 *
 * Usado por instruções de salto (jump/call/return) para redirecionar
 * o fluxo de execução para um endereço arbitrário na memória.
 */
static void gb_cpu_load_pc(struct gb *gb, uint16_t new_pc)
{
     gb->cpu.pc = new_pc;

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_cpu_pushb - Empilha um byte na pilha do processador.
 *
 * Decrementa o SP em 1 (com wrap-around de 16 bits) e escreve o byte
 * no novo endereço apontado pelo SP. A pilha do Game Boy cresce para
 * baixo na memória, portanto o SP é sempre decrementado antes da escrita.
 */
static void gb_cpu_pushb(struct gb *gb, uint8_t b)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->sp = (cpu->sp - 1) & 0xffff;

     gb_cpu_writeb(gb, cpu->sp, b);
}

/*
 * gb_cpu_popb - Desempilha um byte da pilha do processador.
 *
 * Lê o byte no endereço atual do SP e incrementa o SP em 1 (com
 * wrap-around de 16 bits). Como a pilha cresce para baixo, incrementar
 * o SP equivale a "liberar" o topo da pilha.
 * Inverso de gb_cpu_pushb.
 */
static uint8_t gb_cpu_popb(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     uint8_t b = gb_cpu_readb(gb, cpu->sp);

     cpu->sp = (cpu->sp + 1) & 0xffff;

     return b;
}

/*
 * gb_cpu_pushw - Empilha um word (16 bits) na pilha do processador.
 *
 * Empilha primeiro o byte alto (MSB) e depois o byte baixo (LSB), de modo
 * que após as duas operações o SP aponte para o LSB — convenção big-endian
 * na pilha usada pelo Z80/LR35902. Usado por CALL para salvar o endereço
 * de retorno antes de desviar para a sub-rotina.
 */
static void gb_cpu_pushw(struct gb *gb, uint16_t w)
{
     gb_memory_trigger_oam_bug(gb, gb->cpu.sp);
     gb_cpu_pushb(gb, w >> 8);
     gb_cpu_pushb(gb, w & 0xff);
}

/*
 * gb_cpu_popw - Desempilha um word (16 bits) da pilha do processador.
 *
 * Lê dois bytes consecutivos via gb_cpu_popb e os recombina em um uint16_t.
 * O primeiro byte lido é o LSB (byte baixo) e o segundo é o MSB (byte alto),
 * invertendo a ordem de gb_cpu_pushw e restaurando o valor original de 16 bits.
 * Usado por RET para restaurar o endereço de retorno salvo por CALL.
 *
 * Retorna o valor de 16 bits recombinado corretamente.
 */
static uint16_t gb_cpu_popw(struct gb *gb)
{
     uint16_t b0 = gb_cpu_popb(gb); /* byte baixo (LSB) — empilhado por último */
     uint16_t b1 = gb_cpu_popb(gb); /* byte alto  (MSB) — empilhado primeiro   */

     return b0 | (b1 << 8);
}

/*
 * gb_cpu_next_i8 - Lê o próximo byte imediato do fluxo de instruções.
 *
 * Lê o byte no endereço atual do PC e avança o PC em 1, exceto no ciclo
 * afetado pelo HALT bug, quando o PC não é incrementado.
 * Usado para buscar opcodes e operandos de 8 bits.
 * O endereço é mascarado com 0xffff para garantir wrap-around de 16 bits.
 */
static uint8_t gb_cpu_next_i8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     uint8_t i8 = gb_cpu_readb(gb, cpu->pc);

     if (cpu->halt_bug)
     {
          /* HALT bug: o PC nao e incrementado nesta busca */
          cpu->halt_bug = false;
     }
     else
     {
          cpu->pc = (cpu->pc + 1) & 0xffff;
     }

     return i8;
}

/*
 * gb_cpu_next_i16 - Lê o próximo word imediato (16 bits) do fluxo de instruções.
 *
 * O Game Boy usa ordem little-endian: o byte menos significativo vem primeiro.
 * Lê dois bytes consecutivos via gb_cpu_next_i8 e os combina em um uint16_t.
 */
static uint16_t gb_cpu_next_i16(struct gb *gb)
{
     uint16_t b0 = gb_cpu_next_i8(gb); /* byte baixo (LSB) */
     uint16_t b1 = gb_cpu_next_i8(gb); /* byte alto (MSB) */

     return b0 | (b1 << 8);
}

/*********************
 * INSTRUÇÕES *
 ********************/

/* Tipo de ponteiro de função para todas as implementações de instrução.
 * Cada instrução recebe apenas o estado global do Game Boy e não retorna valor. */
typedef void (*gb_instruction_f)(struct gb *);

/*
 * gb_i_unimplemented - Tratador padrão para instruções ainda não implementadas.
 *
 * Lê o opcode que causou a invocação (PC já foi incrementado, então recua 1),
 * imprime uma mensagem de erro com o opcode e o endereço, e encerra a execução.
 * Funciona como um marcador de posição seguro durante o desenvolvimento.
 */
/* static void gb_i_unimplemented(struct gb *gb)
{
    struct gb_cpu *cpu = &gb->cpu;
    uint16_t instruction_pc = (cpu->pc - 1) & 0xffff;
    uint8_t instruction = gb_cpu_readb(gb, instruction_pc);

    fprintf(stderr,
            "Unimplemented instruction 0x%02x at 0x%04x\n",
            instruction, instruction_pc);
    die();
} */

/*********************
 * SEM ESPECIFICAÇÃO *
 ********************/

/*
 * gb_i_nop - Instrução NOP (opcode 0x00): nenhuma operação.
 *
 * Consome 4 ciclos de clock sem fazer nada. Usada para timing e padding.
 */
static void gb_i_nop(struct gb *gb)
{
}

/*
 * gb_i_undefined - Tratador para opcodes inválidos do LR35902.
 *
 * Os opcodes 0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC e 0xFD
 * não existem no conjunto de instruções do LR35902. No hardware real, executá-los
 * aparentemente trava a CPU indefinidamente. Aqui encerramos a emulação com erro.
 */
static void gb_i_undefined(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t instruction_pc = (cpu->pc - 1) & 0xffff;
     uint8_t instruction = gb_memory_readb(gb, instruction_pc);

     fprintf(stderr, "Undefined instruction 0x%02x at 0x%04x\n", instruction, instruction_pc);
     fprintf(stderr, "--- last %d instructions (oldest first) ---\n", GB_CPU_TRACE_SIZE);
     for (int i = 0; i < GB_CPU_TRACE_SIZE; i++)
     {
          unsigned slot = (cpu->trace_head + i) % GB_CPU_TRACE_SIZE;
          uint16_t addr = cpu->trace_buf[slot];
          char buf[64];
          gb_disasm(gb, addr, buf, sizeof(buf));
          fprintf(stderr, "  %04X  %s\n", addr, buf);
     }
     fprintf(stderr, "  %04X  *** 0x%02x (undefined) ***\n", instruction_pc, instruction);
     fprintf(stderr, "  PC=%04X SP=%04X AF=%02X%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X\n",
             cpu->pc, cpu->sp,
             cpu->a, (cpu->f_z<<7)|(cpu->f_n<<6)|(cpu->f_h<<5)|(cpu->f_c<<4),
             cpu->b, cpu->c, cpu->d, cpu->e, cpu->h, cpu->l);
     die();
}

/*
 * gb_i_di - Instrução DI (opcode 0xF3): desabilita interrupções.
 *
 * Limpa o registrador IME (Interrupt Master Enable), impedindo
 * que qualquer interrupção seja atendida até que EI seja executado.
 * O efeito é imediato — diferente de EI, que só entra em vigor após
 * a instrução seguinte.
 */
static void gb_i_di(struct gb *gb)
{
     gb->cpu.irq_enable = false;
     gb->cpu.irq_enable_next = false;
     gb->cpu.irq_enable_delay = 0;
}

/*
 * gb_i_ei - Instrução EI (opcode 0xFB): habilita interrupções com atraso de 1 instrução.
 *
 * Diferente de DI, o efeito de EI não é imediato: o IME só é setado após a
 * instrução seguinte ser executada. Isso garante que um RET executado logo após
 * EI não seja interrompido antes de retornar ao chamador.
 */
static void gb_i_ei(struct gb *gb)
{
     gb->cpu.irq_enable_next = true;
     /* Only start the delay if not already counting down from a previous EI.
      * EI-EI: the second EI must not reset the delay or interrupts would never
      * fire (the delay would keep resetting to 2 on every consecutive EI). */
     if (!gb->cpu.irq_enable_delay)
     {
          gb->cpu.irq_enable_delay = 2;
     }
}
/*
 * gb_i_stop - Instrução STOP (opcode 0x10): para a CPU e o LCD até o próximo input.
 *
 * No Game Boy Color, tambem executa a troca de velocidade quando KEY1 deixa
 * uma alternancia pendente. Sem troca pendente, suspende a CPU ate uma
 * interrupcao de entrada.
 */
static void gb_i_stop(struct gb *gb)
{
     if (gb->speed_switch_pending)
     {
          /* Se uma troca de velocidade foi solicitada, ela e executada no STOP
           * e a execucao continua normalmente depois disso. */

          /* A velocidade do clock vai mudar; sincronize os dispositivos
           * relevantes com a velocidade atual. */
          gb_timer_sync(gb);
          gb_dma_sync(gb);
          gb->timer.divider_counter = 0;

          gb->double_speed = !gb->double_speed;
          gb->speed_switch_pending = false;

          /* O hardware leva 2050 M-cycles durante a transição de velocidade.
           * Adicionamos o delay já na nova escala de timestamp para que todos
           * os dispositivos avancem corretamente ao fazer a próxima sync. */
          gb->timestamp += 2050;

          /* Ressincroniza com a nova previsao. */
          gb_timer_sync(gb);
          gb_dma_sync(gb);

          return;
     }

     if (gb->gbc)
     {
          gb_gpu_sync(gb);

          fprintf(stderr, "stop ts=%d mode=%u ly=%u pos=%u lcdc=%02x gbc=%u\n",
                  gb->timestamp, gb_gpu_get_mode(gb), gb->gpu.ly,
                  gb->gpu.line_pos, gb_gpu_get_lcdc(gb), gb->gbc);

          if (gb_gpu_get_mode(gb) != 3)
          {
               union gb_gpu_color line[GB_LCD_WIDTH];
               for (unsigned x = 0; x < GB_LCD_WIDTH; x++)
                    line[x].gbc_color = 0;
               for (unsigned y = 0; y < GB_LCD_HEIGHT; y++)
                    gb->frontend.draw_line_gbc(gb, y, line);
               gb->gpu.master_enable = false;
          }
     }
     else
     {
          gb_gpu_set_lcdc(gb, gb_gpu_get_lcdc(gb) & (uint8_t)~0x80);
     }

     /* Suspende a CPU ate um botao ser pressionado (gera GB_IRQ_INPUT). */
     gb->cpu.stopped = true;
}

/*
 * gb_i_halt - Instrução HALT (opcode 0x76): suspende a CPU até a próxima interrupção.
 *
 * Com IME=1: suspende a CPU; ela retoma após a próxima interrupção ser atendida.
 * Com IME=0 e IRQ pendente: ativa o HALT bug — a CPU não para, mas o próximo
 *   fetch de opcode não avança o PC (o byte é lido duas vezes).
 * Com IME=0 e sem IRQ pendente: suspende normalmente; a CPU retoma quando uma
 *   interrupção ocorre, mas ela não é atendida (IME=0).
 */
static void gb_i_halt(struct gb *gb)
{
     if (!gb->cpu.irq_enable && (gb->irq.irq_enable & gb->irq.irq_flags & 0x1f))
     {
          /* HALT bug: IME=0 com interrupção pendente — próximo fetch não avança PC */
          gb->cpu.halt_bug = true;
     }
     else
     {
          gb->cpu.halted = true;
     }
}

/*
 * gb_i_scf - Instrução SCF (opcode 0x37): seta o flag de carry.
 *
 * Equivalente a: C = 1
 * Flags afetadas:
 *   N = 0, H = 0 (sempre resetados)
 *   C = 1 (setado incondicionalmente)
 *   Z = não alterado
 */
static void gb_i_scf(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = true;
}

/*
 * gb_i_ccf - Instrução CCF (opcode 0x3F): complementa (inverte) o flag de carry.
 *
 * Equivalente a: C = !C
 * Flags afetadas:
 *   N = 0, H = 0 (sempre resetados)
 *   C = invertido
 *   Z = não alterado
 */
static void gb_i_ccf(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = !cpu->f_c;
}

/*********************
 * ARITMÉTICA *
 ********************/

/*
 * gb_cpu_inc - Incrementa um valor de 8 bits e atualiza as flags correspondentes.
 *
 * Retorna (v + 1) mascarado para 8 bits.
 * Flags afetadas:
 *   Z = 1 se o resultado for zero (wrap-around: 0xFF + 1 = 0x00)
 *   N = 0 (operação de adição)
 *   H = 1 se o nibble baixo era 0xF (carry do bit 3 para o bit 4)
 *   C = não alterado
 *
 * Usada por todas as variantes de INC r e INC (HL).
 */
static uint8_t gb_cpu_inc(struct gb *gb, uint8_t v)
{
     struct gb_cpu *cpu = &gb->cpu;

     uint8_t r = (v + 1) & 0xff;

     cpu->f_z = (r == 0);
     cpu->f_n = false;
     /* Teremos half-carry se o nibble baixo for 0xf */
     cpu->f_h = ((v & 0xf) == 0xf);
     /* cpu->f_c Carry não é modificado por esta instrução */

     return r;
}

/*
 * gb_cpu_dec - Decrementa um valor de 8 bits e atualiza as flags correspondentes.
 *
 * Retorna (v - 1) mascarado para 8 bits.
 * Flags afetadas:
 *   Z = 1 se o resultado for zero (0x01 - 1 = 0x00)
 *   N = 1 (operação de subtração)
 *   H = 1 se o nibble baixo era 0x0 (borrow do bit 4 para o bit 3)
 *   C = não alterado
 *
 * Usada por todas as variantes de DEC r e DEC (HL).
 */
static uint8_t gb_cpu_dec(struct gb *gb, uint8_t v)
{
     struct gb_cpu *cpu = &gb->cpu;

     uint8_t r = (v - 1) & 0xff;

     cpu->f_z = (r == 0);
     cpu->f_n = true;
     /* Teremos half-carry se o nibble baixo for 0 */
     cpu->f_h = ((v & 0xf) == 0);
     /* cpu->f_c Carry não é modificado por esta instrução */

     return r;
}

/* --- INC r : incrementa registrador de 8 bits (flags Z, N=0, H atualizadas; C inalterado) --- */
static void gb_i_inc_a(struct gb *gb)
{
     gb->cpu.a = gb_cpu_inc(gb, gb->cpu.a);
}

static void gb_i_inc_b(struct gb *gb)
{
     gb->cpu.b = gb_cpu_inc(gb, gb->cpu.b);
}

static void gb_i_inc_c(struct gb *gb)
{
     gb->cpu.c = gb_cpu_inc(gb, gb->cpu.c);
}

static void gb_i_inc_d(struct gb *gb)
{
     gb->cpu.d = gb_cpu_inc(gb, gb->cpu.d);
}

static void gb_i_inc_e(struct gb *gb)
{
     gb->cpu.e = gb_cpu_inc(gb, gb->cpu.e);
}

static void gb_i_inc_h(struct gb *gb)
{
     gb->cpu.h = gb_cpu_inc(gb, gb->cpu.h);
}

static void gb_i_inc_l(struct gb *gb)
{
     gb->cpu.l = gb_cpu_inc(gb, gb->cpu.l);
}

/* gb_i_inc_mhl - Instrução INC (HL) (opcode 0x34): incrementa o byte na memória apontada por HL.
 * Equivalente a: mem[HL] = mem[HL] + 1 */
static void gb_i_inc_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v = gb_cpu_readb(gb, hl);

     v = gb_cpu_inc(gb, v);

     gb_cpu_writeb(gb, hl, v);
}

/* --- DEC r : decrementa registrador de 8 bits (flags Z, N=1, H atualizadas; C inalterado) --- */
static void gb_i_dec_a(struct gb *gb)
{
     gb->cpu.a = gb_cpu_dec(gb, gb->cpu.a);
}

static void gb_i_dec_b(struct gb *gb)
{
     gb->cpu.b = gb_cpu_dec(gb, gb->cpu.b);
}

static void gb_i_dec_c(struct gb *gb)
{
     gb->cpu.c = gb_cpu_dec(gb, gb->cpu.c);
}

static void gb_i_dec_d(struct gb *gb)
{
     gb->cpu.d = gb_cpu_dec(gb, gb->cpu.d);
}

static void gb_i_dec_e(struct gb *gb)
{
     gb->cpu.e = gb_cpu_dec(gb, gb->cpu.e);
}

static void gb_i_dec_h(struct gb *gb)
{
     gb->cpu.h = gb_cpu_dec(gb, gb->cpu.h);
}

static void gb_i_dec_l(struct gb *gb)
{
     gb->cpu.l = gb_cpu_dec(gb, gb->cpu.l);
}

/* gb_i_dec_mhl - Instrução DEC (HL) (opcode 0x35): decrementa o byte na memória apontada por HL.
 * Equivalente a: mem[HL] = mem[HL] - 1 */
static void gb_i_dec_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v = gb_cpu_readb(gb, hl);

     v = gb_cpu_dec(gb, v);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_addw_set_flags - Soma dois valores de 16 bits e atualiza as flags correspondentes.
 *
 * Realiza a operação em 32 bits para detectar o carry além do bit 15.
 * Flags afetadas:
 *   Z = não alterado (preserva o valor anterior)
 *   N = 0 (operação de adição)
 *   H = carry do bit 11 para o bit 12 (nibble alto do byte baixo)
 *   C = carry do bit 15 para o bit 16 (overflow de 16 bits)
 *
 * Usada pelas instruções ADD HL, rr para atualizar as flags corretamente.
 * Retorna o resultado truncado para 16 bits.
 */
static uint16_t gb_cpu_addw_set_flags(struct gb *gb, uint16_t a, uint16_t b)
{
     struct gb_cpu *cpu = &gb->cpu;

     uint32_t wa = a;
     uint32_t wb = b;

     uint32_t r = a + b;

     cpu->f_n = false;
     cpu->f_c = r & 0x10000;
     cpu->f_h = (wa ^ wb ^ r) & 0x1000;
     /* cpu->f_z is not altered */

     gb_cpu_clock_tick(gb, 4);

     return r;
}

/*
 * gb_cpu_sub_set_flags - Subtrai dois valores de 8 bits e atualiza as flags correspondentes.
 *
 * Realiza a operação em 16 bits para detectar borrow além do bit 7.
 * Flags afetadas:
 *   Z = 1 se o resultado (byte baixo) for zero
 *   N = 1 (operação de subtração)
 *   H = borrow do bit 3 para o bit 4 (half-borrow, bit 4 da XOR dos operandos e resultado)
 *   C = borrow do bit 7 para o bit 8 (overflow de subtração de 8 bits, bit 8 do resultado)
 *
 * Usada por SUB, SBC e CP para atualizar as flags corretamente.
 * Retorna o resultado de 16 bits (o byte baixo contém o resultado de 8 bits).
 */
static uint16_t gb_cpu_sub_set_flags(struct gb *gb, uint8_t a, uint16_t b)
{
     struct gb_cpu *cpu = &gb->cpu;

     /* Check for carry using 16bit arithmetic */
     uint16_t al = a;
     uint16_t bl = b;

     uint16_t r = al - bl;

     cpu->f_z = !(r & 0xff);
     cpu->f_n = true;
     cpu->f_h = (a ^ b ^ r) & 0x10;
     cpu->f_c = r & 0x100;

     return r;
}

/*
 * gb_i_sub_a_a - Instrução SUB A (opcode 0x97): subtrai A de si mesmo.
 *
 * Equivalente a A = A - A, portanto A sempre resulta em 0.
 * Flags afetadas:
 *   Z = 1 (resultado sempre zero)
 *   N = 1 (operação de subtração)
 *   H = 0 (sem half-borrow: 0x0 - 0x0 = 0)
 *   C = 0 (sem borrow: A - A nunca resulta em negativo)
 *
 * Uso comum: zerar o acumulador de forma compacta (1 byte vs 2 bytes do LD A, 0).
 */
static void gb_i_sub_a_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sub_set_flags(gb, cpu->a, cpu->a);
}

/* --- SUB r : subtrai registrador de A (flags Z, N=1, H, C atualizados) --- */
static void gb_i_sub_a_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sub_set_flags(gb, cpu->a, cpu->b);
}

static void gb_i_sub_a_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sub_set_flags(gb, cpu->a, cpu->c);
}

static void gb_i_sub_a_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sub_set_flags(gb, cpu->a, cpu->d);
}

static void gb_i_sub_a_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sub_set_flags(gb, cpu->a, cpu->e);
}

static void gb_i_sub_a_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sub_set_flags(gb, cpu->a, cpu->h);
}

static void gb_i_sub_a_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sub_set_flags(gb, cpu->a, cpu->l);
}

static void gb_i_sub_a_mhl(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);
     cpu->a = gb_cpu_sub_set_flags(gb, cpu->a, v);
}

static void gb_i_sub_a_i8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t i8 = gb_cpu_next_i8(gb);

     cpu->a = gb_cpu_sub_set_flags(gb, cpu->a, i8);
}

/*
 * gb_cpu_sbc_set_flags - Subtrai dois valores de 8 bits com borrow e atualiza as flags.
 *
 * Equivalente a: r = a - b - C  (onde C é o flag de carry/borrow anterior)
 * Flags afetadas:
 *   Z = 1 se o resultado (byte baixo) for zero
 *   N = 1 (operação de subtração)
 *   H = borrow do nibble baixo (bit 4 da XOR dos operandos e resultado)
 *   C = borrow do nibble alto (bit 8 do resultado)
 *
 * Usada pelas variantes de SBC A, r. Retorna o resultado de 8 bits.
 */
static uint8_t gb_cpu_sbc_set_flags(struct gb *gb, uint8_t a, uint16_t b)
{
     struct gb_cpu *cpu = &gb->cpu;

     /* Aritmética em 16 bits para detectar o borrow além do bit 7 */
     uint16_t al = a;
     uint16_t bl = b;
     uint16_t c = cpu->f_c;

     uint16_t r = al - bl - c;

     cpu->f_z = !(r & 0xff);
     cpu->f_n = true;
     cpu->f_h = (a ^ b ^ r) & 0x10;
     cpu->f_c = r & 0x100;

     return r;
}

/* --- SBC A, r : subtrai registrador e borrow de A (flags Z, N=1, H, C atualizados) --- */
static void gb_i_sbc_a_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sbc_set_flags(gb, cpu->a, cpu->a);
}

static void gb_i_sbc_a_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sbc_set_flags(gb, cpu->a, cpu->b);
}

static void gb_i_sbc_a_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sbc_set_flags(gb, cpu->a, cpu->c);
}

static void gb_i_sbc_a_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sbc_set_flags(gb, cpu->a, cpu->d);
}

static void gb_i_sbc_a_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sbc_set_flags(gb, cpu->a, cpu->e);
}

static void gb_i_sbc_a_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sbc_set_flags(gb, cpu->a, cpu->h);
}

static void gb_i_sbc_a_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_sbc_set_flags(gb, cpu->a, cpu->l);
}

static void gb_i_sbc_a_mhl(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);
     cpu->a = gb_cpu_sbc_set_flags(gb, cpu->a, v);
}

static void gb_i_sbc_a_i8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t i8 = gb_cpu_next_i8(gb);

     cpu->a = gb_cpu_sbc_set_flags(gb, cpu->a, i8);
}

/*
 * gb_cpu_add_set_flags - Soma dois valores de 8 bits e atualiza as flags correspondentes.
 *
 * Realiza a operação em 16 bits para detectar o carry além do bit 7.
 * Flags afetadas:
 *   Z = 1 se o resultado (byte baixo) for zero
 *   N = 0 (operação de adição)
 *   H = carry do bit 3 para o bit 4 (half-carry, bit 4 da XOR dos operandos e resultado)
 *   C = carry do bit 7 para o bit 8 (overflow de 8 bits, bit 8 do resultado)
 *
 * Usada por todas as variantes de ADD A, r para atualizar as flags corretamente.
 * Retorna o resultado truncado para 8 bits.
 */
static uint8_t gb_cpu_add_set_flags(struct gb *gb, uint8_t a, uint8_t b)
{
     struct gb_cpu *cpu = &gb->cpu;

     /* Check for carry using 16bit arithmetic */
     uint16_t al = a;
     uint16_t bl = b;

     uint16_t r = al + bl;

     cpu->f_z = !(r & 0xff);
     cpu->f_n = false;
     cpu->f_h = (a ^ b ^ r) & 0x10;
     cpu->f_c = r & 0x100;

     return r;
}

/*
 * gb_i_add_a_a - Instrução ADD A, A (opcode 0x87): soma A com ele mesmo.
 *
 * Equivalente a: A = A + A = A * 2 (deslocamento aritmético à esquerda de 8 bits).
 * Flags: Z, N=0, H, C atualizados via gb_cpu_add_set_flags.
 */
static void gb_i_add_a_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_add_set_flags(gb, cpu->a, cpu->a);
}

/*
 * gb_i_add_a_b - Instrução ADD A, B (opcode 0x80): soma B ao acumulador.
 * Equivalente a: A = A + B. Flags: Z, N=0, H, C atualizados.
 */
static void gb_i_add_a_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_add_set_flags(gb, cpu->a, cpu->b);
}

/*
 * gb_i_add_a_c - Instrução ADD A, C (opcode 0x81): soma C ao acumulador.
 * Equivalente a: A = A + C. Flags: Z, N=0, H, C atualizados.
 */
static void gb_i_add_a_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_add_set_flags(gb, cpu->a, cpu->c);
}

/*
 * gb_i_add_a_d - Instrução ADD A, D (opcode 0x82): soma D ao acumulador.
 * Equivalente a: A = A + D. Flags: Z, N=0, H, C atualizados.
 */
static void gb_i_add_a_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_add_set_flags(gb, cpu->a, cpu->d);
}

/*
 * gb_i_add_a_e - Instrução ADD A, E (opcode 0x83): soma E ao acumulador.
 * Equivalente a: A = A + E. Flags: Z, N=0, H, C atualizados.
 */
static void gb_i_add_a_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_add_set_flags(gb, cpu->a, cpu->e);
}

/*
 * gb_i_add_a_h - Instrução ADD A, H (opcode 0x84): soma H ao acumulador.
 * Equivalente a: A = A + H. Flags: Z, N=0, H, C atualizados.
 */
static void gb_i_add_a_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_add_set_flags(gb, cpu->a, cpu->h);
}

/*
 * gb_i_add_a_l - Instrução ADD A, L (opcode 0x85): soma L ao acumulador.
 * Equivalente a: A = A + L. Flags: Z, N=0, H, C atualizados.
 */
static void gb_i_add_a_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_add_set_flags(gb, cpu->a, cpu->l);
}

/*
 * gb_i_add_a_mhl - Instrução ADD A, (HL) (opcode 0x86): soma o byte apontado por HL a A.
 * Equivalente a: A = A + mem[HL]. Flags: Z, N=0, H, C atualizados.
 */
static void gb_i_add_a_mhl(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);
     cpu->a = gb_cpu_add_set_flags(gb, cpu->a, v);
}

/*
 * gb_i_add_a_i8 - Instrução ADD A, n (opcode 0xC6): soma imediato de 8 bits a A.
 * Equivalente a: A = A + n. Flags: Z, N=0, H, C atualizados.
 */
static void gb_i_add_a_i8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t i8 = gb_cpu_next_i8(gb);

     cpu->a = gb_cpu_add_set_flags(gb, cpu->a, i8);
}

/*
 * gb_cpu_adc_set_flags - Soma dois valores de 8 bits com carry e atualiza as flags.
 *
 * Equivalente a: r = a + b + C  (onde C é o flag de carry anterior)
 * Flags afetadas:
 *   Z = 1 se o resultado (byte baixo) for zero
 *   N = 0 (operação de adição)
 *   H = carry do nibble baixo (bit 4 da XOR dos operandos e resultado)
 *   C = carry do nibble alto (bit 8 do resultado)
 *
 * Usada pelas variantes de ADC A, r. Retorna o resultado de 8 bits.
 */
static uint8_t gb_cpu_adc_set_flags(struct gb *gb, uint8_t a, uint8_t b)
{
     struct gb_cpu *cpu = &gb->cpu;

     /* Aritmética em 16 bits para detectar o carry além do bit 7 */
     uint16_t al = a;
     uint16_t bl = b;
     uint16_t c = cpu->f_c;

     uint16_t r = al + bl + c;

     cpu->f_z = !(r & 0xff);
     cpu->f_n = false;
     cpu->f_h = (a ^ b ^ r) & 0x10;
     cpu->f_c = r & 0x100;

     return r;
}

/* --- ADC A, r : soma registrador e carry a A (flags Z, N=0, H, C atualizados) --- */
static void gb_i_adc_a_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_adc_set_flags(gb, cpu->a, cpu->a);
}

static void gb_i_adc_a_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_adc_set_flags(gb, cpu->a, cpu->b);
}

static void gb_i_adc_a_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_adc_set_flags(gb, cpu->a, cpu->c);
}

static void gb_i_adc_a_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_adc_set_flags(gb, cpu->a, cpu->d);
}

static void gb_i_adc_a_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_adc_set_flags(gb, cpu->a, cpu->e);
}

static void gb_i_adc_a_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_adc_set_flags(gb, cpu->a, cpu->h);
}

static void gb_i_adc_a_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_adc_set_flags(gb, cpu->a, cpu->l);
}

static void gb_i_adc_a_mhl(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);
     cpu->a = gb_cpu_adc_set_flags(gb, cpu->a, v);
}

static void gb_i_adc_a_i8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t i8 = gb_cpu_next_i8(gb);

     cpu->a = gb_cpu_adc_set_flags(gb, cpu->a, i8);
}

/*
 * gb_i_add_sp_si8 - Instrução ADD SP, e (opcode 0xE8): soma deslocamento com sinal ao SP.
 *
 * Lê um operando imediato de 8 bits com sinal (-128 a +127) e o soma ao SP.
 * Usada para ajustar o ponteiro de pilha em pequenos deslocamentos, como ao
 * acessar variáveis locais relativas ao frame atual.
 *
 * Flags afetadas:
 *   Z = 0  (sempre resetado, mesmo se SP resultar em 0)
 *   N = 0  (sempre resetado)
 *   H = carry do bit 3 para o bit 4 (nibble baixo do SP + nibble baixo do offset)
 *   C = carry do bit 7 para o bit 8 (byte baixo do SP + byte do offset)
 *
 * Nota: H e C são calculados sobre o byte baixo (operação de 8 bits),
 * não sobre o resultado de 16 bits — comportamento peculiar do LR35902.
 *
 */
static uint16_t gb_add_sp_si8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     /* Offset is signed */
     int8_t i8 = gb_cpu_next_i8(gb);

     /* Use 32bit arithmetic to avoid signed integer overflow UB */
     int32_t r = cpu->sp;
     r += i8;

     cpu->f_z = false;
     cpu->f_n = false;

     /* Carry and Half-carry are for the low byte */
     cpu->f_h = (cpu->sp ^ i8 ^ r) & 0x10;
     cpu->f_c = (cpu->sp ^ i8 ^ r) & 0x100;

     return (uint16_t)r;
}

static void gb_i_add_sp_si8(struct gb *gb)
{
     gb->cpu.sp = gb_add_sp_si8(gb);

     gb_cpu_clock_tick(gb, 8);
}

/*
 * gb_i_add_hl_bc - Instrução ADD HL, BC (opcode 0x09): soma BC ao par HL.
 *
 * Equivalente a: HL = HL + BC
 * Flags: N=0, H e C atualizados; Z não alterado. Ver gb_cpu_addw_set_flags.
 */
static void gb_i_add_hl_bc(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t bc = gb_cpu_bc(gb);

     hl = gb_cpu_addw_set_flags(gb, hl, bc);

     gb_cpu_set_hl(gb, hl);
}

/*
 * gb_i_add_hl_de - Instrução ADD HL, DE (opcode 0x19): soma DE ao par HL.
 *
 * Equivalente a: HL = HL + DE
 * Flags: N=0, H e C atualizados; Z não alterado. Ver gb_cpu_addw_set_flags.
 */
static void gb_i_add_hl_de(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t de = gb_cpu_de(gb);

     hl = gb_cpu_addw_set_flags(gb, hl, de);

     gb_cpu_set_hl(gb, hl);
}

/*
 * gb_i_add_hl_hl - Instrução ADD HL, HL (opcode 0x29): dobra o valor de HL.
 *
 * Equivalente a: HL = HL + HL = HL * 2 (deslocamento aritmético à esquerda de 16 bits).
 * Flags: N=0, H e C atualizados; Z não alterado. Ver gb_cpu_addw_set_flags.
 */
static void gb_i_add_hl_hl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);

     hl = gb_cpu_addw_set_flags(gb, hl, hl);

     gb_cpu_set_hl(gb, hl);
}

/*
 * gb_i_add_hl_sp - Instrução ADD HL, SP (opcode 0x39): soma SP ao par HL.
 *
 * Equivalente a: HL = HL + SP
 * Flags: N=0, H e C atualizados; Z não alterado. Ver gb_cpu_addw_set_flags.
 */
static void gb_i_add_hl_sp(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);

     hl = gb_cpu_addw_set_flags(gb, hl, gb->cpu.sp);

     gb_cpu_set_hl(gb, hl);
}

/*
 * gb_i_inc_sp - Instrução INC SP (opcode 0x33): incrementa o Stack Pointer.
 *
 * Equivalente a: SP = (SP + 1) & 0xFFFF
 * Nenhuma flag é afetada (comportamento do LR35902 para INC de 16 bits).
 */
static void gb_i_inc_sp(struct gb *gb)
{
     uint16_t sp = gb->cpu.sp;

     gb_memory_trigger_oam_bug(gb, sp);
     sp = (sp + 1) & 0xffff;

     gb->cpu.sp = sp;

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_i_inc_bc - Instrução INC BC (opcode 0x03): incrementa o par de registradores BC.
 *
 * Equivalente a: BC = (BC + 1) & 0xFFFF
 * Nenhuma flag é afetada (comportamento do LR35902 para INC de 16 bits).
 */
static void gb_i_inc_bc(struct gb *gb)
{
     uint16_t bc = gb_cpu_bc(gb);

     gb_memory_trigger_oam_bug(gb, bc);
     bc = (bc + 1) & 0xffff;

     gb_cpu_set_bc(gb, bc);

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_i_inc_de - Instrução INC DE (opcode 0x13): incrementa o par de registradores DE.
 *
 * Equivalente a: DE = (DE + 1) & 0xFFFF
 * Nenhuma flag é afetada (comportamento do LR35902 para INC de 16 bits).
 */
static void gb_i_inc_de(struct gb *gb)
{
     uint16_t de = gb_cpu_de(gb);

     gb_memory_trigger_oam_bug(gb, de);
     de = (de + 1) & 0xffff;

     gb_cpu_set_de(gb, de);

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_i_inc_hl - Instrução INC HL (opcode 0x23): incrementa o par de registradores HL.
 *
 * Equivalente a: HL = (HL + 1) & 0xFFFF
 * Nenhuma flag é afetada (comportamento do LR35902 para INC de 16 bits).
 * Nota: diferente de INC (HL) (opcode 0x34), que incrementa o byte na memória
 * apontada por HL e afeta as flags Z, N e H.
 */
static void gb_i_inc_hl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);

     gb_memory_trigger_oam_bug(gb, hl);
     hl = (hl + 1) & 0xffff;

     gb_cpu_set_hl(gb, hl);

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_i_dec_sp - Instrução DEC SP (opcode 0x3B): decrementa o Stack Pointer.
 *
 * Equivalente a: SP = (SP - 1) & 0xFFFF
 * Nenhuma flag é afetada (comportamento do LR35902 para DEC de 16 bits).
 */
static void gb_i_dec_sp(struct gb *gb)
{
     uint16_t sp = gb->cpu.sp;

     gb_memory_trigger_oam_bug(gb, sp);
     sp = (sp - 1) & 0xffff;

     gb->cpu.sp = sp;

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_i_dec_bc - Instrução DEC BC (opcode 0x0B): decrementa o par de registradores BC.
 *
 * Equivalente a: BC = (BC - 1) & 0xFFFF
 * Nenhuma flag é afetada (comportamento do LR35902 para DEC de 16 bits).
 */
static void gb_i_dec_bc(struct gb *gb)
{
     uint16_t bc = gb_cpu_bc(gb);

     gb_memory_trigger_oam_bug(gb, bc);
     bc = (bc - 1) & 0xffff;

     gb_cpu_set_bc(gb, bc);

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_i_dec_de - Instrução DEC DE (opcode 0x1B): decrementa o par de registradores DE.
 *
 * Equivalente a: DE = (DE - 1) & 0xFFFF
 * Nenhuma flag é afetada (comportamento do LR35902 para DEC de 16 bits).
 */
static void gb_i_dec_de(struct gb *gb)
{
     uint16_t de = gb_cpu_de(gb);

     gb_memory_trigger_oam_bug(gb, de);
     de = (de - 1) & 0xffff;

     gb_cpu_set_de(gb, de);

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_i_dec_hl - Instrução DEC HL (opcode 0x2B): decrementa o par de registradores HL.
 *
 * Equivalente a: HL = (HL - 1) & 0xFFFF
 * Nenhuma flag é afetada (comportamento do LR35902 para DEC de 16 bits).
 * Nota: diferente de DEC (HL) (opcode 0x35), que decrementa o byte na memória
 * apontada por HL e afeta as flags Z, N e H.
 */
static void gb_i_dec_hl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);

     gb_memory_trigger_oam_bug(gb, hl);
     hl = (hl - 1) & 0xffff;

     gb_cpu_set_hl(gb, hl);

     gb_cpu_clock_tick(gb, 4);
}

/* --- CP A, r : compara A com registrador (A-r), atualiza só as flags, não altera A --- */
static void gb_i_cp_a_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sub_set_flags(gb, cpu->a, cpu->a);
}

static void gb_i_cp_a_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sub_set_flags(gb, cpu->a, cpu->b);
}

static void gb_i_cp_a_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sub_set_flags(gb, cpu->a, cpu->c);
}

static void gb_i_cp_a_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sub_set_flags(gb, cpu->a, cpu->d);
}

static void gb_i_cp_a_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sub_set_flags(gb, cpu->a, cpu->e);
}

static void gb_i_cp_a_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sub_set_flags(gb, cpu->a, cpu->h);
}

static void gb_i_cp_a_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sub_set_flags(gb, cpu->a, cpu->l);
}

static void gb_i_cp_a_mhl(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);
     gb_cpu_sub_set_flags(gb, cpu->a, v);
}

/*
 * gb_i_cp_a_i8 - Instrução CP n (opcode 0xFE): compara A com imediato de 8 bits.
 *
 * Realiza A - n mas descarta o resultado, atualizando apenas as flags.
 * Equivalente a SUB n sem modificar A. Usado em desvios condicionais:
 *   após CP n, JR Z salta se A == n; JR C salta se A < n (unsigned).
 *
 * Flags afetadas:
 *   Z = 1 se A == n
 *   N = 1 (operação de subtração)
 *   H = borrow do nibble baixo
 *   C = 1 se A < n (borrow)
 */
static void gb_i_cp_a_i8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t i8 = gb_cpu_next_i8(gb);

     gb_cpu_sub_set_flags(gb, cpu->a, i8);
}

/*
 * gb_cpu_and_set_flags - AND bit a bit de dois valores de 8 bits e atualiza as flags.
 *
 * Flags afetadas:
 *   Z = 1 se o resultado for zero
 *   N = 0 (não é subtração)
 *   H = 1 (sempre, peculiaridade do LR35902)
 *   C = 0 (sempre resetado)
 *
 * Usada pelas variantes de AND A, r e AND A, n.
 * Retorna o resultado da operação AND.
 */
static uint8_t gb_cpu_and_set_flags(struct gb *gb, uint8_t a, uint8_t b)
{
     struct gb_cpu *cpu = &gb->cpu;

     uint8_t r = a & b;

     cpu->f_z = (r == 0);
     cpu->f_n = false;
     cpu->f_h = true;
     cpu->f_c = false;

     return r;
}

/* --- AND A, r : AND bit a bit de A com registrador (flags Z, N=0, H=1, C=0) --- */
static void gb_i_and_a_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_and_set_flags(gb, cpu->a, cpu->a);
}

static void gb_i_and_a_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_and_set_flags(gb, cpu->a, cpu->b);
}

static void gb_i_and_a_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_and_set_flags(gb, cpu->a, cpu->c);
}

static void gb_i_and_a_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_and_set_flags(gb, cpu->a, cpu->d);
}

static void gb_i_and_a_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_and_set_flags(gb, cpu->a, cpu->e);
}

static void gb_i_and_a_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_and_set_flags(gb, cpu->a, cpu->h);
}

static void gb_i_and_a_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_and_set_flags(gb, cpu->a, cpu->l);
}

static void gb_i_and_a_mhl(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);
     cpu->a = gb_cpu_and_set_flags(gb, cpu->a, v);
}

/*
 * gb_i_and_a_i8 - Instrução AND n (opcode 0xE6): AND de A com imediato de 8 bits.
 *
 * Equivalente a: A = A & n. Flags: Z, N=0, H=1, C=0 atualizados.
 */
static void gb_i_and_a_i8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t i8 = gb_cpu_next_i8(gb);

     cpu->a = gb_cpu_and_set_flags(gb, cpu->a, i8);
}

/*
 * gb_cpu_xor_set_flags - XOR bit a bit de dois valores de 8 bits e atualiza as flags.
 *
 * Flags afetadas:
 *   Z = 1 se o resultado for zero
 *   N = 0, H = 0, C = 0 (sempre resetados)
 *
 * Usada pelas variantes de XOR A, r e XOR A, n.
 * Retorna o resultado da operação XOR.
 */
static uint8_t gb_cpu_xor_set_flags(struct gb *gb, uint8_t a, uint8_t b)
{
     struct gb_cpu *cpu = &gb->cpu;

     uint8_t r = a ^ b;

     cpu->f_z = (r == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = false;

     return r;
}

/* --- XOR A, r : XOR bit a bit de A com registrador (flags Z, N=0, H=0, C=0) --- */
static void gb_i_xor_a_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_xor_set_flags(gb, cpu->a, cpu->a);
}

static void gb_i_xor_a_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_xor_set_flags(gb, cpu->a, cpu->b);
}

static void gb_i_xor_a_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_xor_set_flags(gb, cpu->a, cpu->c);
}

static void gb_i_xor_a_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_xor_set_flags(gb, cpu->a, cpu->d);
}

static void gb_i_xor_a_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_xor_set_flags(gb, cpu->a, cpu->e);
}

static void gb_i_xor_a_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_xor_set_flags(gb, cpu->a, cpu->h);
}

static void gb_i_xor_a_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_xor_set_flags(gb, cpu->a, cpu->l);
}

static void gb_i_xor_a_mhl(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);
     cpu->a = gb_cpu_xor_set_flags(gb, cpu->a, v);
}

/*
 * gb_i_xor_a_i8 - Instrução XOR n (opcode 0xEE): XOR de A com imediato de 8 bits.
 * Equivalente a: A = A ^ n. Flags: Z, N=0, H=0, C=0 atualizados.
 */
static void gb_i_xor_a_i8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t i8 = gb_cpu_next_i8(gb);

     cpu->a = gb_cpu_xor_set_flags(gb, cpu->a, i8);
}

/*
 * gb_cpu_or_set_flags - OR bit a bit de dois valores de 8 bits e atualiza as flags.
 *
 * Flags afetadas:
 *   Z = 1 se o resultado for zero
 *   N = 0, H = 0, C = 0 (sempre resetados)
 *
 * Usada pelas variantes de OR A, r.
 * Retorna o resultado da operação OR.
 */
static uint8_t gb_cpu_or_set_flags(struct gb *gb, uint8_t a, uint8_t b)
{
     struct gb_cpu *cpu = &gb->cpu;

     uint8_t r = a | b;

     cpu->f_z = (r == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = false;

     return r;
}

/* --- OR A, r : OR bit a bit de A com registrador de origem (flags Z, N=0, H=0, C=0) --- */
static void gb_i_or_a_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_or_set_flags(gb, cpu->a, cpu->a);
}

static void gb_i_or_a_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_or_set_flags(gb, cpu->a, cpu->b);
}

static void gb_i_or_a_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_or_set_flags(gb, cpu->a, cpu->c);
}

static void gb_i_or_a_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_or_set_flags(gb, cpu->a, cpu->d);
}

static void gb_i_or_a_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_or_set_flags(gb, cpu->a, cpu->e);
}

static void gb_i_or_a_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_or_set_flags(gb, cpu->a, cpu->h);
}

static void gb_i_or_a_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     cpu->a = gb_cpu_or_set_flags(gb, cpu->a, cpu->l);
}

/*
 * gb_i_or_a_mhl - Instrução OR (HL) (opcode 0xB6): OR de A com o byte apontado por HL.
 * Equivalente a: A = A | mem[HL]. Flags: Z, N=0, H=0, C=0 atualizados.
 */
static void gb_i_or_a_mhl(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);
     cpu->a = gb_cpu_or_set_flags(gb, cpu->a, v);
}

/*
 * gb_i_or_a_i8 - Instrução OR n (opcode 0xF6): OR de A com imediato de 8 bits.
 * Equivalente a: A = A | n. Flags: Z, N=0, H=0, C=0 atualizados.
 */
static void gb_i_or_a_i8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t i8 = gb_cpu_next_i8(gb);

     cpu->a = gb_cpu_or_set_flags(gb, cpu->a, i8);
}

/*
 * gb_i_cpl_a - Instrução CPL (opcode 0x2F): complemento bit a bit do acumulador.
 *
 * Inverte todos os bits de A: A = ~A
 * Flags afetadas:
 *   N = 1, H = 1 (sempre setados)
 *   Z e C = não alterados
 */
static void gb_i_cpl_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     /* Complement A */
     cpu->a = ~cpu->a;

     cpu->f_n = true;
     cpu->f_h = true;
}

/*
 * gb_i_rlca - Instrução RLCA (opcode 0x07): rotação circular de A para a esquerda.
 *
 * O bit 7 é copiado para C e também rotacionado para o bit 0.
 * Equivalente a: C = A >> 7; A = (A << 1) | C
 * Flags: Z=0, N=0, H=0, C = bit 7 original de A
 */
static void gb_i_rlca(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t a = cpu->a;
     uint8_t c;

     c = a >> 7;
     a = (a << 1) & 0xff;
     a |= c;

     cpu->a = a;

     cpu->f_z = false;
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = c;
}

/*
 * gb_i_rla - Instrução RLA (opcode 0x17): rotação de A para a esquerda através do carry.
 *
 * O bit 7 vai para C e o valor anterior de C entra pelo bit 0.
 * Diferente de RLCA: usa C como bit de entrada em vez de recircular o bit 7.
 * Equivalente a: new_C = A >> 7; A = (A << 1) | old_C; C = new_C
 * Flags: Z=0, N=0, H=0, C = bit 7 original de A
 */
static void gb_i_rla(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t a = cpu->a;
     uint8_t c = cpu->f_c;
     uint8_t new_c;

     new_c = a >> 7;
     a = (a << 1) & 0xff;
     a |= c;

     cpu->a = a;

     cpu->f_z = false;
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = new_c;
}

/*
 * gb_i_rrca - Instrução RRCA (opcode 0x0F): rotação circular de A para a direita.
 *
 * O bit 0 é copiado para C e também rotacionado para o bit 7.
 * Equivalente a: C = A & 1; A = (A >> 1) | (C << 7)
 * Flags: Z=0, N=0, H=0, C = bit 0 original de A
 */
static void gb_i_rrca(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t a = cpu->a;
     uint8_t c;

     c = a & 1;
     a = a >> 1;
     a |= (c << 7);

     cpu->a = a;

     cpu->f_z = false;
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = c;
}

/*
 * gb_i_rra - Instrução RRA (opcode 0x1F): rotação de A para a direita através do carry.
 *
 * O bit 0 vai para C e o valor anterior de C entra pelo bit 7.
 * Diferente de RRCA: usa C como bit de entrada em vez de recircular o bit 0.
 * Equivalente a: new_C = A & 1; A = (A >> 1) | (old_C << 7); C = new_C
 * Flags: Z=0, N=0, H=0, C = bit 0 original de A
 */
static void gb_i_rra(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t a = cpu->a;
     uint8_t c = cpu->f_c;
     uint8_t new_c;

     new_c = a & 1;
     a = a >> 1;
     a |= (c << 7);

     cpu->a = a;

     cpu->f_z = false;
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = new_c;
}

/*
 * gb_i_daa - Instrução DAA (opcode 0x27): ajuste decimal do acumulador (BCD).
 *
 * Corrige o valor de A após uma operação ADD ou SUB para representar
 * o resultado correto em BCD (Binary-Coded Decimal), onde cada nibble
 * armazena um dígito decimal de 0 a 9.
 *
 * O ajuste depende dos flags N (última operação foi subtração?), H
 * (half-carry no nibble baixo?) e C (carry no nibble alto?).
 * Após adição: soma 0x06 ao nibble baixo e/ou 0x60 ao nibble alto conforme necessário.
 * Após subtração: subtrai os mesmos ajustes.
 *
 * Flags afetadas:
 *   Z = 1 se A resultar em 0 após o ajuste
 *   N = não alterado (preserva o tipo da última operação)
 *   H = 0 (sempre resetado após DAA)
 *   C = 1 se houve ajuste no nibble alto (resultado BCD ultrapassou 99)
 */
static void gb_i_daa(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t a = cpu->a;
     uint8_t adj = 0;

     /* See if we had a carry/borrow for the low nibble in the last operation */
     if (cpu->f_h)
     {
          /* Yes, we have to ajust it. */
          adj |= 0x06;
     }

     /* See if we had a carry/borrow for the high nibble in the last operation */
     if (cpu->f_c)
     {
          /* Yes, we have to ajust it. */
          adj |= 0x60;
     }

     if (cpu->f_n)
     {
          a -= adj;
     }
     else
     {
          if ((a & 0xf) > 0x09)
          {
               adj |= 0x06;
          }

          if (a > 0x99)
          {
               adj |= 0x60;
          }

          a += adj;
     }

     cpu->a = a;
     cpu->f_z = (a == 0);
     cpu->f_c = ((adj & 0x60) != 0);
     cpu->f_h = false;
}

/****************
 * CARREGAMENTO *
 ****************/

/*
 * gb_i_ld_a_i8 - Instrução LD A, n (opcode 0x3E): carrega imediato de 8 bits em A.
 *
 * Lê o byte seguinte ao opcode no fluxo de instruções e o armazena no
 * registrador acumulador A. Equivalente a: A = n
 */
static void gb_i_ld_a_i8(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);

     gb->cpu.a = i8;
}

/*
 * gb_i_ld_b_i8 - Instrução LD B, n (opcode 0x06): carrega imediato de 8 bits em B.
 * Equivalente a: B = n
 */
static void gb_i_ld_b_i8(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);

     gb->cpu.b = i8;
}

/*
 * gb_i_ld_c_i8 - Instrução LD C, n (opcode 0x0E): carrega imediato de 8 bits em C.
 * Equivalente a: C = n
 */
static void gb_i_ld_c_i8(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);

     gb->cpu.c = i8;
}

/*
 * gb_i_ld_d_i8 - Instrução LD D, n (opcode 0x16): carrega imediato de 8 bits em D.
 * Equivalente a: D = n
 */
static void gb_i_ld_d_i8(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);

     gb->cpu.d = i8;
}

/*
 * gb_i_ld_e_i8 - Instrução LD E, n (opcode 0x1E): carrega imediato de 8 bits em E.
 * Equivalente a: E = n
 */
static void gb_i_ld_e_i8(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);

     gb->cpu.e = i8;
}

/*
 * gb_i_ld_h_i8 - Instrução LD H, n (opcode 0x26): carrega imediato de 8 bits em H.
 * Equivalente a: H = n
 */
static void gb_i_ld_h_i8(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);

     gb->cpu.h = i8;
}

/*
 * gb_i_ld_l_i8 - Instrução LD L, n (opcode 0x2E): carrega imediato de 8 bits em L.
 * Equivalente a: L = n
 */
static void gb_i_ld_l_i8(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);

     gb->cpu.l = i8;
}

/*
 * gb_i_ld_mhl_i8 - Instrução LD (HL), n (opcode 0x36): armazena imediato de 8 bits em (HL).
 *
 * Lê o byte imediato do fluxo de instruções e o escreve no endereço apontado por HL.
 * Equivalente a: mem[HL] = n
 */
static void gb_i_ld_mhl_i8(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);
     uint16_t hl = gb_cpu_hl(gb);

     gb_cpu_writeb(gb, hl, i8);
}

/*
 * gb_i_ld_mi16_a - Instrução LD (nn), A (opcode 0xEA): armazena A no endereço de 16 bits.
 *
 * Lê um endereço absoluto de 16 bits do fluxo de instruções (little-endian)
 * e escreve o conteúdo do acumulador A nesse endereço de memória.
 * Equivalente a: mem[nn] = A
 *
 * Uso comum: salvar o valor de A em uma posição fixa de memória (variável global,
 * registrador de hardware, etc.).
 */
static void gb_i_ld_mi16_a(struct gb *gb)
{
     uint16_t i16 = gb_cpu_next_i16(gb);

     gb_cpu_writeb(gb, i16, gb->cpu.a);
}

/*
 * gb_i_ld_mi16_sp - Instrução LD (nn), SP (opcode 0x08): salva o Stack Pointer na memória.
 *
 * Lê um endereço absoluto de 16 bits do fluxo de instruções e escreve SP nesse endereço
 * em formato little-endian: o byte baixo em nn e o byte alto em nn+1.
 * Equivalente a: mem[nn] = SP & 0xFF; mem[nn+1] = SP >> 8
 *
 * Uso comum: preservar SP no início de uma rotina antes de manipulá-lo.
 */
static void gb_i_ld_mi16_sp(struct gb *gb)
{
     uint16_t i16 = gb_cpu_next_i16(gb);
     uint16_t sp = gb->cpu.sp;

     gb_cpu_writeb(gb, i16, sp & 0xff);
     gb_cpu_writeb(gb, i16 + 1, sp >> 8);
}

/*
 * gb_i_ld_a_mi16 - Instrução LD A, (nn) (opcode 0xFA): carrega byte de endereço absoluto em A.
 *
 * Lê um endereço de 16 bits do fluxo de instruções e carrega o byte nesse
 * endereço de memória no acumulador A.
 * Equivalente a: A = mem[nn]
 */
static void gb_i_ld_a_mi16(struct gb *gb)
{
     uint16_t i16 = gb_cpu_next_i16(gb);

     gb->cpu.a = gb_cpu_readb(gb, i16);
}

/*
 * gb_i_ldh_mi8_a - Instrução LDH (n), A (opcode 0xE0): escrita na página alta (0xFF00+n).
 *
 * Lê um deslocamento de 8 bits do fluxo de instruções e escreve A no endereço
 * 0xFF00 | n. Essa região (0xFF00–0xFF7F) contém os registradores de hardware
 * do Game Boy (joypad, timer, serial, LCD, áudio, DMA, etc.).
 * Equivalente a: mem[0xFF00 + n] = A
 *
 * LDH é mais compacto que LD (nn), A quando o destino é um registrador de I/O,
 * economizando 1 byte de codificação.
 */
static void gb_i_ldh_mi8_a(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);
     uint16_t addr = 0xff00 | i8; /* mapeia para a faixa de registradores de I/O */

     gb_cpu_writeb(gb, addr, gb->cpu.a);
}

/*
 * gb_i_ldh_a_mi8 - Instrução LDH A, (n) (opcode 0xF0): leitura de registrador de I/O para A.
 *
 * Lê um deslocamento de 8 bits do fluxo de instruções e carrega em A o byte
 * no endereço 0xFF00 | n. Simétrico de gb_i_ldh_mi8_a (escrita).
 * Equivalente a: A = mem[0xFF00 + n]
 *
 * Uso comum: ler o estado do joypad (0xFF00), registradores de timer,
 * status do LCD (0xFF41), etc.
 */
static void gb_i_ldh_a_mi8(struct gb *gb)
{
     uint8_t i8 = gb_cpu_next_i8(gb);
     uint16_t addr = 0xff00 | i8; /* mapeia para a faixa de registradores de I/O */

     gb->cpu.a = gb_cpu_readb(gb, addr);
}

/*
 * gb_i_ldh_mc_a - Instrução LDH (C), A (opcode 0xE2): escrita em 0xFF00+C.
 *
 * Usa o registrador C como deslocamento em vez de um imediato.
 * Equivalente a: mem[0xFF00 + C] = A
 */
static void gb_i_ldh_mc_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t addr = 0xff00 | cpu->c;

     gb_cpu_writeb(gb, addr, cpu->a);
}

/*
 * gb_i_ldh_a_mc - Instrução LDH A, (C) (opcode 0xF2): leitura de 0xFF00+C para A.
 *
 * Usa o registrador C como deslocamento em vez de um imediato.
 * Equivalente a: A = mem[0xFF00 + C]
 */
static void gb_i_ldh_a_mc(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t addr = 0xff00 | cpu->c;

     cpu->a = gb_cpu_readb(gb, addr);
}

/*
 * gb_i_ld_bc_i16 - Instrução LD BC, nn (opcode 0x01): carrega imediato de 16 bits em BC.
 *
 * Lê dois bytes consecutivos little-endian do fluxo de instruções e os armazena
 * no par BC. Nenhuma flag é afetada.
 * Equivalente a: BC = nn
 */
static void gb_i_ld_bc_i16(struct gb *gb)
{
     uint16_t i16 = gb_cpu_next_i16(gb);

     gb_cpu_set_bc(gb, i16);
}

/*
 * gb_i_ld_de_i16 - Instrução LD DE, nn (opcode 0x11): carrega imediato de 16 bits em DE.
 *
 * Lê dois bytes consecutivos little-endian do fluxo de instruções e os armazena
 * no par DE. Nenhuma flag é afetada.
 * Equivalente a: DE = nn
 */
static void gb_i_ld_de_i16(struct gb *gb)
{
     uint16_t i16 = gb_cpu_next_i16(gb);

     gb_cpu_set_de(gb, i16);
}

/*
 * gb_i_ld_sp_i16 - Instrução LD SP, nn (opcode 0x31): carrega imediato de 16 bits no SP.
 *
 * Lê os dois bytes seguintes ao opcode (little-endian) e os armazena
 * diretamente no Stack Pointer. Tipicamente usada no início do programa
 * para inicializar a pilha em um endereço válido da RAM.
 * Equivalente a: SP = nn
 */
static void gb_i_ld_sp_i16(struct gb *gb)
{
     uint16_t i16 = gb_cpu_next_i16(gb);

     gb->cpu.sp = i16;
}

/*
 * gb_i_ld_sp_hl - Instrução LD SP, HL (opcode 0xF9): copia HL no Stack Pointer.
 *
 * Consome 8 ciclos no total: 4 do fetch e 4 de ciclo interno.
 * Equivalente a: SP = HL
 */
static void gb_i_ld_sp_hl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);

     gb->cpu.sp = hl;

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_i_ld_hl_i16 - Instrução LD HL, nn (opcode 0x21): carrega imediato de 16 bits em HL.
 *
 * Lê os dois bytes seguintes ao opcode (little-endian) e os armazena em HL.
 * HL é frequentemente usado como ponteiro base para acesso indireto à memória.
 * Equivalente a: HL = nn
 */
static void gb_i_ld_hl_i16(struct gb *gb)
{
     uint16_t i16 = gb_cpu_next_i16(gb);
     gb_cpu_set_hl(gb, i16);
}

/*
 * gb_i_ld_mbc_a - Instrução LD (BC), A (opcode 0x02): armazena A no endereço apontado por BC.
 * Equivalente a: mem[BC] = A
 */
static void gb_i_ld_mbc_a(struct gb *gb)
{
     uint16_t bc = gb_cpu_bc(gb);
     uint16_t a = gb->cpu.a;

     gb_cpu_writeb(gb, bc, a);
}

/*
 * gb_i_ld_mde_a - Instrução LD (DE), A (opcode 0x12): armazena A no endereço apontado por DE.
 * Equivalente a: mem[DE] = A
 */
static void gb_i_ld_mde_a(struct gb *gb)
{
     uint16_t de = gb_cpu_de(gb);
     uint16_t a = gb->cpu.a;

     gb_cpu_writeb(gb, de, a);
}

/*
 * gb_i_ld_mhl_a - Instrução LD (HL), A (opcode 0x77): armazena A no endereço apontado por HL.
 * Equivalente a: mem[HL] = A
 */
static void gb_i_ld_mhl_a(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t a = gb->cpu.a;

     gb_cpu_writeb(gb, hl, a);
}

/* --- LD (HL), r : armazena registrador de origem no endereço apontado por HL --- */
static void gb_i_ld_mhl_b(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t b = gb->cpu.b;

     gb_cpu_writeb(gb, hl, b);
}

static void gb_i_ld_mhl_c(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t c = gb->cpu.c;

     gb_cpu_writeb(gb, hl, c);
}

static void gb_i_ld_mhl_d(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t d = gb->cpu.d;

     gb_cpu_writeb(gb, hl, d);
}

static void gb_i_ld_mhl_e(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t e = gb->cpu.e;

     gb_cpu_writeb(gb, hl, e);
}

static void gb_i_ld_mhl_h(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t h = gb->cpu.h;

     gb_cpu_writeb(gb, hl, h);
}

static void gb_i_ld_mhl_l(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t l = gb->cpu.l;

     gb_cpu_writeb(gb, hl, l);
}
/*
 * gb_i_ldi_mhl_a - Instrução LD (HL+), A (opcode 0x22): armazena A em (HL) e incrementa HL.
 *
 * Escreve o acumulador A no endereço apontado por HL e então incrementa HL em 1
 * (com wrap-around de 16 bits). Útil para preenchimento sequencial de buffers.
 * Equivalente a: mem[HL] = A; HL++
 */
static void gb_i_ldi_mhl_a(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t a = gb->cpu.a;

     gb_cpu_writeb(gb, hl, a);

     hl = (hl + 1) & 0xffff;
     gb_cpu_set_hl(gb, hl);
}

/*
 * gb_i_ldd_mhl_a - Instrução LD (HL-), A (opcode 0x32): armazena A em (HL) e decrementa HL.
 *
 * Escreve o acumulador A no endereço apontado por HL e então decrementa HL em 1
 * (com wrap-around de 16 bits). Padrão usado para preencher regiões de memória
 * de trás para frente, como limpar a VRAM em ordem descendente.
 * Equivalente a: mem[HL] = A; HL--
 */
static void gb_i_ldd_mhl_a(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint16_t a = gb->cpu.a;

     gb_cpu_writeb(gb, hl, a);

     hl = (hl - 1) & 0xffff;
     gb_cpu_set_hl(gb, hl);
}

/*
 * gb_i_ld_a_mhl - Instrução LD A, (HL) (opcode 0x7E): lê o byte apontado por HL em A.
 * Equivalente a: A = mem[HL]
 */
static void gb_i_ld_a_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v = gb_cpu_readb(gb, hl);

     gb->cpu.a = v;
}

/*
 * gb_i_ldi_a_mhl - Instrução LD A, (HL+) (opcode 0x2A): carrega (HL) em A e incrementa HL.
 *
 * Lê o byte no endereço apontado por HL, armazena em A e então incrementa HL
 * (com wrap-around de 16 bits). Simétrico de gb_i_ldi_mhl_a (leitura vs escrita).
 * Equivalente a: A = mem[HL]; HL++
 */
static void gb_i_ldi_a_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);

     gb->cpu.a = gb_cpu_readb(gb, hl);

     hl = (hl + 1) & 0xffff;
     gb_cpu_set_hl(gb, hl);
}

/*
 * gb_i_ldd_a_mhl - Instrução LD A, (HL-) (opcode 0x3A): carrega (HL) em A e decrementa HL.
 *
 * Lê o byte no endereço apontado por HL, armazena em A e então decrementa HL
 * (com wrap-around de 16 bits). Simétrico de gb_i_ldd_mhl_a (leitura vs escrita).
 * Equivalente a: A = mem[HL]; HL--
 */
static void gb_i_ldd_a_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);

     gb->cpu.a = gb_cpu_readb(gb, hl);

     hl = (hl - 1) & 0xffff;
     gb_cpu_set_hl(gb, hl);
}

/*
 * gb_i_ld_a_mbc - Instrução LD A, (BC) (opcode 0x0A): lê o byte apontado por BC em A.
 * Equivalente a: A = mem[BC]
 */
static void gb_i_ld_a_mbc(struct gb *gb)
{
     uint16_t bc = gb_cpu_bc(gb);
     uint8_t v = gb_cpu_readb(gb, bc);

     gb->cpu.a = v;
}

/*
 * gb_i_ld_a_mde - Instrução LD A, (DE) (opcode 0x1A): lê o byte apontado por DE em A.
 * Equivalente a: A = mem[DE]
 */
static void gb_i_ld_a_mde(struct gb *gb)
{
     uint16_t de = gb_cpu_de(gb);
     uint8_t v = gb_cpu_readb(gb, de);

     gb->cpu.a = v;
}

/*
 * gb_i_ld_b_mhl - Instrução LD B, (HL) (opcode 0x46): lê o byte apontado por HL em B.
 * Equivalente a: B = mem[HL]
 */
static void gb_i_ld_b_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v = gb_cpu_readb(gb, hl);

     gb->cpu.b = v;
}

/*
 * gb_i_ld_c_mhl - Instrução LD C, (HL) (opcode 0x4E): lê o byte apontado por HL em C.
 * Equivalente a: C = mem[HL]
 */
static void gb_i_ld_c_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v = gb_cpu_readb(gb, hl);

     gb->cpu.c = v;
}

/*
 * gb_i_ld_d_mhl - Instrução LD D, (HL) (opcode 0x56): lê o byte apontado por HL em D.
 * Equivalente a: D = mem[HL]
 */
static void gb_i_ld_d_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v = gb_cpu_readb(gb, hl);

     gb->cpu.d = v;
}

/*
 * gb_i_ld_e_mhl - Instrução LD E, (HL) (opcode 0x5E): lê o byte apontado por HL em E.
 * Equivalente a: E = mem[HL]
 */
static void gb_i_ld_e_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v = gb_cpu_readb(gb, hl);

     gb->cpu.e = v;
}

/*
 * gb_i_ld_h_mhl - Instrução LD H, (HL) (opcode 0x66): lê o byte apontado por HL em H.
 *
 * Atenção: HL é lido antes de ser sobrescrito, portanto o endereço utilizado
 * é o valor original de HL, e somente H é alterado.
 * Equivalente a: H = mem[HL]  (HL_endereço é o valor anterior de HL)
 */
static void gb_i_ld_h_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v = gb_cpu_readb(gb, hl);

     gb->cpu.h = v;
}

/*
 * gb_i_ld_l_mhl - Instrução LD L, (HL) (opcode 0x6E): lê o byte apontado por HL em L.
 *
 * Atenção: HL é lido antes de ser sobrescrito, portanto o endereço utilizado
 * é o valor original de HL, e somente L é alterado.
 * Equivalente a: L = mem[HL]  (HL_endereço é o valor anterior de HL)
 */
static void gb_i_ld_l_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v = gb_cpu_readb(gb, hl);

     gb->cpu.l = v;
}

/*
 * gb_i_ld_hl_sp_si8 - Instrução LD HL, SP+e (opcode 0xF8): calcula SP+e e armazena em HL.
 *
 * Lê o deslocamento com sinal, calcula SP+e via gb_add_sp_si8 (que atualiza as flags)
 * e armazena o resultado em HL sem modificar SP. Consome 12 ciclos.
 * Equivalente a: HL = SP + (int8_t)e
 */
static void gb_i_ld_hl_sp_si8(struct gb *gb)
{
     uint16_t hl = gb_add_sp_si8(gb);

     gb_cpu_set_hl(gb, hl);

     gb_cpu_clock_tick(gb, 4);
}

/*
 * gb_i_push_bc - Instrução PUSH BC (opcode 0xC5): empilha o par BC na pilha.
 *
 * Empilha B (MSB) e depois C (LSB) via gb_cpu_pushw.
 * Equivalente a: push(BC)
 */
static void gb_i_push_bc(struct gb *gb)
{
     uint16_t bc = gb_cpu_bc(gb);

     gb_memory_trigger_oam_bug(gb, gb->cpu.sp);
     gb_cpu_clock_tick(gb, 4);
     gb_cpu_pushb(gb, bc >> 8);
     gb_cpu_pushb(gb, bc & 0xff);
}

/*
 * gb_i_push_de - Instrução PUSH DE (opcode 0xD5): empilha o par DE na pilha.
 *
 * Empilha D (MSB) e depois E (LSB) via gb_cpu_pushw.
 * Equivalente a: push(DE)
 */
static void gb_i_push_de(struct gb *gb)
{
     uint16_t de = gb_cpu_de(gb);

     gb_memory_trigger_oam_bug(gb, gb->cpu.sp);
     gb_cpu_clock_tick(gb, 4);
     gb_cpu_pushb(gb, de >> 8);
     gb_cpu_pushb(gb, de & 0xff);
}

/*
 * gb_i_push_hl - Instrução PUSH HL (opcode 0xE5): empilha o par HL na pilha.
 *
 * Empilha H (MSB) e depois L (LSB) via gb_cpu_pushw.
 * Equivalente a: push(HL)
 */
static void gb_i_push_hl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);

     gb_memory_trigger_oam_bug(gb, gb->cpu.sp);
     gb_cpu_clock_tick(gb, 4);
     gb_cpu_pushb(gb, hl >> 8);
     gb_cpu_pushb(gb, hl & 0xff);
}

/*
 * gb_i_push_af - Instrução PUSH AF (opcode 0xF5): empilha A e os flags na pilha.
 *
 * Ao contrário dos outros PUSHes, AF não é um par simples — o registrador F
 * precisa ser reconstituído a partir dos 4 flags individuais (Z, N, H, C)
 * nos bits 7–4 (bits 3–0 sempre ficam em 0 no hardware real).
 *
 * Ordem de empilhamento: A primeiro (MSB), F depois (LSB).
 * Equivalente a: push(A); push(F)
 */
static void gb_i_push_af(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t f = 0;

     f |= cpu->f_z << 7;
     f |= cpu->f_n << 6;
     f |= cpu->f_h << 5;
     f |= cpu->f_c << 4;

     gb_memory_trigger_oam_bug(gb, cpu->sp);
     gb_cpu_clock_tick(gb, 4);
     gb_cpu_pushb(gb, cpu->a);
     gb_cpu_pushb(gb, f);
}

/*
 * gb_i_pop_bc - Instrução POP BC (opcode 0xC1): desempilha 16 bits da pilha para BC.
 *
 * Desempilha dois bytes via gb_cpu_popw e os armazena no par BC.
 * Equivalente a: BC = pop()
 */
static void gb_i_pop_bc(struct gb *gb)
{
     uint16_t bc = gb_cpu_popw(gb);

     gb_cpu_set_bc(gb, bc);
}

/*
 * gb_i_pop_de - Instrução POP DE (opcode 0xD1): desempilha 16 bits da pilha para DE.
 *
 * Desempilha dois bytes via gb_cpu_popw e os armazena no par DE.
 * Equivalente a: DE = pop()
 */
static void gb_i_pop_de(struct gb *gb)
{
     uint16_t de = gb_cpu_popw(gb);

     gb_cpu_set_de(gb, de);
}

/*
 * gb_i_pop_hl - Instrução POP HL (opcode 0xE1): desempilha 16 bits da pilha para HL.
 *
 * Equivalente a: HL = pop()
 *
 * BUG corrigido: a chamada usava gb_cpu_set_bc em vez de gb_cpu_set_hl.
 */
static void gb_i_pop_hl(struct gb *gb)
{
     uint16_t hl = gb_cpu_popw(gb);

     gb_cpu_set_hl(gb, hl);
}

/*
 * gb_i_pop_af - Instrução POP AF (opcode 0xF1): desempilha 16 bits da pilha para AF (A e flags).
 *
 * Ao contrário dos outros POPs, AF não é um par simples de registradores — o
 * registrador F é composto pelos 4 flags individuais (Z, N, H, C) nos bits 7–4.
 * Os bits 3–0 do byte F são ignorados (sempre 0 no hardware real).
 *
 * Ordem de desempilhamento:
 *   1. Primeiro byte lido = F (flags)
 *   2. Segundo byte lido  = A (acumulador)
 *
 * Equivalente a: F = pop_low(); A = pop_high()
 */
static void gb_i_pop_af(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t f = gb_cpu_popb(gb);
     uint8_t a = gb_cpu_popb(gb);

     cpu->a = a;

     /* Restore flags from memory (low 4 bits ignored) */
     cpu->f_z = f & (1U << 7);
     cpu->f_n = f & (1U << 6);
     cpu->f_h = f & (1U << 5);
     cpu->f_c = f & (1U << 4);
}

/* --- LD A, r : carrega registrador de origem em A (acumulador) --- */
static void gb_i_ld_a_b(struct gb *gb)
{
     gb->cpu.a = gb->cpu.b;
}
static void gb_i_ld_a_c(struct gb *gb)
{
     gb->cpu.a = gb->cpu.c;
}

static void gb_i_ld_a_d(struct gb *gb)
{
     gb->cpu.a = gb->cpu.d;
}

static void gb_i_ld_a_e(struct gb *gb)
{
     gb->cpu.a = gb->cpu.e;
}

static void gb_i_ld_a_h(struct gb *gb)
{
     gb->cpu.a = gb->cpu.h;
}

static void gb_i_ld_a_l(struct gb *gb)
{
     gb->cpu.a = gb->cpu.l;
}

/* --- LD B, r : carrega registrador de origem em B --- */
static void gb_i_ld_b_a(struct gb *gb)
{
     gb->cpu.b = gb->cpu.a;
}

static void gb_i_ld_b_c(struct gb *gb)
{
     gb->cpu.b = gb->cpu.c;
}

static void gb_i_ld_b_d(struct gb *gb)
{
     gb->cpu.b = gb->cpu.d;
}

static void gb_i_ld_b_e(struct gb *gb)
{
     gb->cpu.b = gb->cpu.e;
}

static void gb_i_ld_b_h(struct gb *gb)
{
     gb->cpu.b = gb->cpu.h;
}

static void gb_i_ld_b_l(struct gb *gb)
{
     gb->cpu.b = gb->cpu.l;
}

/* --- LD C, r : carrega registrador de origem em C --- */
static void gb_i_ld_c_a(struct gb *gb)
{
     gb->cpu.c = gb->cpu.a;
}

static void gb_i_ld_c_b(struct gb *gb)
{
     gb->cpu.c = gb->cpu.b;
}

static void gb_i_ld_c_d(struct gb *gb)
{
     gb->cpu.c = gb->cpu.d;
}

static void gb_i_ld_c_e(struct gb *gb)
{
     gb->cpu.c = gb->cpu.e;
}

static void gb_i_ld_c_h(struct gb *gb)
{
     gb->cpu.c = gb->cpu.h;
}

static void gb_i_ld_c_l(struct gb *gb)
{
     gb->cpu.c = gb->cpu.l;
}

/* --- LD D, r : carrega registrador de origem em D --- */
static void gb_i_ld_d_a(struct gb *gb)
{
     gb->cpu.d = gb->cpu.a;
}

static void gb_i_ld_d_b(struct gb *gb)
{
     gb->cpu.d = gb->cpu.b;
}

static void gb_i_ld_d_c(struct gb *gb)
{
     gb->cpu.d = gb->cpu.c;
}

static void gb_i_ld_d_e(struct gb *gb)
{
     gb->cpu.d = gb->cpu.e;
}

static void gb_i_ld_d_h(struct gb *gb)
{
     gb->cpu.d = gb->cpu.h;
}

static void gb_i_ld_d_l(struct gb *gb)
{
     gb->cpu.d = gb->cpu.l;
}

/* --- LD E, r : carrega registrador de origem em E --- */
static void gb_i_ld_e_a(struct gb *gb)
{
     gb->cpu.e = gb->cpu.a;
}

static void gb_i_ld_e_b(struct gb *gb)
{
     gb->cpu.e = gb->cpu.b;
}

static void gb_i_ld_e_c(struct gb *gb)
{
     gb->cpu.e = gb->cpu.c;
}

static void gb_i_ld_e_d(struct gb *gb)
{
     gb->cpu.e = gb->cpu.d;
}

static void gb_i_ld_e_h(struct gb *gb)
{
     gb->cpu.e = gb->cpu.h;
}

static void gb_i_ld_e_l(struct gb *gb)
{
     gb->cpu.e = gb->cpu.l;
}

/* --- LD H, r : carrega registrador de origem em H (byte alto de HL) --- */
static void gb_i_ld_h_a(struct gb *gb)
{
     gb->cpu.h = gb->cpu.a;
}

static void gb_i_ld_h_b(struct gb *gb)
{
     gb->cpu.h = gb->cpu.b;
}

static void gb_i_ld_h_c(struct gb *gb)
{
     gb->cpu.h = gb->cpu.c;
}

static void gb_i_ld_h_d(struct gb *gb)
{
     gb->cpu.h = gb->cpu.d;
}

static void gb_i_ld_h_e(struct gb *gb)
{
     gb->cpu.h = gb->cpu.e;
}

static void gb_i_ld_h_l(struct gb *gb)
{
     gb->cpu.h = gb->cpu.l;
}

/* --- LD L, r : carrega registrador de origem em L (byte baixo de HL) --- */
static void gb_i_ld_l_a(struct gb *gb)
{
     gb->cpu.l = gb->cpu.a;
}

static void gb_i_ld_l_b(struct gb *gb)
{
     gb->cpu.l = gb->cpu.b;
}

static void gb_i_ld_l_c(struct gb *gb)
{
     gb->cpu.l = gb->cpu.c;
}

static void gb_i_ld_l_d(struct gb *gb)
{
     gb->cpu.l = gb->cpu.d;
}

static void gb_i_ld_l_e(struct gb *gb)
{
     gb->cpu.l = gb->cpu.e;
}

static void gb_i_ld_l_h(struct gb *gb)
{
     gb->cpu.l = gb->cpu.h;
}

/*************************
 * SALTOS / CARREGAMENTO *
 *************************/

/*
 * gb_i_jp_i16 - Instrução JP nn (opcode 0xC3): salto incondicional.
 *
 * Lê um endereço de 16 bits imediato do fluxo de instruções e carrega
 * esse valor no PC, desviando a execução para o endereço especificado.
 * Equivalente a: PC = nn
 */
static void gb_i_jp_i16(struct gb *gb)
{
     uint16_t i16 = gb_cpu_next_i16(gb);

     gb_cpu_load_pc(gb, i16);
}

/* --- JP cc, nn : salto condicional absoluto (lê nn mas só desvia se condição verdadeira) --- */
static void gb_i_jp_nz_i16(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t i16 = gb_cpu_next_i16(gb);

     if (!cpu->f_z)
     {
          gb_cpu_load_pc(gb, i16);
     }
}

static void gb_i_jp_z_i16(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t i16 = gb_cpu_next_i16(gb);

     if (cpu->f_z)
     {
          gb_cpu_load_pc(gb, i16);
     }
}

static void gb_i_jp_nc_i16(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t i16 = gb_cpu_next_i16(gb);

     if (!cpu->f_c)
     {
          gb_cpu_load_pc(gb, i16);
     }
}

static void gb_i_jp_c_i16(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint16_t i16 = gb_cpu_next_i16(gb);

     if (cpu->f_c)
     {
          gb_cpu_load_pc(gb, i16);
     }
}

/*
 * gb_i_jp_hl - Instrução JP (HL) (opcode 0xE9): salto indireto para o endereço em HL.
 *
 * Diferente das outras instruções JP, não lê operandos — o destino é simplesmente
 * o valor atual de HL. Consome apenas 4 ciclos (apenas o fetch do opcode).
 * Equivalente a: PC = HL
 */
static void gb_i_jp_hl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);

     gb->cpu.pc = hl;
}

/*
 * gb_i_jr_si8 - Instrução JR e (opcode 0x18): salto relativo incondicional.
 *
 * Lê um deslocamento de 8 bits com sinal (-128 a +127) do fluxo de instruções
 * e soma ao PC atual (que já aponta para a instrução seguinte).
 * O resultado é mascarado para 16 bits, garantindo wrap-around correto.
 * Equivalente a: PC = PC + (int8_t)e
 */
static void gb_i_jr_si8(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t i8 = gb_cpu_next_i8(gb);
     uint16_t pc = cpu->pc;

     pc = pc + (int8_t)i8;

     gb_cpu_load_pc(gb, pc);
}

/*
 * gb_i_jr_z_si8 - Instrução JR Z, e (opcode 0x28): salto relativo se flag Z estiver setada.
 *
 * Se Z=1 (resultado anterior foi zero), aplica o deslocamento ao PC via gb_i_jr_si8.
 * Se Z=0, apenas consome o byte de deslocamento sem desviar.
 * Equivalente a: if (Z) PC += (int8_t)e  else PC++
 */
static void gb_i_jr_z_si8(struct gb *gb)
{
     if (gb->cpu.f_z)
     {
          gb_i_jr_si8(gb);
     }
     else
     {
          gb_cpu_next_i8(gb);
     }
}

/*
 * gb_i_jr_c_si8 - Instrução JR C, e (opcode 0x38): salto relativo se flag C estiver setada.
 *
 * Se C=1 (carry na operação anterior), aplica o deslocamento ao PC.
 * Se C=0, apenas consome o byte de deslocamento sem desviar.
 * Equivalente a: if (C) PC += (int8_t)e  else PC++
 */
static void gb_i_jr_c_si8(struct gb *gb)
{
     if (gb->cpu.f_c)
     {
          gb_i_jr_si8(gb);
     }
     else
     {
          gb_cpu_next_i8(gb);
     }
}

/*
 * gb_i_jr_nz_si8 - Instrução JR NZ, e (opcode 0x20): salto relativo se flag Z estiver limpa.
 *
 * Se Z=0 (resultado anterior não foi zero), aplica o deslocamento ao PC.
 * Se Z=1, apenas consome o byte de deslocamento sem desviar.
 * Equivalente a: if (!Z) PC += (int8_t)e  else PC++
 */
static void gb_i_jr_nz_si8(struct gb *gb)
{
     if (!gb->cpu.f_z)
     {
          gb_i_jr_si8(gb);
     }
     else
     {
          gb_cpu_next_i8(gb);
     }
}

/*
 * gb_i_jr_nc_si8 - Instrução JR NC, e (opcode 0x30): salto relativo se flag C estiver limpa.
 *
 * Se C=0 (sem carry na operação anterior), aplica o deslocamento ao PC.
 * Se C=1, apenas consome o byte de deslocamento sem desviar.
 * Equivalente a: if (!C) PC += (int8_t)e  else PC++
 */
static void gb_i_jr_nc_si8(struct gb *gb)
{
     if (!gb->cpu.f_c)
     {
          gb_i_jr_si8(gb);
     }
     else
     {
          gb_cpu_next_i8(gb);
     }
}

/*
 * gb_i_call_i16 - Instrução CALL nn (opcode 0xCD): chamada incondicional de sub-rotina.
 *
 * Lê o endereço de destino de 16 bits do fluxo de instruções, empilha o
 * endereço de retorno (PC atual, já apontando para a instrução seguinte ao
 * CALL) e desvia a execução para nn. O RET subsequente restaurará o PC
 * desempilhando esse endereço.
 * Equivalente a: push PC; PC = nn
 */
static void gb_i_call_i16(struct gb *gb)
{
     uint16_t i16 = gb_cpu_next_i16(gb);

     gb_cpu_clock_tick(gb, 4);
     gb_cpu_pushw(gb, gb->cpu.pc); /* salva endereço de retorno na pilha */

     gb->cpu.pc = i16;
}

/* --- CALL cc, nn : chamada condicional (lê nn mas só desvia se condição verdadeira) --- */
static void gb_i_call_nz_i16(struct gb *gb)
{
     if (!gb->cpu.f_z)
     {
          gb_i_call_i16(gb);
     }
     else
     {
          gb_cpu_next_i16(gb);
     }
}

static void gb_i_call_z_i16(struct gb *gb)
{
     if (gb->cpu.f_z)
     {
          gb_i_call_i16(gb);
     }
     else
     {
          gb_cpu_next_i16(gb);
     }
}

static void gb_i_call_nc_i16(struct gb *gb)
{
     if (!gb->cpu.f_c)
     {
          gb_i_call_i16(gb);
     }
     else
     {
          gb_cpu_next_i16(gb);
     }
}

static void gb_i_call_c_i16(struct gb *gb)
{
     if (gb->cpu.f_c)
     {
          gb_i_call_i16(gb);
     }
     else
     {
          gb_cpu_next_i16(gb);
     }
}

/*
 * gb_i_ret - Instrução RET (opcode 0xC9): retorno incondicional de sub-rotina.
 *
 * Desempilha o endereço de retorno de 16 bits (salvo pelo CALL correspondente)
 * e o carrega no PC, retomando a execução a partir da instrução seguinte ao CALL.
 * Equivalente a: PC = pop()
 *
 * Complemento direto de gb_i_call_i16.
 */
static void gb_i_ret(struct gb *gb)
{
     uint16_t addr = gb_cpu_popw(gb); /* recupera endereço de retorno da pilha */

     gb_cpu_load_pc(gb, addr);
}

/*
 * gb_i_ret_z - Instrução RET Z (opcode 0xC8): retorno de sub-rotina se Z estiver setada.
 *
 * Desempilha o endereço de retorno e carrega no PC somente se Z=1.
 * Equivalente a: if (Z) PC = pop()
 */
static void gb_i_ret_z(struct gb *gb)
{
     gb_cpu_clock_tick(gb, 4);

     if (gb->cpu.f_z)
     {
          gb_i_ret(gb);
     }
}

/*
 * gb_i_ret_c - Instrução RET C (opcode 0xD8): retorno de sub-rotina se C estiver setada.
 *
 * Desempilha o endereço de retorno e carrega no PC somente se C=1.
 * Equivalente a: if (C) PC = pop()
 */
static void gb_i_ret_c(struct gb *gb)
{
     gb_cpu_clock_tick(gb, 4);

     if (gb->cpu.f_c)
     {
          gb_i_ret(gb);
     }
}

/*
 * gb_i_ret_nz - Instrução RET NZ (opcode 0xC0): retorno de sub-rotina se Z estiver limpa.
 *
 * Desempilha o endereço de retorno e carrega no PC somente se Z=0.
 * Equivalente a: if (!Z) PC = pop()
 *
 * BUG corrigido: a condição original era (f_z) em vez de (!f_z), invertendo
 * o comportamento da instrução (retornava quando Z estava setada, não limpa).
 */
static void gb_i_ret_nz(struct gb *gb)
{
     gb_cpu_clock_tick(gb, 4);

     if (!gb->cpu.f_z)
     {
          gb_i_ret(gb);
     }
}

/*
 * gb_i_ret_nc - Instrução RET NC (opcode 0xD0): retorno de sub-rotina se C estiver limpa.
 *
 * Desempilha o endereço de retorno e carrega no PC somente se C=0.
 * Equivalente a: if (!C) PC = pop()
 */
static void gb_i_ret_nc(struct gb *gb)
{
     gb_cpu_clock_tick(gb, 4);

     if (!gb->cpu.f_c)
     {
          gb_i_ret(gb);
     }
}

/*
 * gb_i_reti - Instrução RETI (opcode 0xD9): retorno de rotina de interrupção.
 *
 * Desempilha o endereço de retorno e habilita as interrupções imediatamente
 * (sem o atraso de EI). Usado para finalizar handlers de IRQ.
 * Equivalente a: PC = pop(); IME = 1
 */
static void gb_i_reti(struct gb *gb)
{
     gb_i_ret(gb);

     gb->cpu.irq_enable = true;
     gb->cpu.irq_enable_next = true;
     gb->cpu.irq_enable_delay = 0;
}

/*
 * gb_cpu_rst - Executa um reset de software para o vetor fixo 'target'.
 *
 * Empilha o PC atual (endereço de retorno) e desvia para o endereço de destino.
 * Os vetores RST do LR35902 são: 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38.
 * Equivalente a um CALL para um endereço fixo de 1 byte de codificação.
 */
static void gb_cpu_rst(struct gb *gb, uint16_t target)
{
     gb_cpu_clock_tick(gb, 4);
     gb_cpu_pushw(gb, gb->cpu.pc);

     gb->cpu.pc = target;
}

/* --- RST n : chamada para vetor fixo (empilha PC e salta para endereço de 1 byte) --- */
static void gb_i_rst_00(struct gb *gb)
{
     gb_cpu_rst(gb, 0x00);
}

static void gb_i_rst_08(struct gb *gb)
{
     gb_cpu_rst(gb, 0x08);
}

static void gb_i_rst_10(struct gb *gb)
{
     gb_cpu_rst(gb, 0x10);
}

static void gb_i_rst_18(struct gb *gb)
{
     gb_cpu_rst(gb, 0x18);
}

static void gb_i_rst_20(struct gb *gb)
{
     gb_cpu_rst(gb, 0x20);
}

static void gb_i_rst_28(struct gb *gb)
{
     gb_cpu_rst(gb, 0x28);
}

static void gb_i_rst_30(struct gb *gb)
{
     gb_cpu_rst(gb, 0x30);
}

static void gb_i_rst_38(struct gb *gb)
{
     gb_cpu_rst(gb, 0x38);
}

/* Forward declaration: gb_i_op_cb é definida após gb_instructions_cb (ver abaixo). */
static void gb_i_op_cb(struct gb *gb);

/*
 * gb_instructions - Tabela de despacho de instruções (dispatch table).
 *
 * Array de 256 ponteiros de função indexado pelo opcode (0x00–0xFF).
 * gb_cpu_run_instruction() usa o opcode lido da memória como índice direto,
 * tornando o despacho O(1). Instruções não implementadas apontam para
 * gb_i_unimplemented como sentinela.
 *
 */

static gb_instruction_f gb_instructions[0x100] = {
    // 0x00 - NOP e instruções de carga de registradores de 16 bits (BC, DE), rotações
    gb_i_nop,        /* 0x00: NOP */
    gb_i_ld_bc_i16,  /* 0x01: LD BC, nn */
    gb_i_ld_mbc_a,   /* 0x02: LD (BC), A */
    gb_i_inc_bc,     /* 0x03: INC BC */
    gb_i_inc_b,      /* 0x04: INC B */
    gb_i_dec_b,      /* 0x05: DEC B */
    gb_i_ld_b_i8,    /* 0x06: LD B, n */
    gb_i_rlca,       /* 0x07: RLCA */
    gb_i_ld_mi16_sp, /* 0x08: LD (nn), SP */
    gb_i_add_hl_bc,  /* 0x09: ADD HL, BC */
    gb_i_ld_a_mbc,   /* 0x0A: LD A, (BC) */
    gb_i_dec_bc,     /* 0x0B: DEC BC */
    gb_i_inc_c,      /* 0x0C: INC C */
    gb_i_dec_c,      /* 0x0D: DEC C */
    gb_i_ld_c_i8,    /* 0x0E: LD C, n */
    gb_i_rrca,       /* 0x0F: RRCA */
    // 0x10 - STOP, cargas de pares de registradores DE/HL, rotações e saltos relativos
    gb_i_stop,      /* 0x10: STOP */
    gb_i_ld_de_i16, /* 0x11: LD DE, nn */
    gb_i_ld_mde_a,  /* 0x12: LD (DE), A */
    gb_i_inc_de,    /* 0x13: INC DE */
    gb_i_inc_d,     /* 0x14: INC D */
    gb_i_dec_d,     /* 0x15: DEC D */
    gb_i_ld_d_i8,   /* 0x16: LD D, n */
    gb_i_rla,       /* 0x17: RLA */
    gb_i_jr_si8,    /* 0x18: JR e */
    gb_i_add_hl_de, /* 0x19: ADD HL, DE */
    gb_i_ld_a_mde,  /* 0x1A: LD A, (DE) */
    gb_i_dec_de,    /* 0x1B: DEC DE */
    gb_i_inc_e,     /* 0x1C: INC E */
    gb_i_dec_e,     /* 0x1D: DEC E */
    gb_i_ld_e_i8,   /* 0x1E: LD E, n */
    gb_i_rra,       /* 0x1F: RRA */
    // 0x20 - saltos relativos condicionais, cargas HL, incrementos e DAA/CPL
    gb_i_jr_nz_si8, /* 0x20: JR NZ, e */
    gb_i_ld_hl_i16, /* 0x21: LD HL, nn */
    gb_i_ldi_mhl_a, /* 0x22: LD (HL+), A */
    gb_i_inc_hl,    /* 0x23: INC HL */
    gb_i_inc_h,     /* 0x24: INC H */
    gb_i_dec_h,     /* 0x25: DEC H */
    gb_i_ld_h_i8,   /* 0x26: LD H, n */
    gb_i_daa,       /* 0x27: DAA */
    gb_i_jr_z_si8,  /* 0x28: JR Z, e */
    gb_i_add_hl_hl, /* 0x29: ADD HL, HL */
    gb_i_ldi_a_mhl, /* 0x2A: LD A, (HL+) */
    gb_i_dec_hl,    /* 0x2B: DEC HL */
    gb_i_inc_l,     /* 0x2C: INC L */
    gb_i_dec_l,     /* 0x2D: DEC L */
    gb_i_ld_l_i8,   /* 0x2E: LD L, n */
    gb_i_cpl_a,     /* 0x2F: CPL A*/
    // 0x30 - JR NC/C, carga SP, manipulação de HL com auto-inc/dec e flags SCF/CCF
    gb_i_jr_nc_si8, /* 0x30: JR NC, e */
    gb_i_ld_sp_i16, /* 0x31: LD SP, nn */
    gb_i_ldd_mhl_a, /* 0x32: LD (HL-), A */
    gb_i_inc_sp,    /* 0x33: INC SP */
    gb_i_inc_mhl,   /* 0x34: INC (HL) */
    gb_i_dec_mhl,   /* 0x35: DEC (HL) */
    gb_i_ld_mhl_i8, /* 0x36: LD (HL), n */
    gb_i_scf,       /* 0x37: SCF */
    gb_i_jr_c_si8,  /* 0x38: JR C, e */
    gb_i_add_hl_sp, /* 0x39: ADD HL, SP */
    gb_i_ldd_a_mhl, /* 0x3A: LD A, (HL-) */
    gb_i_dec_sp,    /* 0x3B: DEC SP */
    gb_i_inc_a,     /* 0x3C: INC A */
    gb_i_dec_a,     /* 0x3D: DEC A */
    gb_i_ld_a_i8,   /* 0x3E: LD A, n */
    gb_i_ccf,       /* 0x3F: CCF */
    // 0x40 - bloco de instruções LD r, r' (registrador para registrador): B e C como destino
    gb_i_nop,      /* 0x40: LD B, B */
    gb_i_ld_b_c,   /* 0x41: LD B, C */
    gb_i_ld_b_d,   /* 0x42: LD B, D */
    gb_i_ld_b_e,   /* 0x43: LD B, E */
    gb_i_ld_b_h,   /* 0x44: LD B, H */
    gb_i_ld_b_l,   /* 0x45: LD B, L */
    gb_i_ld_b_mhl, /* 0x46: LD B, (HL) */
    gb_i_ld_b_a,   /* 0x47: LD B, A */
    gb_i_ld_c_b,   /* 0x48: LD C, B */
    gb_i_nop,      /* 0x49: LD C, C */
    gb_i_ld_c_d,   /* 0x4A: LD C, D */
    gb_i_ld_c_e,   /* 0x4B: LD C, E */
    gb_i_ld_c_h,   /* 0x4C: LD C, H */
    gb_i_ld_c_l,   /* 0x4D: LD C, L */
    gb_i_ld_c_mhl, /* 0x4E: LD C, (HL) */
    gb_i_ld_c_a,   /* 0x4F: LD C, A */
    // 0x50 - bloco de instruções LD r, r' (continuação): registradores D e E como destino
    gb_i_ld_d_b,   /* 0x50: LD D, B */
    gb_i_ld_d_c,   /* 0x51: LD D, C */
    gb_i_nop,      /* 0x52: LD D, D */
    gb_i_ld_d_e,   /* 0x53: LD D, E */
    gb_i_ld_d_h,   /* 0x54: LD D, H */
    gb_i_ld_d_l,   /* 0x55: LD D, L */
    gb_i_ld_d_mhl, /* 0x56: LD D, (HL) */
    gb_i_ld_d_a,   /* 0x57: LD D, A */
    gb_i_ld_e_b,   /* 0x58: LD E, B */
    gb_i_ld_e_c,   /* 0x59: LD E, C */
    gb_i_ld_e_d,   /* 0x5A: LD E, D */
    gb_i_nop,      /* 0x5B: LD E, E */
    gb_i_ld_e_h,   /* 0x5C: LD E, H */
    gb_i_ld_e_l,   /* 0x5D: LD E, L */
    gb_i_ld_e_mhl, /* 0x5E: LD E, (HL) */
    gb_i_ld_e_a,   /* 0x5F: LD E, A */
    // 0x60 - bloco de instruções LD r, r' (continuação): registradores H e L como destino
    gb_i_ld_h_b,   /* 0x60: LD H, B */
    gb_i_ld_h_c,   /* 0x61: LD H, C */
    gb_i_ld_h_d,   /* 0x62: LD H, D */
    gb_i_ld_h_e,   /* 0x63: LD H, E */
    gb_i_nop,      /* 0x64: LD H, H */
    gb_i_ld_h_l,   /* 0x65: LD H, L */
    gb_i_ld_h_mhl, /* 0x66: LD H, (HL) */
    gb_i_ld_h_a,   /* 0x67: LD H, A */
    gb_i_ld_l_b,   /* 0x68: LD L, B */
    gb_i_ld_l_c,   /* 0x69: LD L, C */
    gb_i_ld_l_d,   /* 0x6A: LD L, D */
    gb_i_ld_l_e,   /* 0x6B: LD L, E */
    gb_i_ld_l_h,   /* 0x6C: LD L, H */
    gb_i_nop,      /* 0x6D: LD L, L */
    gb_i_ld_l_mhl, /* 0x6E: LD L, (HL) */
    gb_i_ld_l_a,   /* 0x6F: LD L, A */
    // 0x70 - bloco de instruções LD r, r' (continuação): (HL) e A como destino, mais HALT
    gb_i_ld_mhl_b, /* 0x70: LD (HL), B */
    gb_i_ld_mhl_c, /* 0x71: LD (HL), C */
    gb_i_ld_mhl_d, /* 0x72: LD (HL), D */
    gb_i_ld_mhl_e, /* 0x73: LD (HL), E */
    gb_i_ld_mhl_h, /* 0x74: LD (HL), H */
    gb_i_ld_mhl_l, /* 0x75: LD (HL), L */
    gb_i_halt,     /* 0x76: HALT — suspende CPU até a próxima interrupção */
    gb_i_ld_mhl_a, /* 0x77: LD (HL), A */
    gb_i_ld_a_b,   /* 0x78: LD A, B */
    gb_i_ld_a_c,   /* 0x79: LD A, C */
    gb_i_ld_a_d,   /* 0x7A: LD A, D */
    gb_i_ld_a_e,   /* 0x7B: LD A, E */
    gb_i_ld_a_h,   /* 0x7C: LD A, H */
    gb_i_ld_a_l,   /* 0x7D: LD A, L */
    gb_i_ld_a_mhl, /* 0x7E: LD A, (HL) */
    gb_i_nop,      /* 0x7F: LD A, A */
    // 0x80 - bloco de instruções aritméticas ADD/ADC (soma sem/com carry)
    gb_i_add_a_b,   /* 0x80: ADD A, B */
    gb_i_add_a_c,   /* 0x81: ADD A, C */
    gb_i_add_a_d,   /* 0x82: ADD A, D */
    gb_i_add_a_e,   /* 0x83: ADD A, E */
    gb_i_add_a_h,   /* 0x84: ADD A, H */
    gb_i_add_a_l,   /* 0x85: ADD A, L */
    gb_i_add_a_mhl, /* 0x86: ADD A, (HL) */
    gb_i_add_a_a,   /* 0x87: ADD A, A */
    gb_i_adc_a_b,   /* 0x88: ADC A, B */
    gb_i_adc_a_c,   /* 0x89: ADC A, C */
    gb_i_adc_a_d,   /* 0x8A: ADC A, D */
    gb_i_adc_a_e,   /* 0x8B: ADC A, E */
    gb_i_adc_a_h,   /* 0x8C: ADC A, H */
    gb_i_adc_a_l,   /* 0x8D: ADC A, L */
    gb_i_adc_a_mhl, /* 0x8E: ADC A, (HL) */
    gb_i_adc_a_a,   /* 0x8F: ADC A, A */
    // 0x90 - bloco de instruções SUB/SBC (subtração sem/com borrow)
    gb_i_sub_a_b,   /* 0x90: SUB B */
    gb_i_sub_a_c,   /* 0x91: SUB C */
    gb_i_sub_a_d,   /* 0x92: SUB D */
    gb_i_sub_a_e,   /* 0x93: SUB E */
    gb_i_sub_a_h,   /* 0x94: SUB H */
    gb_i_sub_a_l,   /* 0x95: SUB L */
    gb_i_sub_a_mhl, /* 0x96: SUB (HL) */
    gb_i_sub_a_a,   /* 0x97: SUB A  */
    gb_i_sbc_a_b,   /* 0x98: SBC A, B */
    gb_i_sbc_a_c,   /* 0x99: SBC A, C */
    gb_i_sbc_a_d,   /* 0x9A: SBC A, D */
    gb_i_sbc_a_e,   /* 0x9B: SBC A, E */
    gb_i_sbc_a_h,   /* 0x9C: SBC A, H */
    gb_i_sbc_a_l,   /* 0x9D: SBC A, L */
    gb_i_sbc_a_mhl, /* 0x9E: SBC A, (HL) */
    gb_i_sbc_a_a,   /* 0x9F: SBC A, A */
    // 0xA0 - bloco lógico AND/XOR (AND seta H=1; XOR seta todas as flags a 0 exceto Z)
    gb_i_and_a_b,   /* 0xA0: AND B */
    gb_i_and_a_c,   /* 0xA1: AND C */
    gb_i_and_a_d,   /* 0xA2: AND D */
    gb_i_and_a_e,   /* 0xA3: AND E */
    gb_i_and_a_h,   /* 0xA4: AND H */
    gb_i_and_a_l,   /* 0xA5: AND L */
    gb_i_and_a_mhl, /* 0xA6: AND (HL) */
    gb_i_and_a_a,   /* 0xA7: AND A */
    gb_i_xor_a_b,   /* 0xA8: XOR B */
    gb_i_xor_a_c,   /* 0xA9: XOR C */
    gb_i_xor_a_d,   /* 0xAA: XOR D */
    gb_i_xor_a_e,   /* 0xAB: XOR E */
    gb_i_xor_a_h,   /* 0xAC: XOR H */
    gb_i_xor_a_l,   /* 0xAD: XOR L */
    gb_i_xor_a_mhl, /* 0xAE: XOR (HL) */
    gb_i_xor_a_a,   /* 0xAF: XOR A — padrão para zerar A (A^A=0, 1 byte) */
    // 0xB0 - bloco lógico OR/CP (CP compara sem armazenar resultado, apenas flags)
    gb_i_or_a_b,   /* 0xB0: OR B */
    gb_i_or_a_c,   /* 0xB1: OR C */
    gb_i_or_a_d,   /* 0xB2: OR D */
    gb_i_or_a_e,   /* 0xB3: OR E */
    gb_i_or_a_h,   /* 0xB4: OR H */
    gb_i_or_a_l,   /* 0xB5: OR L */
    gb_i_or_a_mhl, /* 0xB6: OR (HL) */
    gb_i_or_a_a,   /* 0xB7: OR A */
    gb_i_cp_a_b,   /* 0xB8: CP B */
    gb_i_cp_a_c,   /* 0xB9: CP C */
    gb_i_cp_a_d,   /* 0xBA: CP D */
    gb_i_cp_a_e,   /* 0xBB: CP E */
    gb_i_cp_a_h,   /* 0xBC: CP H */
    gb_i_cp_a_l,   /* 0xBD: CP L */
    gb_i_cp_a_mhl, /* 0xBE: CP (HL) */
    gb_i_cp_a_a,   /* 0xBF: CP A — sempre Z=1 (A-A=0), útil para testar flags */
    // 0xC0 - saltos condicionais, CALL, RET e RST (reset vectors)
    gb_i_ret_nz,      /* 0xC0: RET NZ */
    gb_i_pop_bc,      /* 0xC1: POP BC */
    gb_i_jp_nz_i16,   /* 0xC2: JP NZ, nn */
    gb_i_jp_i16,      /* 0xC3: JP nn  */
    gb_i_call_nz_i16, /* 0xC4: CALL NZ, nn */
    gb_i_push_bc,     /* 0xC5: PUSH BC */
    gb_i_add_a_i8,    /* 0xC6: ADD A, n */
    gb_i_rst_00,      /* 0xC7: RST 00H — salta para vetor de interrupção 0x0000 */
    gb_i_ret_z,       /* 0xC8: RET Z */
    gb_i_ret,         /* 0xC9: RET  */
    gb_i_jp_z_i16,    /* 0xCA: JP Z, nn */
    gb_i_op_cb,       /* 0xCB: prefixo CB — instrução extendida de bit (2 bytes) */
    gb_i_call_z_i16,  /* 0xCC: CALL Z, nn */
    gb_i_call_i16,    /* 0xCD: CALL nn  */
    gb_i_adc_a_i8,    /* 0xCE: ADC A, n */
    gb_i_rst_08,      /* 0xCF: RST 08H */
    // 0xD0 - RET/JP/CALL condicionais por carry (NC/C) e RST
    gb_i_ret_nc,      /* 0xD0: RET NC */
    gb_i_pop_de,      /* 0xD1: POP DE */
    gb_i_jp_nc_i16,   /* 0xD2: JP NC, nn */
    gb_i_undefined,   /* 0xD3: (inválido — não existe no LR35902) */
    gb_i_call_nc_i16, /* 0xD4: CALL NC, nn */
    gb_i_push_de,     /* 0xD5: PUSH DE */
    gb_i_sub_a_i8,    /* 0xD6: SUB n */
    gb_i_rst_10,      /* 0xD7: RST 10H */
    gb_i_ret_c,       /* 0xD8: RET C */
    gb_i_reti,        /* 0xD9: RETI — RET e habilita interrupções (IME=1) */
    gb_i_jp_c_i16,    /* 0xDA: JP C, nn */
    gb_i_undefined,   /* 0xDB: (inválido) */
    gb_i_call_c_i16,  /* 0xDC: CALL C, nn */
    gb_i_undefined,   /* 0xDD: (inválido) */
    gb_i_sbc_a_i8,    /* 0xDE: SBC A, n */
    gb_i_rst_18,      /* 0xDF: RST 18H */
    // 0xE0 - instruções de I/O (LDH), pilha (PUSH/POP HL) e aritmética de SP
    gb_i_ldh_mi8_a,  /* 0xE0: LDH (n), A  */
    gb_i_pop_hl,     /* 0xE1: POP HL */
    gb_i_ldh_mc_a,   /* 0xE2: LDH (C), A — escreve A em 0xFF00+C */
    gb_i_undefined,  /* 0xE3: (inválido) */
    gb_i_undefined,  /* 0xE4: (inválido) */
    gb_i_push_hl,    /* 0xE5: PUSH HL */
    gb_i_and_a_i8,   /* 0xE6: AND n */
    gb_i_rst_20,     /* 0xE7: RST 20H */
    gb_i_add_sp_si8, /* 0xE8: ADD SP, e  */
    gb_i_jp_hl,      /* 0xE9: JP (HL) — salto indireto para endereço em HL */
    gb_i_ld_mi16_a,  /* 0xEA: LD (nn), A  */
    gb_i_undefined,  /* 0xEB: (inválido) */
    gb_i_undefined,  /* 0xEC: (inválido) */
    gb_i_undefined,  /* 0xED: (inválido) */
    gb_i_xor_a_i8,   /* 0xEE: XOR n */
    gb_i_rst_28,     /* 0xEF: RST 28H */
    // 0xF0 - instruções de I/O (LDH leitura), pilha AF, controle de interrupções (DI/EI)
    gb_i_ldh_a_mi8,    /* 0xF0: LDH A, (n) — lê registrador de I/O para A */
    gb_i_pop_af,       /* 0xF1: POP AF — restaura A e flags da pilha */
    gb_i_ldh_a_mc,     /* 0xF2: LDH A, (C) — lê 0xFF00+C para A */
    gb_i_di,           /* 0xF3: DI */
    gb_i_undefined,    /* 0xF4: (inválido) */
    gb_i_push_af,      /* 0xF5: PUSH AF — salva A e flags na pilha */
    gb_i_or_a_i8,      /* 0xF6: OR n */
    gb_i_rst_30,       /* 0xF7: RST 30H */
    gb_i_ld_hl_sp_si8, /* 0xF8: LD HL, SP+e — HL = SP + deslocamento com sinal */
    gb_i_ld_sp_hl,     /* 0xF9: LD SP, HL */
    gb_i_ld_a_mi16,    /* 0xFA: LD A, (nn) — lê byte de endereço absoluto para A */
    gb_i_ei,           /* 0xFB: EI — habilita interrupções (IME=1, efeito na próxima instrução) */
    gb_i_undefined,    /* 0xFC: (inválido) */
    gb_i_undefined,    /* 0xFD: (inválido) */
    gb_i_cp_a_i8,      /* 0xFE: CP n — compara A com imediato (A-n), atualiza apenas flags */
    gb_i_rst_38,       /* 0xFF: RST 38H */
};

/* Addresses of the interrupt handlers in memory */
static const uint16_t gb_irq_handlers[5] = {
    [GB_IRQ_VSYNC] = 0x0040,
    [GB_IRQ_LCD_STAT] = 0x0048,
    [GB_IRQ_TIMER] = 0x0050,
    [GB_IRQ_SERIAL] = 0x0058,
    [GB_IRQ_INPUT] = 0x0060

};

/*
 * gb_cpu_check_interrupts - Verifica e despacha interrupções pendentes.
 *
 * Chamada entre instruções. Se IME=1 e há um IRQ ativo (enable & flags & 0x1F),
 * desabilita o IME, empilha o PC, limpa o bit da interrupção e desvia para o
 * handler correspondente. IRQs de menor índice têm maior prioridade.
 *
 * Mesmo com IME=0, um IRQ pendente tira a CPU do estado de halt.
 */
static void gb_cpu_check_interrupts(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;
     struct gb_irq *irq = &gb->irq;
     uint8_t active_irq;
     uint16_t handler;
     unsigned i;

     /* Verifica se há alguma interrupção pendente */
     active_irq = irq->irq_enable & irq->irq_flags & 0x1f;

     if (cpu->stopped)
     {
          if (!(active_irq & (1U << GB_IRQ_INPUT)))
               return;
          cpu->stopped = false;
     }

     if (!active_irq)
     {
          return;
     }

     /* We have an active IRQ, that gets us outside of halted mode even if the
      * IME is not set in the CPU */
     cpu->halted = false;

     if (!cpu->irq_enable)
     {
          /* IME is not set, nothing to do */
          return;
     }

     /*
      * A real interrupt dispatch consumes the next fetch slot. If HALT saw a
      * pending IRQ while EI's delayed IME enable was still counting down, the
      * stale HALT-bug latch must not leak into the interrupt vector fetch.
      */
     cpu->halt_bug = false;

     /* Find the first active IRQ. The order is significant, IRQs with a lower
      * number have the priority. */
     for (i = 0; i < 5; i++)
     {
          if (active_irq & (1U << i))
          {
               break;
          }
     }

     /* That shouldn't happen since we check if we have an active IRQ above */
     assert(i < 5);

     handler = gb_irq_handlers[i];

     cpu->irq_enable = false;
     cpu->irq_enable_next = false;
     cpu->irq_enable_delay = 0;

     /* Interrupt dispatch takes 20 cycles total:
      *  - 2 internal cycles (8 T-cycles) before the push
      *  - push high byte (4 T-cycles via writeb)
      *  - push low byte  (4 T-cycles via writeb)
      *  - load PC        (4 T-cycles via load_pc)
      */
     gb_cpu_clock_tick(gb, 8);

     /* Push high byte of PC. After this write, IE may have changed if SP-1
      * happened to land on 0xFFFF (the IE register). Re-read active_irq using
      * the potentially-modified IE value to implement the ie_push hardware bug. */
     gb_memory_trigger_oam_bug(gb, gb->cpu.sp);
     gb->cpu.sp = (gb->cpu.sp - 1) & 0xffff;
     gb_cpu_writeb(gb, gb->cpu.sp, gb->cpu.pc >> 8);

     /* Re-check active IRQ with (possibly modified) IE */
     active_irq = irq->irq_enable & irq->irq_flags & 0x1f;
     if (active_irq)
     {
          for (i = 0; i < 5; i++)
          {
               if (active_irq & (1U << i))
               {
                    break;
               }
          }
          handler = gb_irq_handlers[i];
     }
     else
     {
          /* IE was cleared (or no longer matches IF) after the high byte push.
           * The CPU dispatches to 0x0000 in this case. */
          handler = 0x0000;
          i = 5; /* sentinel: no IRQ to acknowledge */
     }

     /* Push low byte of PC */
     gb->cpu.sp = (gb->cpu.sp - 1) & 0xffff;
     gb_cpu_writeb(gb, gb->cpu.sp, gb->cpu.pc & 0xff);

     /* Acknowledge the interrupt that was selected */
     if (i < 5)
     {
          irq->irq_flags &= ~(1U << i);
          gb_debug_hw_trace_irq(gb, true, irq->irq_flags, irq->irq_enable);
     }

     /* Jump to the IRQ handler */
     gb_cpu_load_pc(gb, handler);
}

static void gb_cpu_update_ime(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     if (!cpu->irq_enable_delay)
     {
          return;
     }

     cpu->irq_enable_delay--;

     if (!cpu->irq_enable_delay)
     {
          cpu->irq_enable = cpu->irq_enable_next;
     }
}

/*
 * gb_cpu_run_instruction - Executa um único ciclo fetch-decode-execute.
 *
 * 1. Fetch:  lê o byte no endereço atual do PC e incrementa o PC.
 * 2. Decode: usa o byte como índice na tabela gb_instructions.
 * 3. Execute: chama o ponteiro de função correspondente.
 *
 * Esta função deve ser chamada repetidamente pelo loop principal do emulador
 * para simular a execução contínua da CPU.
 */
static void gb_cpu_run_instruction(struct gb *gb)
{
     uint8_t instruction;
     uint16_t fetch_pc = gb->cpu.pc;

     gb->cpu.trace_buf[gb->cpu.trace_head] = fetch_pc;
     gb->cpu.trace_head = (gb->cpu.trace_head + 1) % GB_CPU_TRACE_SIZE;

     instruction = gb_cpu_next_i8(gb);
     gb_debug_hw_trace_cpu_fetch(gb, fetch_pc, instruction);

     gb_instructions[instruction](gb);
}

/*
 * gb_cpu_run_cycles - Executa instruções até consumir o número de ciclos solicitado.
 *
 * Reinicia a base de tempo via gb_sync_rebase e entra em loop, executando uma
 * instrução por iteração. Antes do próximo fetch, verifica interrupções
 * pendentes; após executar uma instrução real, aplica o atraso do EI. Se a CPU
 * estiver em halt, avança o timestamp até o próximo evento programado.
 *
 * Retorna o timestamp final (pode ser > cycles por causa do último ciclo).
 */
int32_t gb_cpu_run_cycles(struct gb *gb, int32_t cycles)
{

     struct gb_cpu *cpu = &gb->cpu;

     gb_sync_rebase(gb);

     while (gb->timestamp < cycles)
     {
          gb_cpu_check_interrupts(gb);

          if (cpu->halted || cpu->stopped)
          {
               int32_t skip_cycles;

               /* The CPU is halted so we skip to the next event or `cycles`,
                * whichever comes first */
               if (cycles < gb->sync.first_event)
               {
                    skip_cycles = cycles - gb->timestamp;
               }
               else
               {
                    skip_cycles = gb->sync.first_event - gb->timestamp;
               }

               if (skip_cycles > 0)
               {
                    gb->timestamp += skip_cycles;
               }
               else
               {
                    /* Avoid spinning if an event reschedules onto the current
                     * timestamp while the CPU is halted in double-speed mode. */
                    gb_sync_check_events(gb);
                    if (gb->sync.first_event <= gb->timestamp)
                         gb->timestamp++;
               }

               /* See if any event needs to run. This may trigger an IRQ which
                * will un-halt the CPU in the next iteration */
               gb_sync_check_events(gb);
          }
          else
          {
               bool debug_stepping = gb->debug.enabled &&
                                     gb->debug.state == GB_DEBUG_STEPPING;

               gb_debug_before_instr(gb);

               if (gb->debug.enabled && gb->debug.state == GB_DEBUG_PAUSED)
               {
                    break;
               }

               gb_cpu_run_instruction(gb);
               gb_cpu_update_ime(gb);

               if (debug_stepping && gb->debug.state == GB_DEBUG_STEPPING)
               {
                    gb->debug.state = GB_DEBUG_PAUSED;
               }
          }
     }

     return gb->timestamp;
}

/*
 * Extendend CB instruction map
 */

/*
 * gb_i_unimplemented_cb - Tratador padrão para instruções CB ainda não implementadas.
 *
 * Análogo a gb_i_unimplemented, mas para opcodes do mapa estendido (prefixo 0xCB).
 * Imprime "Unimplemented instruction 0xCB 0xXX at 0xYYYY" e encerra a execução.
 * O PC já foi incrementado ao entrar aqui, então recua 1 para ler o sub-opcode.
 */

/* static void gb_i_unimplemented_cb(struct gb *gb)
{
    struct gb_cpu *cpu = &gb->cpu;
    uint16_t instruction_pc = (cpu->pc - 1) & 0xffff;
    uint8_t instruction = gb_cpu_readb(gb, instruction_pc);

    fprintf(stderr, "Unimplemented instruction 0xCB 0x%02x at 0x%04x\n", instruction, instruction_pc);

    die();
} */

/*
 * gb_cpu_rlc_set_flags - Rotação circular para a esquerda e atualiza as flags.
 *
 * O bit 7 vai para C e também para o bit 0 (circular).
 * Equivalente a: C = *v >> 7; *v = (*v << 1) | C
 * Flags: Z, N=0, H=0, C = bit 7 original.
 */
static void gb_cpu_rlc_set_flags(struct gb *gb, uint8_t *v)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t c = *v >> 7;

     *v = (*v << 1) | c;

     cpu->f_z = (*v == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = c;
}

/* --- RLC r : rotação circular para a esquerda (CB 0x00–0x07) --- */
static void gb_i_rlc_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rlc_set_flags(gb, &cpu->a);
}

static void gb_i_rlc_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rlc_set_flags(gb, &cpu->b);
}

static void gb_i_rlc_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rlc_set_flags(gb, &cpu->c);
}

static void gb_i_rlc_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rlc_set_flags(gb, &cpu->d);
}

static void gb_i_rlc_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rlc_set_flags(gb, &cpu->e);
}

static void gb_i_rlc_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rlc_set_flags(gb, &cpu->h);
}

static void gb_i_rlc_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rlc_set_flags(gb, &cpu->l);
}

static void gb_i_rlc_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_rlc_set_flags(gb, &v);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_rrc_set_flags - Rotação circular para a direita e atualiza as flags.
 *
 * O bit 0 vai para C e também para o bit 7 (circular).
 * Equivalente a: C = *v & 1; *v = (*v >> 1) | (C << 7)
 * Flags: Z, N=0, H=0, C = bit 0 original.
 */
static void gb_cpu_rrc_set_flags(struct gb *gb, uint8_t *v)
{
     struct gb_cpu *cpu = &gb->cpu;
     uint8_t c = *v & 1;

     *v = (*v >> 1) | (c << 7);

     cpu->f_z = (*v == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = c;
}

/* --- RRC r : rotação circular para a direita (CB 0x08–0x0F) --- */
static void gb_i_rrc_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rrc_set_flags(gb, &cpu->a);
}

static void gb_i_rrc_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rrc_set_flags(gb, &cpu->b);
}

static void gb_i_rrc_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rrc_set_flags(gb, &cpu->c);
}

static void gb_i_rrc_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rrc_set_flags(gb, &cpu->d);
}

static void gb_i_rrc_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rrc_set_flags(gb, &cpu->e);
}

static void gb_i_rrc_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rrc_set_flags(gb, &cpu->h);
}

static void gb_i_rrc_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rrc_set_flags(gb, &cpu->l);
}

static void gb_i_rrc_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_rrc_set_flags(gb, &v);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_rl_set_flags - Rotação para a esquerda através do carry e atualiza as flags.
 *
 * O bit 7 vai para C e o C antigo entra pelo bit 0.
 * Equivalente a: new_C = *v >> 7; *v = (*v << 1) | old_C; C = new_C
 * Flags: Z, N=0, H=0, C = bit 7 original.
 */
static void gb_cpu_rl_set_flags(struct gb *gb, uint8_t *v)
{
     struct gb_cpu *cpu = &gb->cpu;
     bool new_c = *v >> 7;

     *v = (*v << 1) | (uint8_t)cpu->f_c;

     cpu->f_z = (*v == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = new_c;
}

/* --- RL r : rotação para a esquerda através do carry (CB 0x10–0x17) --- */
static void gb_i_rl_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rl_set_flags(gb, &cpu->a);
}

static void gb_i_rl_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rl_set_flags(gb, &cpu->b);
}

static void gb_i_rl_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rl_set_flags(gb, &cpu->c);
}

static void gb_i_rl_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rl_set_flags(gb, &cpu->d);
}

static void gb_i_rl_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rl_set_flags(gb, &cpu->e);
}

static void gb_i_rl_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rl_set_flags(gb, &cpu->h);
}

static void gb_i_rl_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rl_set_flags(gb, &cpu->l);
}

static void gb_i_rl_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_rl_set_flags(gb, &v);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_rr_set_flags - Rotação para a direita através do carry e atualiza as flags.
 *
 * O bit 0 vai para C e o C antigo entra pelo bit 7.
 * Equivalente a: new_C = *v & 1; *v = (*v >> 1) | (old_C << 7); C = new_C
 * Flags: Z, N=0, H=0, C = bit 0 original.
 */
static void gb_cpu_rr_set_flags(struct gb *gb, uint8_t *v)
{
     struct gb_cpu *cpu = &gb->cpu;
     bool new_c = *v & 1;
     uint8_t old_c = cpu->f_c;

     *v = (*v >> 1) | (old_c << 7);

     cpu->f_z = (*v == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = new_c;
}

/* --- RR r : rotação para a direita através do carry (CB 0x18–0x1F) --- */
static void gb_i_rr_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rr_set_flags(gb, &cpu->a);
}

static void gb_i_rr_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rr_set_flags(gb, &cpu->b);
}

static void gb_i_rr_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rr_set_flags(gb, &cpu->c);
}

static void gb_i_rr_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rr_set_flags(gb, &cpu->d);
}

static void gb_i_rr_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rr_set_flags(gb, &cpu->e);
}

static void gb_i_rr_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rr_set_flags(gb, &cpu->h);
}

static void gb_i_rr_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_rr_set_flags(gb, &cpu->l);
}

static void gb_i_rr_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_rr_set_flags(gb, &v);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_sla_set_flags - Deslocamento aritmético para a esquerda e atualiza as flags.
 *
 * O bit 7 vai para C e o bit 0 recebe 0 (não circular).
 * Equivalente a: C = *v >> 7; *v = *v << 1  (bit 0 = 0)
 * Flags: Z, N=0, H=0, C = bit 7 original.
 */
static void gb_cpu_sla_set_flags(struct gb *gb, uint8_t *v)
{
     struct gb_cpu *cpu = &gb->cpu;
     bool c = *v >> 7;

     *v = *v << 1;

     cpu->f_z = (*v == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = c;
}

/* --- SLA r : deslocamento aritmético para a esquerda (CB 0x20–0x27) --- */
static void gb_i_sla_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sla_set_flags(gb, &cpu->a);
}

static void gb_i_sla_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sla_set_flags(gb, &cpu->b);
}

static void gb_i_sla_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sla_set_flags(gb, &cpu->c);
}

static void gb_i_sla_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sla_set_flags(gb, &cpu->d);
}

static void gb_i_sla_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sla_set_flags(gb, &cpu->e);
}

static void gb_i_sla_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sla_set_flags(gb, &cpu->h);
}

static void gb_i_sla_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sla_set_flags(gb, &cpu->l);
}

static void gb_i_sla_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_sla_set_flags(gb, &v);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_sra_set_flags - Deslocamento aritmético para a direita e atualiza as flags.
 *
 * O bit 0 vai para C e o bit 7 é preservado (extensão de sinal — para divisão por 2 com sinal).
 * Equivalente a: C = *v & 1; *v = (*v >> 1) | (*v & 0x80)
 * Flags: Z, N=0, H=0, C = bit 0 original.
 */
static void gb_cpu_sra_set_flags(struct gb *gb, uint8_t *v)
{
     struct gb_cpu *cpu = &gb->cpu;
     bool c = *v & 1;

     *v = (*v >> 1) | (*v & 0x80);

     cpu->f_z = (*v == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = c;
}

/* --- SRA r : deslocamento aritmético para a direita com extensão de sinal (CB 0x28–0x2F) --- */
static void gb_i_sra_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sra_set_flags(gb, &cpu->a);
}

static void gb_i_sra_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sra_set_flags(gb, &cpu->b);
}

static void gb_i_sra_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sra_set_flags(gb, &cpu->c);
}

static void gb_i_sra_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sra_set_flags(gb, &cpu->d);
}

static void gb_i_sra_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sra_set_flags(gb, &cpu->e);
}

static void gb_i_sra_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sra_set_flags(gb, &cpu->h);
}

static void gb_i_sra_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_sra_set_flags(gb, &cpu->l);
}

static void gb_i_sra_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_sra_set_flags(gb, &v);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_swap_set_flags - Troca os nibbles alto e baixo de um byte e atualiza as flags.
 *
 * Realiza: *v = ((*v << 4) | (*v >> 4)) & 0xFF
 * O nibble baixo (bits 3–0) vai para os bits 7–4 e vice-versa.
 *
 * Flags afetadas:
 *   Z = 1 se o resultado for zero
 *   N = 0, H = 0, C = 0 (sempre resetados)
 *
 * Usada por todas as variantes de SWAP r e SWAP (HL) (mapa CB, opcodes 0x30–0x37).
 */
static void gb_cpu_swap_set_flags(struct gb *gb, uint8_t *v)
{
     struct gb_cpu *cpu = &gb->cpu;

     *v = ((*v << 4) | (*v >> 4)) & 0xff;

     cpu->f_z = (*v == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = false;
}

/* --- SWAP r / SWAP (HL) : troca os nibbles de um registrador ou byte na memória (CB 0x30–0x37) --- */
static void gb_i_swap_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_swap_set_flags(gb, &cpu->a);
}

static void gb_i_swap_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_swap_set_flags(gb, &cpu->b);
}

static void gb_i_swap_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_swap_set_flags(gb, &cpu->c);
}

static void gb_i_swap_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_swap_set_flags(gb, &cpu->d);
}

static void gb_i_swap_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_swap_set_flags(gb, &cpu->e);
}

static void gb_i_swap_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_swap_set_flags(gb, &cpu->h);
}

static void gb_i_swap_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_swap_set_flags(gb, &cpu->l);
}

/* gb_i_swap_mhl - Instrução SWAP (HL) (CB 0x36): troca os nibbles do byte apontado por HL.
 * Lê o byte em mem[HL], aplica o swap e reescreve no mesmo endereço. */
static void gb_i_swap_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_swap_set_flags(gb, &v);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_srl_set_flags - Deslocamento lógico para a direita e atualiza as flags.
 *
 * O bit 0 vai para C e o bit 7 recebe 0 (sem extensão de sinal — para divisão unsigned por 2).
 * Equivalente a: C = *v & 1; *v = *v >> 1  (bit 7 = 0)
 * Flags: Z, N=0, H=0, C = bit 0 original.
 */
static void gb_cpu_srl_set_flags(struct gb *gb, uint8_t *v)
{
     struct gb_cpu *cpu = &gb->cpu;
     bool c = *v & 1;

     *v = *v >> 1;

     cpu->f_z = (*v == 0);
     cpu->f_n = false;
     cpu->f_h = false;
     cpu->f_c = c;
}

/* --- SRL r : deslocamento lógico para a direita (CB 0x38–0x3F) --- */
static void gb_i_srl_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_srl_set_flags(gb, &cpu->a);
}

static void gb_i_srl_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_srl_set_flags(gb, &cpu->b);
}

static void gb_i_srl_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_srl_set_flags(gb, &cpu->c);
}

static void gb_i_srl_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_srl_set_flags(gb, &cpu->d);
}

static void gb_i_srl_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_srl_set_flags(gb, &cpu->e);
}

static void gb_i_srl_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_srl_set_flags(gb, &cpu->h);
}

static void gb_i_srl_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_srl_set_flags(gb, &cpu->l);
}

static void gb_i_srl_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_srl_set_flags(gb, &v);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_bit_set_flags - Testa um bit de um byte e atualiza as flags.
 *
 * Testa o bit 'bit' de *v: se o bit for 0, Z=1; se for 1, Z=0.
 * O valor de *v não é modificado.
 * Flags afetadas:
 *   Z = 1 se o bit testado for 0
 *   N = 0, H = 1 (sempre)
 *   C = não alterado
 */
static void gb_cpu_bit_set_flags(struct gb *gb, uint8_t *v, unsigned bit)
{
     struct gb_cpu *cpu = &gb->cpu;
     bool set = *v & (1U << bit);

     cpu->f_z = !set;
     cpu->f_n = false;
     cpu->f_h = true;
}

/* --- BIT n, r : testa bit n do registrador (Z = !bit, N=0, H=1; CB 0x40–0x7F) --- */
static void gb_i_bit_0_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->a, 0);
}

static void gb_i_bit_0_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->b, 0);
}

static void gb_i_bit_0_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->c, 0);
}

static void gb_i_bit_0_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->d, 0);
}

static void gb_i_bit_0_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->e, 0);
}

static void gb_i_bit_0_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->h, 0);
}

static void gb_i_bit_0_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->l, 0);
}

static void gb_i_bit_0_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_bit_set_flags(gb, &v, 0);
}

static void gb_i_bit_1_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->a, 1);
}

static void gb_i_bit_1_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->b, 1);
}

static void gb_i_bit_1_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->c, 1);
}

static void gb_i_bit_1_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->d, 1);
}

static void gb_i_bit_1_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->e, 1);
}

static void gb_i_bit_1_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->h, 1);
}

static void gb_i_bit_1_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->l, 1);
}

static void gb_i_bit_1_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_bit_set_flags(gb, &v, 1);
}

static void gb_i_bit_2_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->a, 2);
}

static void gb_i_bit_2_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->b, 2);
}

static void gb_i_bit_2_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->c, 2);
}

static void gb_i_bit_2_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->d, 2);
}

static void gb_i_bit_2_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->e, 2);
}

static void gb_i_bit_2_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->h, 2);
}

static void gb_i_bit_2_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->l, 2);
}

static void gb_i_bit_2_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_bit_set_flags(gb, &v, 2);
}

static void gb_i_bit_3_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->a, 3);
}

static void gb_i_bit_3_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->b, 3);
}

static void gb_i_bit_3_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->c, 3);
}

static void gb_i_bit_3_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->d, 3);
}

static void gb_i_bit_3_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->e, 3);
}

static void gb_i_bit_3_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->h, 3);
}

static void gb_i_bit_3_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->l, 3);
}

static void gb_i_bit_3_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_bit_set_flags(gb, &v, 3);
}

static void gb_i_bit_4_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->a, 4);
}

static void gb_i_bit_4_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->b, 4);
}

static void gb_i_bit_4_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->c, 4);
}

static void gb_i_bit_4_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->d, 4);
}

static void gb_i_bit_4_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->e, 4);
}

static void gb_i_bit_4_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->h, 4);
}

static void gb_i_bit_4_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->l, 4);
}

static void gb_i_bit_4_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_bit_set_flags(gb, &v, 4);
}

static void gb_i_bit_5_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->a, 5);
}

static void gb_i_bit_5_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->b, 5);
}

static void gb_i_bit_5_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->c, 5);
}

static void gb_i_bit_5_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->d, 5);
}

static void gb_i_bit_5_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->e, 5);
}

static void gb_i_bit_5_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->h, 5);
}

static void gb_i_bit_5_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->l, 5);
}

static void gb_i_bit_5_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_bit_set_flags(gb, &v, 5);
}

static void gb_i_bit_6_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->a, 6);
}

static void gb_i_bit_6_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->b, 6);
}

static void gb_i_bit_6_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->c, 6);
}

static void gb_i_bit_6_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->d, 6);
}

static void gb_i_bit_6_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->e, 6);
}

static void gb_i_bit_6_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->h, 6);
}

static void gb_i_bit_6_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->l, 6);
}

static void gb_i_bit_6_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_bit_set_flags(gb, &v, 6);
}

static void gb_i_bit_7_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->a, 7);
}

static void gb_i_bit_7_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->b, 7);
}

static void gb_i_bit_7_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->c, 7);
}

static void gb_i_bit_7_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->d, 7);
}

static void gb_i_bit_7_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->e, 7);
}

static void gb_i_bit_7_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->h, 7);
}

static void gb_i_bit_7_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_bit_set_flags(gb, &cpu->l, 7);
}

static void gb_i_bit_7_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_bit_set_flags(gb, &v, 7);
}

/*
 * gb_cpu_res - Limpa (reseta) o bit 'bit' de *v.
 *
 * Equivalente a: *v &= ~(1 << bit). Nenhuma flag é afetada.
 * Usada por todas as variantes de RES n, r (CB 0x80–0xBF).
 */
static void gb_cpu_res(struct gb *gb, uint8_t *v, unsigned bit)
{
     *v = *v & ~(1U << bit);
}

/* --- RES n, r : limpa o bit n do registrador (nenhuma flag afetada; CB 0x80–0xBF) --- */
static void gb_i_res_0_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->a, 0);
}

static void gb_i_res_0_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->b, 0);
}

static void gb_i_res_0_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->c, 0);
}

static void gb_i_res_0_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->d, 0);
}

static void gb_i_res_0_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->e, 0);
}

static void gb_i_res_0_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->h, 0);
}

static void gb_i_res_0_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->l, 0);
}

static void gb_i_res_0_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_res(gb, &v, 0);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_res_1_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->a, 1);
}

static void gb_i_res_1_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->b, 1);
}

static void gb_i_res_1_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->c, 1);
}

static void gb_i_res_1_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->d, 1);
}

static void gb_i_res_1_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->e, 1);
}

static void gb_i_res_1_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->h, 1);
}

static void gb_i_res_1_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->l, 1);
}

static void gb_i_res_1_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_res(gb, &v, 1);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_res_2_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->a, 2);
}

static void gb_i_res_2_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->b, 2);
}

static void gb_i_res_2_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->c, 2);
}

static void gb_i_res_2_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->d, 2);
}

static void gb_i_res_2_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->e, 2);
}

static void gb_i_res_2_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->h, 2);
}

static void gb_i_res_2_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->l, 2);
}

static void gb_i_res_2_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_res(gb, &v, 2);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_res_3_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->a, 3);
}

static void gb_i_res_3_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->b, 3);
}

static void gb_i_res_3_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->c, 3);
}

static void gb_i_res_3_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->d, 3);
}

static void gb_i_res_3_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->e, 3);
}

static void gb_i_res_3_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->h, 3);
}

static void gb_i_res_3_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->l, 3);
}

static void gb_i_res_3_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_res(gb, &v, 3);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_res_4_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->a, 4);
}

static void gb_i_res_4_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->b, 4);
}

static void gb_i_res_4_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->c, 4);
}

static void gb_i_res_4_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->d, 4);
}

static void gb_i_res_4_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->e, 4);
}

static void gb_i_res_4_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->h, 4);
}

static void gb_i_res_4_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->l, 4);
}

static void gb_i_res_4_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_res(gb, &v, 4);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_res_5_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->a, 5);
}

static void gb_i_res_5_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->b, 5);
}

static void gb_i_res_5_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->c, 5);
}

static void gb_i_res_5_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->d, 5);
}

static void gb_i_res_5_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->e, 5);
}

static void gb_i_res_5_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->h, 5);
}

static void gb_i_res_5_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->l, 5);
}

static void gb_i_res_5_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_res(gb, &v, 5);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_res_6_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->a, 6);
}

static void gb_i_res_6_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->b, 6);
}

static void gb_i_res_6_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->c, 6);
}

static void gb_i_res_6_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->d, 6);
}

static void gb_i_res_6_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->e, 6);
}

static void gb_i_res_6_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->h, 6);
}

static void gb_i_res_6_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->l, 6);
}

static void gb_i_res_6_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_res(gb, &v, 6);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_res_7_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->a, 7);
}

static void gb_i_res_7_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->b, 7);
}

static void gb_i_res_7_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->c, 7);
}

static void gb_i_res_7_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->d, 7);
}

static void gb_i_res_7_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->e, 7);
}

static void gb_i_res_7_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->h, 7);
}

static void gb_i_res_7_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_res(gb, &cpu->l, 7);
}

static void gb_i_res_7_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_res(gb, &v, 7);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_cpu_set - Seta o bit 'bit' de *v.
 *
 * Equivalente a: *v |= (1 << bit). Nenhuma flag é afetada.
 * Usada por todas as variantes de SET n, r (CB 0xC0–0xFF).
 */
static void gb_cpu_set(struct gb *gb, uint8_t *v, unsigned bit)
{
     *v = *v | (1U << bit);
}

/* --- SET n, r : seta o bit n do registrador (nenhuma flag afetada; CB 0xC0–0xFF) --- */
static void gb_i_set_0_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->a, 0);
}

static void gb_i_set_0_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->b, 0);
}

static void gb_i_set_0_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->c, 0);
}

static void gb_i_set_0_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->d, 0);
}

static void gb_i_set_0_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->e, 0);
}

static void gb_i_set_0_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->h, 0);
}

static void gb_i_set_0_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->l, 0);
}

static void gb_i_set_0_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_set(gb, &v, 0);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_set_1_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->a, 1);
}

static void gb_i_set_1_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->b, 1);
}

static void gb_i_set_1_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->c, 1);
}

static void gb_i_set_1_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->d, 1);
}

static void gb_i_set_1_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->e, 1);
}

static void gb_i_set_1_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->h, 1);
}

static void gb_i_set_1_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->l, 1);
}

static void gb_i_set_1_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_set(gb, &v, 1);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_set_2_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->a, 2);
}

static void gb_i_set_2_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->b, 2);
}

static void gb_i_set_2_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->c, 2);
}

static void gb_i_set_2_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->d, 2);
}

static void gb_i_set_2_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->e, 2);
}

static void gb_i_set_2_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->h, 2);
}

static void gb_i_set_2_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->l, 2);
}

static void gb_i_set_2_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_set(gb, &v, 2);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_set_3_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->a, 3);
}

static void gb_i_set_3_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->b, 3);
}

static void gb_i_set_3_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->c, 3);
}

static void gb_i_set_3_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->d, 3);
}

static void gb_i_set_3_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->e, 3);
}

static void gb_i_set_3_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->h, 3);
}

static void gb_i_set_3_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->l, 3);
}

static void gb_i_set_3_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_set(gb, &v, 3);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_set_4_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->a, 4);
}

static void gb_i_set_4_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->b, 4);
}

static void gb_i_set_4_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->c, 4);
}

static void gb_i_set_4_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->d, 4);
}

static void gb_i_set_4_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->e, 4);
}

static void gb_i_set_4_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->h, 4);
}

static void gb_i_set_4_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->l, 4);
}

static void gb_i_set_4_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_set(gb, &v, 4);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_set_5_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->a, 5);
}

static void gb_i_set_5_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->b, 5);
}

static void gb_i_set_5_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->c, 5);
}

static void gb_i_set_5_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->d, 5);
}

static void gb_i_set_5_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->e, 5);
}

static void gb_i_set_5_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->h, 5);
}

static void gb_i_set_5_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->l, 5);
}

static void gb_i_set_5_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_set(gb, &v, 5);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_set_6_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->a, 6);
}

static void gb_i_set_6_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->b, 6);
}

static void gb_i_set_6_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->c, 6);
}

static void gb_i_set_6_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->d, 6);
}

static void gb_i_set_6_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->e, 6);
}

static void gb_i_set_6_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->h, 6);
}

static void gb_i_set_6_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->l, 6);
}

static void gb_i_set_6_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_set(gb, &v, 6);

     gb_cpu_writeb(gb, hl, v);
}

static void gb_i_set_7_a(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->a, 7);
}

static void gb_i_set_7_b(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->b, 7);
}

static void gb_i_set_7_c(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->c, 7);
}

static void gb_i_set_7_d(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->d, 7);
}

static void gb_i_set_7_e(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->e, 7);
}

static void gb_i_set_7_h(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->h, 7);
}

static void gb_i_set_7_l(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     gb_cpu_set(gb, &cpu->l, 7);
}

static void gb_i_set_7_mhl(struct gb *gb)
{
     uint16_t hl = gb_cpu_hl(gb);
     uint8_t v;

     v = gb_cpu_readb(gb, hl);

     gb_cpu_set(gb, &v, 7);

     gb_cpu_writeb(gb, hl, v);
}

/*
 * gb_instructions_cb - Tabela de despacho para o mapa estendido de instruções (prefixo 0xCB).
 *
 * Quando gb_cpu_run_instruction despacha para gb_i_op_cb (opcode 0xCB), esta função
 * lê o segundo byte e o usa como índice nesta tabela secundária de 256 entradas.
 * O mapa CB implementa operações de bit: rotações (RLC, RRC, RL, RR, SLA, SRA, SRL),
 * troca de nibbles (SWAP) e testes/set/reset de bits individuais (BIT, SET, RES).
 *
 */
static gb_instruction_f gb_instructions_cb[0x100] = {
    // 0x00 - RLC r : rotação circular para a esquerda (bit 7 → C e bit 0)
    gb_i_rlc_b,   /* 0x00: RLC B */
    gb_i_rlc_c,   /* 0x01: RLC C */
    gb_i_rlc_d,   /* 0x02: RLC D */
    gb_i_rlc_e,   /* 0x03: RLC E */
    gb_i_rlc_h,   /* 0x04: RLC H */
    gb_i_rlc_l,   /* 0x05: RLC L */
    gb_i_rlc_mhl, /* 0x06: RLC (HL) */
    gb_i_rlc_a,   /* 0x07: RLC A */
    // 0x08 - RRC r : rotação circular para a direita (bit 0 → C e bit 7)
    gb_i_rrc_b,   /* 0x08: RRC B */
    gb_i_rrc_c,   /* 0x09: RRC C */
    gb_i_rrc_d,   /* 0x0A: RRC D */
    gb_i_rrc_e,   /* 0x0B: RRC E */
    gb_i_rrc_h,   /* 0x0C: RRC H */
    gb_i_rrc_l,   /* 0x0D: RRC L */
    gb_i_rrc_mhl, /* 0x0E: RRC (HL) */
    gb_i_rrc_a,   /* 0x0F: RRC A */
    // 0x10 - RL r : rotação para a esquerda através do carry (bit 7 → C; old C → bit 0)
    gb_i_rl_b,   /* 0x10: RL B */
    gb_i_rl_c,   /* 0x11: RL C */
    gb_i_rl_d,   /* 0x12: RL D */
    gb_i_rl_e,   /* 0x13: RL E */
    gb_i_rl_h,   /* 0x14: RL H */
    gb_i_rl_l,   /* 0x15: RL L */
    gb_i_rl_mhl, /* 0x16: RL (HL) */
    gb_i_rl_a,   /* 0x17: RL A */
    // 0x18 - RR r : rotação para a direita através do carry (bit 0 → C; old C → bit 7)
    gb_i_rr_b,   /* 0x18: RR B */
    gb_i_rr_c,   /* 0x19: RR C */
    gb_i_rr_d,   /* 0x1A: RR D */
    gb_i_rr_e,   /* 0x1B: RR E */
    gb_i_rr_h,   /* 0x1C: RR H */
    gb_i_rr_l,   /* 0x1D: RR L */
    gb_i_rr_mhl, /* 0x1E: RR (HL) */
    gb_i_rr_a,   /* 0x1F: RR A */
    // 0x20 - SLA r : deslocamento aritmético para a esquerda (bit 7 → C; bit 0 = 0)
    gb_i_sla_b,   /* 0x20: SLA B */
    gb_i_sla_c,   /* 0x21: SLA C */
    gb_i_sla_d,   /* 0x22: SLA D */
    gb_i_sla_e,   /* 0x23: SLA E */
    gb_i_sla_h,   /* 0x24: SLA H */
    gb_i_sla_l,   /* 0x25: SLA L */
    gb_i_sla_mhl, /* 0x26: SLA (HL) */
    gb_i_sla_a,   /* 0x27: SLA A */
    // 0x28 - SRA r : deslocamento aritmético para a direita (bit 0 → C; bit 7 preservado)
    gb_i_sra_b,   /* 0x28: SRA B */
    gb_i_sra_c,   /* 0x29: SRA C */
    gb_i_sra_d,   /* 0x2A: SRA D */
    gb_i_sra_e,   /* 0x2B: SRA E */
    gb_i_sra_h,   /* 0x2C: SRA H */
    gb_i_sra_l,   /* 0x2D: SRA L */
    gb_i_sra_mhl, /* 0x2E: SRA (HL) */
    gb_i_sra_a,   /* 0x2F: SRA A */
    // 0x30 - SWAP r : troca nibble alto e nibble baixo
    gb_i_swap_b,   /* 0x30: SWAP B */
    gb_i_swap_c,   /* 0x31: SWAP C */
    gb_i_swap_d,   /* 0x32: SWAP D */
    gb_i_swap_e,   /* 0x33: SWAP E */
    gb_i_swap_h,   /* 0x34: SWAP H */
    gb_i_swap_l,   /* 0x35: SWAP L */
    gb_i_swap_mhl, /* 0x36: SWAP (HL) */
    gb_i_swap_a,   /* 0x37: SWAP A */
    // 0x38 - SRL r : deslocamento lógico para a direita (bit 0 → C; bit 7 = 0)
    gb_i_srl_b,   /* 0x38: SRL B */
    gb_i_srl_c,   /* 0x39: SRL C */
    gb_i_srl_d,   /* 0x3A: SRL D */
    gb_i_srl_e,   /* 0x3B: SRL E */
    gb_i_srl_h,   /* 0x3C: SRL H */
    gb_i_srl_l,   /* 0x3D: SRL L */
    gb_i_srl_mhl, /* 0x3E: SRL (HL) */
    gb_i_srl_a,   /* 0x3F: SRL A */
    // 0x40 - BIT 0, r : testa o bit 0 do registrador (Z = !bit)
    gb_i_bit_0_b,   /* 0x40: BIT 0, B */
    gb_i_bit_0_c,   /* 0x41: BIT 0, C */
    gb_i_bit_0_d,   /* 0x42: BIT 0, D */
    gb_i_bit_0_e,   /* 0x43: BIT 0, E */
    gb_i_bit_0_h,   /* 0x44: BIT 0, H */
    gb_i_bit_0_l,   /* 0x45: BIT 0, L */
    gb_i_bit_0_mhl, /* 0x46: BIT 0, (HL) */
    gb_i_bit_0_a,   /* 0x47: BIT 0, A */
    // 0x48 - BIT 1, r
    gb_i_bit_1_b,   /* 0x48: BIT 1, B */
    gb_i_bit_1_c,   /* 0x49: BIT 1, C */
    gb_i_bit_1_d,   /* 0x4A: BIT 1, D */
    gb_i_bit_1_e,   /* 0x4B: BIT 1, E */
    gb_i_bit_1_h,   /* 0x4C: BIT 1, H */
    gb_i_bit_1_l,   /* 0x4D: BIT 1, L */
    gb_i_bit_1_mhl, /* 0x4E: BIT 1, (HL) */
    gb_i_bit_1_a,   /* 0x4F: BIT 1, A */
    // 0x50 - BIT 2, r
    gb_i_bit_2_b,   /* 0x50: BIT 2, B */
    gb_i_bit_2_c,   /* 0x51: BIT 2, C */
    gb_i_bit_2_d,   /* 0x52: BIT 2, D */
    gb_i_bit_2_e,   /* 0x53: BIT 2, E */
    gb_i_bit_2_h,   /* 0x54: BIT 2, H */
    gb_i_bit_2_l,   /* 0x55: BIT 2, L */
    gb_i_bit_2_mhl, /* 0x56: BIT 2, (HL) */
    gb_i_bit_2_a,   /* 0x57: BIT 2, A */
    // 0x58 - BIT 3, r
    gb_i_bit_3_b,   /* 0x58: BIT 3, B */
    gb_i_bit_3_c,   /* 0x59: BIT 3, C */
    gb_i_bit_3_d,   /* 0x5A: BIT 3, D */
    gb_i_bit_3_e,   /* 0x5B: BIT 3, E */
    gb_i_bit_3_h,   /* 0x5C: BIT 3, H */
    gb_i_bit_3_l,   /* 0x5D: BIT 3, L */
    gb_i_bit_3_mhl, /* 0x5E: BIT 3, (HL) */
    gb_i_bit_3_a,   /* 0x5F: BIT 3, A */
    // 0x60 - BIT 4, r
    gb_i_bit_4_b,   /* 0x60: BIT 4, B */
    gb_i_bit_4_c,   /* 0x61: BIT 4, C */
    gb_i_bit_4_d,   /* 0x62: BIT 4, D */
    gb_i_bit_4_e,   /* 0x63: BIT 4, E */
    gb_i_bit_4_h,   /* 0x64: BIT 4, H */
    gb_i_bit_4_l,   /* 0x65: BIT 4, L */
    gb_i_bit_4_mhl, /* 0x66: BIT 4, (HL) */
    gb_i_bit_4_a,   /* 0x67: BIT 4, A */
    // 0x68 - BIT 5, r
    gb_i_bit_5_b,   /* 0x68: BIT 5, B */
    gb_i_bit_5_c,   /* 0x69: BIT 5, C */
    gb_i_bit_5_d,   /* 0x6A: BIT 5, D */
    gb_i_bit_5_e,   /* 0x6B: BIT 5, E */
    gb_i_bit_5_h,   /* 0x6C: BIT 5, H */
    gb_i_bit_5_l,   /* 0x6D: BIT 5, L */
    gb_i_bit_5_mhl, /* 0x6E: BIT 5, (HL) */
    gb_i_bit_5_a,   /* 0x6F: BIT 5, A */
    // 0x70 - BIT 6, r
    gb_i_bit_6_b,   /* 0x70: BIT 6, B */
    gb_i_bit_6_c,   /* 0x71: BIT 6, C */
    gb_i_bit_6_d,   /* 0x72: BIT 6, D */
    gb_i_bit_6_e,   /* 0x73: BIT 6, E */
    gb_i_bit_6_h,   /* 0x74: BIT 6, H */
    gb_i_bit_6_l,   /* 0x75: BIT 6, L */
    gb_i_bit_6_mhl, /* 0x76: BIT 6, (HL) */
    gb_i_bit_6_a,   /* 0x77: BIT 6, A */
    // 0x78 - BIT 7, r
    gb_i_bit_7_b,   /* 0x78: BIT 7, B */
    gb_i_bit_7_c,   /* 0x79: BIT 7, C */
    gb_i_bit_7_d,   /* 0x7A: BIT 7, D */
    gb_i_bit_7_e,   /* 0x7B: BIT 7, E */
    gb_i_bit_7_h,   /* 0x7C: BIT 7, H */
    gb_i_bit_7_l,   /* 0x7D: BIT 7, L */
    gb_i_bit_7_mhl, /* 0x7E: BIT 7, (HL) */
    gb_i_bit_7_a,   /* 0x7F: BIT 7, A */
    // 0x80 - RES 0, r : limpa o bit 0 do registrador
    gb_i_res_0_b,   /* 0x80: RES 0, B */
    gb_i_res_0_c,   /* 0x81: RES 0, C */
    gb_i_res_0_d,   /* 0x82: RES 0, D */
    gb_i_res_0_e,   /* 0x83: RES 0, E */
    gb_i_res_0_h,   /* 0x84: RES 0, H */
    gb_i_res_0_l,   /* 0x85: RES 0, L */
    gb_i_res_0_mhl, /* 0x86: RES 0, (HL) */
    gb_i_res_0_a,   /* 0x87: RES 0, A */
    // 0x88 - RES 1, r
    gb_i_res_1_b,   /* 0x88: RES 1, B */
    gb_i_res_1_c,   /* 0x89: RES 1, C */
    gb_i_res_1_d,   /* 0x8A: RES 1, D */
    gb_i_res_1_e,   /* 0x8B: RES 1, E */
    gb_i_res_1_h,   /* 0x8C: RES 1, H */
    gb_i_res_1_l,   /* 0x8D: RES 1, L */
    gb_i_res_1_mhl, /* 0x8E: RES 1, (HL) */
    gb_i_res_1_a,   /* 0x8F: RES 1, A */
    // 0x90 - RES 2, r
    gb_i_res_2_b,   /* 0x90: RES 2, B */
    gb_i_res_2_c,   /* 0x91: RES 2, C */
    gb_i_res_2_d,   /* 0x92: RES 2, D */
    gb_i_res_2_e,   /* 0x93: RES 2, E */
    gb_i_res_2_h,   /* 0x94: RES 2, H */
    gb_i_res_2_l,   /* 0x95: RES 2, L */
    gb_i_res_2_mhl, /* 0x96: RES 2, (HL) */
    gb_i_res_2_a,   /* 0x97: RES 2, A */
    // 0x98 - RES 3, r
    gb_i_res_3_b,   /* 0x98: RES 3, B */
    gb_i_res_3_c,   /* 0x99: RES 3, C */
    gb_i_res_3_d,   /* 0x9A: RES 3, D */
    gb_i_res_3_e,   /* 0x9B: RES 3, E */
    gb_i_res_3_h,   /* 0x9C: RES 3, H */
    gb_i_res_3_l,   /* 0x9D: RES 3, L */
    gb_i_res_3_mhl, /* 0x9E: RES 3, (HL) */
    gb_i_res_3_a,   /* 0x9F: RES 3, A */
    // 0xa0 - RES 4, r
    gb_i_res_4_b,   /* 0xA0: RES 4, B */
    gb_i_res_4_c,   /* 0xA1: RES 4, C */
    gb_i_res_4_d,   /* 0xA2: RES 4, D */
    gb_i_res_4_e,   /* 0xA3: RES 4, E */
    gb_i_res_4_h,   /* 0xA4: RES 4, H */
    gb_i_res_4_l,   /* 0xA5: RES 4, L */
    gb_i_res_4_mhl, /* 0xA6: RES 4, (HL) */
    gb_i_res_4_a,   /* 0xA7: RES 4, A */
    // 0xa8 - RES 5, r
    gb_i_res_5_b,   /* 0xA8: RES 5, B */
    gb_i_res_5_c,   /* 0xA9: RES 5, C */
    gb_i_res_5_d,   /* 0xAA: RES 5, D */
    gb_i_res_5_e,   /* 0xAB: RES 5, E */
    gb_i_res_5_h,   /* 0xAC: RES 5, H */
    gb_i_res_5_l,   /* 0xAD: RES 5, L */
    gb_i_res_5_mhl, /* 0xAE: RES 5, (HL) */
    gb_i_res_5_a,   /* 0xAF: RES 5, A */
    // 0xb0 - RES 6, r
    gb_i_res_6_b,   /* 0xB0: RES 6, B */
    gb_i_res_6_c,   /* 0xB1: RES 6, C */
    gb_i_res_6_d,   /* 0xB2: RES 6, D */
    gb_i_res_6_e,   /* 0xB3: RES 6, E */
    gb_i_res_6_h,   /* 0xB4: RES 6, H */
    gb_i_res_6_l,   /* 0xB5: RES 6, L */
    gb_i_res_6_mhl, /* 0xB6: RES 6, (HL) */
    gb_i_res_6_a,   /* 0xB7: RES 6, A */
    // 0xb8 - RES 7, r
    gb_i_res_7_b,   /* 0xB8: RES 7, B */
    gb_i_res_7_c,   /* 0xB9: RES 7, C */
    gb_i_res_7_d,   /* 0xBA: RES 7, D */
    gb_i_res_7_e,   /* 0xBB: RES 7, E */
    gb_i_res_7_h,   /* 0xBC: RES 7, H */
    gb_i_res_7_l,   /* 0xBD: RES 7, L */
    gb_i_res_7_mhl, /* 0xBE: RES 7, (HL) */
    gb_i_res_7_a,   /* 0xBF: RES 7, A */
    // 0xc0 - SET 0, r : seta o bit 0 do registrador
    gb_i_set_0_b,   /* 0xC0: SET 0, B */
    gb_i_set_0_c,   /* 0xC1: SET 0, C */
    gb_i_set_0_d,   /* 0xC2: SET 0, D */
    gb_i_set_0_e,   /* 0xC3: SET 0, E */
    gb_i_set_0_h,   /* 0xC4: SET 0, H */
    gb_i_set_0_l,   /* 0xC5: SET 0, L */
    gb_i_set_0_mhl, /* 0xC6: SET 0, (HL) */
    gb_i_set_0_a,   /* 0xC7: SET 0, A */
    // 0xc8 - SET 1, r
    gb_i_set_1_b,   /* 0xC8: SET 1, B */
    gb_i_set_1_c,   /* 0xC9: SET 1, C */
    gb_i_set_1_d,   /* 0xCA: SET 1, D */
    gb_i_set_1_e,   /* 0xCB: SET 1, E */
    gb_i_set_1_h,   /* 0xCC: SET 1, H */
    gb_i_set_1_l,   /* 0xCD: SET 1, L */
    gb_i_set_1_mhl, /* 0xCE: SET 1, (HL) */
    gb_i_set_1_a,   /* 0xCF: SET 1, A */
    // 0xd0 - SET 2, r
    gb_i_set_2_b,   /* 0xD0: SET 2, B */
    gb_i_set_2_c,   /* 0xD1: SET 2, C */
    gb_i_set_2_d,   /* 0xD2: SET 2, D */
    gb_i_set_2_e,   /* 0xD3: SET 2, E */
    gb_i_set_2_h,   /* 0xD4: SET 2, H */
    gb_i_set_2_l,   /* 0xD5: SET 2, L */
    gb_i_set_2_mhl, /* 0xD6: SET 2, (HL) */
    gb_i_set_2_a,   /* 0xD7: SET 2, A */
    // 0xd8 - SET 3, r
    gb_i_set_3_b,   /* 0xD8: SET 3, B */
    gb_i_set_3_c,   /* 0xD9: SET 3, C */
    gb_i_set_3_d,   /* 0xDA: SET 3, D */
    gb_i_set_3_e,   /* 0xDB: SET 3, E */
    gb_i_set_3_h,   /* 0xDC: SET 3, H */
    gb_i_set_3_l,   /* 0xDD: SET 3, L */
    gb_i_set_3_mhl, /* 0xDE: SET 3, (HL) */
    gb_i_set_3_a,   /* 0xDF: SET 3, A */
    // 0xe0 - SET 4, r
    gb_i_set_4_b,   /* 0xE0: SET 4, B */
    gb_i_set_4_c,   /* 0xE1: SET 4, C */
    gb_i_set_4_d,   /* 0xE2: SET 4, D */
    gb_i_set_4_e,   /* 0xE3: SET 4, E */
    gb_i_set_4_h,   /* 0xE4: SET 4, H */
    gb_i_set_4_l,   /* 0xE5: SET 4, L */
    gb_i_set_4_mhl, /* 0xE6: SET 4, (HL) */
    gb_i_set_4_a,   /* 0xE7: SET 4, A */
    // 0xe8 - SET 5, r
    gb_i_set_5_b,   /* 0xE8: SET 5, B */
    gb_i_set_5_c,   /* 0xE9: SET 5, C */
    gb_i_set_5_d,   /* 0xEA: SET 5, D */
    gb_i_set_5_e,   /* 0xEB: SET 5, E */
    gb_i_set_5_h,   /* 0xEC: SET 5, H */
    gb_i_set_5_l,   /* 0xED: SET 5, L */
    gb_i_set_5_mhl, /* 0xEE: SET 5, (HL) */
    gb_i_set_5_a,   /* 0xEF: SET 5, A */
    // 0xf0 - SET 6, r
    gb_i_set_6_b,   /* 0xF0: SET 6, B */
    gb_i_set_6_c,   /* 0xF1: SET 6, C */
    gb_i_set_6_d,   /* 0xF2: SET 6, D */
    gb_i_set_6_e,   /* 0xF3: SET 6, E */
    gb_i_set_6_h,   /* 0xF4: SET 6, H */
    gb_i_set_6_l,   /* 0xF5: SET 6, L */
    gb_i_set_6_mhl, /* 0xF6: SET 6, (HL) */
    gb_i_set_6_a,   /* 0xF7: SET 6, A */
    // 0xf8 - SET 7, r
    gb_i_set_7_b,   /* 0xF8: SET 7, B */
    gb_i_set_7_c,   /* 0xF9: SET 7, C */
    gb_i_set_7_d,   /* 0xFA: SET 7, D */
    gb_i_set_7_e,   /* 0xFB: SET 7, E */
    gb_i_set_7_h,   /* 0xFC: SET 7, H */
    gb_i_set_7_l,   /* 0xFD: SET 7, L */
    gb_i_set_7_mhl, /* 0xFE: SET 7, (HL) */
    gb_i_set_7_a,   /* 0xFF: SET 7, A */
};

/*
 * gb_i_op_cb - Tratador do prefixo CB (opcode 0xCB): despacha instrução estendida.
 *
 * Lê o segundo byte e usa como índice na tabela gb_instructions_cb.
 * Todas as instruções do mapa CB consomem 8 ciclos (4 do prefixo + 4 do opcode),
 * exceto as variantes (HL) que consomem 16 ciclos.
 */
static void gb_i_op_cb(struct gb *gb)
{
     /* O opcode 0xCB é um prefixo para o segundo mapa de instruções */
     uint8_t instruction = gb_cpu_next_i8(gb);

     gb_instructions_cb[instruction](gb);
}
