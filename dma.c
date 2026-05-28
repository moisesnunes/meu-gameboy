/*
 * dma.c - Emulação do OAM DMA do Game Boy
 *
 * O OAM DMA (Direct Memory Access) copia 160 bytes de uma região de memória
 * para a OAM (Object Attribute Memory, 0xFE00–0xFE9F), que armazena os
 * atributos dos 40 sprites do hardware.
 *
 * Funcionamento:
 *   - Iniciado escrevendo o byte alto do endereço fonte em 0xFF46 (ex: 0x80
 *     copia de 0x8000–0x809F).
 *   - O DMA transfere 1 byte a cada 4 T-cycles (1 ciclo de máquina), levando
 *     160 ciclos de máquina (640 T-cycles) para completar.
 *   - Durante a transferência a CPU só pode acessar a HRAM (0xFF80–0xFFFE);
 *     qualquer outra leitura retorna 0xFF.
 *   - O DMA pode ser reiniciado enquanto está em andamento; nesse caso a
 *     posição é resetada mas os dados já copiados permanecem na OAM.
 *   - Em double speed (CGB), cycles_per_byte é 2 T-cycles em vez de 4, pois
 *     o DMA opera no clock da CPU, que dobrou.
 *
 * Atraso de início:
 *   - O hardware demora 8 T-cycles (2 ciclos de máquina) antes de começar a
 *     copiar o primeiro byte. Nesse intervalo o barramento já está bloqueado.
 */

#include "gb.h"

/* Tamanho total da transferência DMA: 40 sprites × 4 bytes cada = 160 bytes */
#define GB_DMA_LENGTH_BYTES (GB_GPU_MAX_SPRITES * 4)

/* Atraso em T-cycles entre a escrita em 0xFF46 e a primeira cópia de byte */
#define GB_DMA_START_DELAY_CYCLES 8U

/*
 * gb_dma_reset - Inicializa o estado do DMA para o início da emulação.
 *
 * Garante que nenhuma transferência esteja ativa e todos os contadores estão
 * zerados. Chamado durante gb_reset() antes de qualquer ROM ser executada.
 */
void gb_dma_reset(struct gb *gb)
{
    struct gb_dma *dma = &gb->dma;

    dma->running = false;
    dma->restarting = false;
    dma->syncing = false;
    dma->source = 0;
    dma->position = 0;
    dma->delay = 0;
}

/*
 * gb_dma_sync - Avança o estado do DMA até o timestamp atual.
 *
 * Chamado sempre que o sistema precisa observar o estado da OAM (leituras,
 * escritas, fim de scanline) ou quando o DMA pode ter completado desde a
 * última sincronização.
 *
 * O loop consome `elapsed` T-cycles processando um byte por vez:
 *   1. Aguarda o delay atual (atraso de início ou intervalo entre bytes).
 *   2. Lê 1 byte do endereço fonte e grava na OAM.
 *   3. Agenda o próximo evento via gb_sync_next.
 *
 * Caso especial: endereços >= 0xE000 (Echo RAM / área proibida).
 *   - No DMG: espelha para 0xC000–0xDFFF removendo o bit 13.
 *   - No CGB: retorna 0xFF (acesso inválido nesta faixa).
 */
void gb_dma_sync(struct gb *gb)
{
    struct gb_dma *dma = &gb->dma;
    int32_t elapsed = gb_sync_resync(gb, GB_SYNC_DMA);
    /* Em double speed o DMA ainda copia 1 byte por ciclo de máquina,
     * mas cada ciclo de máquina vale 2 T-cycles em vez de 4. */
    unsigned cycles_per_byte = 4U >> gb->double_speed;

    if (!dma->running)
    {
        /* Nothing to do */
        gb_sync_next(gb, GB_SYNC_DMA, GB_SYNC_NEVER);
        return;
    }

    while (dma->running)
    {
        uint16_t source_addr;
        uint32_t b;

        /* Ainda dentro do período de delay: subtrai o tempo decorrido e
         * agenda o próximo evento para quando o delay expirar. */
        if (elapsed < dma->delay)
        {
            dma->delay -= (uint8_t)elapsed;
            gb_sync_next(gb, GB_SYNC_DMA, dma->delay);
            return;
        }

        elapsed -= dma->delay;

        if (dma->position >= GB_DMA_LENGTH_BYTES)
        {
            /* The bus remains blocked for one final DMA slot after byte 159. */
            dma->running = false;
            dma->restarting = false;
            dma->delay = 0;
            gb_sync_next(gb, GB_SYNC_DMA, GB_SYNC_NEVER);
            return;
        }

        /* Flag prevents the CPU bus-block check in gb_memory_readb from
         * blocking the DMA engine's own reads from the source address. */
        source_addr = dma->source + dma->position;
        if (source_addr >= 0xe000U && gb->gbc)
        {
            /* No CGB, leituras de 0xE000+ pelo DMA retornam 0xFF */
            b = 0xff;
        }
        else
        {
            /* No DMG, 0xE000–0xFFFF espelha a WRAM (remove o bit 13) */
            if (source_addr >= 0xe000U)
                source_addr &= ~0x2000U;

            dma->syncing = true;
            b = gb_memory_readb(gb, source_addr);
            dma->syncing = false;
        }

        gb->gpu.oam[dma->position] = b;
        gb->debug.sys_viz.fade_dma_oam = 1.0f;
        gb_debug_hw_trace_oam_dma(gb, (uint8_t)dma->position, (uint8_t)b);

        dma->position++;

        if (dma->position >= GB_DMA_LENGTH_BYTES)
        {
            /* Todos os 160 bytes foram copiados. Mantém o barramento bloqueado
             * por mais um slot antes de liberar (comportamento do hardware). */
            dma->delay = (uint8_t)cycles_per_byte;
            gb_sync_next(gb, GB_SYNC_DMA, dma->delay);
            return;
        }

        /* Intervalo de um ciclo de máquina até o próximo byte */
        dma->delay = (uint8_t)cycles_per_byte;
    }
}

/*
 * gb_dma_start - Inicia (ou reinicia) uma transferência OAM DMA.
 *
 * Chamado quando a CPU escreve em 0xFF46. O byte `source` é o valor escrito;
 * o endereço fonte real é `source << 8` (ex: 0xC0 → copia de 0xC000).
 *
 * Se o DMA já estiver em andamento (position < 160), a transferência é
 * reiniciada: posição volta a 0, novo endereço fonte é carregado. Os bytes
 * já copiados para a OAM não são desfeitos. O flag `restarting` permite que
 * a lógica de bloqueio de barramento trate o primeiro ciclo do DMA reiniciado
 * de forma especial.
 */
void gb_dma_start(struct gb *gb, uint8_t source)
{
    struct gb_dma *dma = &gb->dma;
    bool restarting;

    /* Sync our state in case we were already running */
    gb_dma_sync(gb);
    restarting = dma->running && dma->position < GB_DMA_LENGTH_BYTES;

    dma->source = (uint16_t)source << 8;
    dma->position = 0;
    dma->delay = GB_DMA_START_DELAY_CYCLES >> gb->double_speed;
    dma->restarting = restarting;

    dma->running = true;
    gb_sync_next(gb, GB_SYNC_DMA, dma->delay);
}
