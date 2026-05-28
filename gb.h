#ifndef _GB_GB_H_
#define _GB_GB_H_

#include <stdint.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <semaphore.h>

struct gb;

#include "sync.h"
#include "irq.h"
#include "cpu.h"
#include "memory.h"
#include "cart.h"
#include "gpu.h"
#include "input.h"
#include "dma.h"
#include "hdma.h"
#include "timer.h"
#include "spu.h"
#include "frontend.h"
#include "debug.h"

/* Frequência do CPU DMG. O Super GameBoy roda ligeiramente mais rápido (4.295454MHz). */
#define GB_CPU_FREQ_HZ 4194304U

enum gb_hw_model
{
     GB_HW_DMG,    /* DMG original (revisões A/B/C) */
     GB_HW_DMG0,   /* DMG original revisão 0 */
     GB_HW_MGB,    /* Game Boy Pocket */
     GB_HW_SGB,    /* Super Game Boy */
     GB_HW_SGB2,   /* Super Game Boy 2 */
     GB_HW_CGB0,   /* Game Boy Color revisão 0 */
     GB_HW_CGB,    /* Game Boy Color */
};

struct gb
{
     /* Verdadeiro se estamos emulando um GBC, falso se estamos emulando um DMG */
     bool gbc;
     /* Modelo de hardware (afeta o estado dos registradores na inicialização) */
     enum gb_hw_model hw_model;

     /* Verdadeiro se uma troca de velocidade foi solicitada. Entrará em vigor
      * quando uma operação STOP for executada */
     bool speed_switch_pending;
     /* Verdadeiro se o GBC está rodando em modo de velocidade dupla */
     bool double_speed;

     /* Contador que rastreia quantos ciclos de CPU se passaram desde um
      * ponto arbitrário no tempo. Usado para sincronizar os outros dispositivos. */
     int32_t timestamp;
     /* Definido pelo frontend quando o usuário solicitou que a emulação pare */
     bool quit;

     /* Registradores de transferência serial. O callback opcional é útil para
      * testes de compatibilidade headless que reportam resultados via SB/SC. */
     uint8_t serial_data;
     uint8_t serial_control;
     void (*serial_tx)(struct gb *gb, uint8_t byte);

     struct gb_irq irq;
     struct gb_frontend frontend;
     struct gb_sync sync;
     struct gb_cpu cpu;
     struct gb_cart cart;
     struct gb_gpu gpu;
     struct gb_input input;
     struct gb_dma dma;
     struct gb_hdma hdma;
     struct gb_timer timer;
     struct gb_spu spu;
     struct gb_debug debug;
     /* RAM interna: 8KiB no DMG, 32 KiB no GBC */
     uint8_t iram[0x8000];
     /* Sempre 1 no DMG, 1-7 no GBC */
     uint8_t iram_high_bank;
     /* RAM de página zero */
     uint8_t zram[0x7f];
     /* RAM de vídeo: 8KiB no DMG, 16KiB no GBC */
     uint8_t vram[0x4000];
     /* Sempre falso no DMG */
     bool vram_high_bank;

     /* Porta de Comunicação Infravermelha do GBC (REG_RP = 0xFF56).
      * Bit 7: habilita leitura; Bit 1: saída do LED; Bit 6 (IR recebido) sempre 0. */
     uint8_t ir_port;
     /* Registradores CGB não documentados expostos também no modo compatibilidade. */
     uint8_t cgb_reg_ff72;
     uint8_t cgb_reg_ff73;
     uint8_t cgb_reg_ff75;

     /* Boot ROM opcional. NULL quando não carregada.
      * DMG: 256 bytes, mapeada em 0x0000–0x00FF.
      * CGB: 2304 bytes (0x900), mapeada em 0x0000–0x00FF e 0x0200–0x08FF. */
     uint8_t *bootrom;
     uint32_t bootrom_size;
     /* Verdadeiro enquanto a boot ROM ainda está mapeada sobre a ROM do cartucho */
     bool bootrom_mapped;
};

static inline void die(void)
{
     exit(EXIT_FAILURE);
}

#endif /* _GB_GB_H_ */
