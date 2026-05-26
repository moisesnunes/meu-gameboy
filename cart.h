#ifndef _GB_CART_H_
#define _GB_CART_H_

#include "rtc.h"

enum gb_cart_model
{
     /* Sem mapeador: 2 bancos de ROM, RAM opcional */
     GB_CART_SIMPLE = 0,
     /* Mapeador MBC1, ate 128 bancos de ROM, 4 bancos de RAM */
     GB_CART_MBC1,
     /* Mapeador MBC2, ate 16 bancos de ROM, uma unica RAM de 512 * 4 bits */
     GB_CART_MBC2,
     /* Mapeador MBC3, ate 128 bancos de ROM, 4 bancos de RAM, RTC opcional */
     GB_CART_MBC3,
     /* Mapeador MBC5, ate 512 bancos de ROM, 16 bancos de RAM */
     GB_CART_MBC5,
     /* Mapeador MBC7: 256 bancos de ROM, acelerômetro ADXL202E, EEPROM 93C56 */
     GB_CART_MBC7,
     /* Mapeador Hudson HuC1: ROM/RAM banking e porta infravermelha */
     GB_CART_HUC1,
     /* Mapeador Hudson HuC3: ROM/RAM banking, RTC proprio e infravermelho */
     GB_CART_HUC3,
};

/*
 * Estado do chip EEPROM 93C56 embutido no MBC7.
 *
 * O cartucho MBC7 (Kirby Tilt 'n' Tumble, Command Master) conecta um chip de
 * EEPROM serial 93C56 ao barramento de RAM em 0xA008. O acesso é bit-banged:
 * o jogo controla CS, CLK e DIN escrevendo no byte 0xA008, e lê DOUT do bit 0.
 *
 * O 93C56 tem 128 palavras de 16 bits (256 bytes) e usa comandos de 11 bits:
 *   1 start-bit + 2 opcode bits + 8 bits de endereço
 * Seguidos de 16 bits de dados (para READ/WRITE).
 */
struct gb_mbc7
{
     /* Armazenamento persistente: 128 palavras × 16 bits = 256 bytes */
     uint8_t eeprom[256];
     /* Verdadeiro após o jogo executar o comando EWEN (write enable) */
     bool write_enabled;

     /* Interface serial bit-banged */
     uint8_t cs;         /* CS atual */
     uint8_t clk;        /* CLK atual */
     uint8_t din;        /* DIN atual (para echo na leitura) */
     uint8_t dout;       /* DOUT corrente: dado saindo da EEPROM para o jogo */

     /* Registrador de entrada: bits recebidos enquanto recebe o comando */
     uint32_t recv;      /* shift register; MSB recebido primeiro */
     int recv_count;     /* número de bits recebidos até agora (0–11) */

     /* Modo READ: enviando 16 bits de dados */
     uint16_t send;      /* palavra sendo deslocada para fora */
     int send_count;     /* bits restantes a enviar (0 = inativo) */

     /* Modo WRITE/WRAL: recebendo 16 bits de dados */
     uint16_t wdata;     /* palavra sendo montada */
     int wdata_count;    /* bits restantes a receber (0 = inativo) */
     uint8_t waddr;      /* endereço de destino para WRITE */
     bool wral;          /* true quando WRAL (write-all) está em andamento */

     /* Acelerômetro ADXL202E (emulado como posição neutra) */
     bool accel_latch_prepare; /* primeiro passo do latch (write 0x55 a offset 0x00) */
     bool accel_latched;       /* true após completar o latch de dois passos */
     uint16_t accel_x;         /* valor latched de X (neutro = 0x8000) */
     uint16_t accel_y;         /* valor latched de Y (neutro = 0x8000) */

     /* Habilitar a região 0xA000-0xBFFF requer dois desbloqueios separados */
     bool ram_enable_1;  /* write 0x0A para 0x0000-0x1FFF */
     bool ram_enable_2;  /* write 0x40 para 0x4000-0x5FFF */
};

struct gb_huc1
{
     bool ir_mode;
     uint8_t ir_out;
};

struct gb_huc3
{
     uint8_t mode;
     uint8_t access_index;
     uint8_t read;
     uint8_t access_flags;
     uint8_t ir_out;
     uint16_t minutes;
     uint16_t days;
     uint16_t alarm_minutes;
     uint16_t alarm_days;
     bool alarm_enabled;
     uint64_t last_rtc_second;
};

struct gb_cart
{
     /* Conteudo completo da ROM */
     uint8_t *rom;
     /* Tamanho da ROM em bytes */
     unsigned rom_length;
     /* Numero de bancos de ROM (cada banco tem 16 KB) */
     unsigned rom_banks;
     /* Banco de ROM selecionado atualmente */
     unsigned cur_rom_bank;
     /* Conteudo completo da RAM do cartucho */
     uint8_t *ram;
     /* Tamanho da RAM em bytes */
     unsigned ram_length;
     /* Numero de bancos de RAM (cada banco tem 8 KB) */
     unsigned ram_banks;
     /* Banco de RAM selecionado atualmente */
     unsigned cur_ram_bank;
     /* Verdadeiro se a RAM esta protegida contra escrita (somente leitura) */
     bool ram_write_protected;
     /* Tipo de cartucho */
     enum gb_cart_model model;
     /* Falso se o cartucho MBC1 opera na configuracao de 128 bancos de ROM/
      * 1 banco de RAM; caso contrario, opera na configuracao de 32 bancos de
      * ROM/4 bancos de RAM. */
     bool mbc1_bank_ram;
     /* Verdadeiro se o cartucho e um MBC1M (multi-cart): dois jogos de 4MB
      * num unico cartucho, detectado comparando o logo/header nas duas ROMs. */
     bool mbc1_multicart;
     /* Se houver bateria de backup, salvamos e restauramos o conteudo da RAM a
      * partir deste arquivo */
     char *save_file;
     /* Flag de modificacao, definida como verdadeira quando a RAM recebe escrita */
     bool dirty_ram;
     /* Verdadeiro se o cartucho tem Relogio em Tempo Real (MBC3 tipos 0x0F e 0x10) */
     bool has_rtc;
     /* Verdadeiro se o cartucho MBC5 usa bit de rumble no registrador de RAM */
     bool has_rumble;
     /* Verdadeiro se o cartucho MBC7 tem EEPROM persistente */
     bool has_eeprom;
     /* Estado do RTC (valido apenas quando has_rtc e verdadeiro) */
     struct gb_rtc rtc;
     /* Estado do MBC7 (valido apenas quando model == GB_CART_MBC7) */
     struct gb_mbc7 mbc7;
     /* Estado do HuC1 (valido apenas quando model == GB_CART_HUC1) */
     struct gb_huc1 huc1;
     /* Estado do HuC3 (valido apenas quando model == GB_CART_HUC3) */
     struct gb_huc3 huc3;
};

void gb_cart_load(struct gb *gb, const char *rom_path);
void gb_cart_unload(struct gb *gb);
void gb_cart_sync(struct gb *gb);
bool gb_cart_save_ram_now(struct gb *gb);
bool gb_cart_load_ram_now(struct gb *gb);
uint8_t gb_cart_rom_readb(struct gb *gb, uint16_t addr);
void gb_cart_rom_writeb(struct gb *gb, uint16_t addr, uint8_t v);
uint8_t gb_cart_ram_readb(struct gb *gb, uint16_t addr);
void gb_cart_ram_writeb(struct gb *gb, uint16_t addr, uint8_t v);

#endif /* _GB_CART_H_ */
