/*
 * cart.c — Emulação de Cartucho do Game Boy
 *
 * Este módulo cuida do carregamento e da emulação de cartuchos de Game Boy,
 * incluindo os diversos Memory Bank Controllers (MBC) que permitiam aos jogos
 * superar o limite de 32 KB do espaço de endereçamento bruto.
 *
 * Mapa de memória do GB (regiões relevantes):
 *   0x0000–0x3FFF  Banco de ROM 0   (fixo, sempre banco 0)
 *   0x4000–0x7FFF  Banco de ROM N   (comutável via MBC)
 *   0xA000–0xBFFF  RAM externa      (RAM do cartucho, janela de 8 KB, comutável)
 *
 * Tipos de MBC emulados:
 *   SIMPLE  Sem mapeador, até 32 KB de ROM, RAM opcional.
 *   MBC1    Até 2 MB de ROM / 32 KB de RAM. Primeiro e mais comum MBC.
 *   MBC2    Até 256 KB de ROM / 512×4 bits de RAM. Raro, com RAM de nibble embutida.
 *   MBC3    Até 2 MB de ROM / 32 KB de RAM. Comum nos jogos de Pokémon (RTC não emulado).
 *   MBC5    Até 8 MB de ROM / 128 KB de RAM. Usado em jogos GB/GBC de geração mais recente.
 *
 * Layout do cabeçalho do cartucho (offsets na ROM):
 *   0x0100–0x0103  Ponto de entrada
 *   0x0104–0x0133  Logo da Nintendo (verificado pela boot ROM)
 *   0x0134–0x0143  Título do jogo (ASCII, preenchido com nulos)
 *   0x0143         Flag de compatibilidade GBC (bit 7 = jogo GBC)
 *   0x0147         Tipo de cartucho / MBC
 *   0x0148         Código do tamanho da ROM
 *   0x0149         Código do tamanho da RAM
 *   0x014D         Checksum do cabeçalho (verificado pela boot ROM)
 */

#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "gb.h"

/* Cada banco de ROM tem 16 KB. A CPU do GB acessa a ROM em duas janelas de 16 KB. */
#define GB_ROM_BANK_SIZE (16 * 1024)

/* Cada banco de RAM tem 8 KB. A CPU do GB acessa a RAM externa em uma janela de 8 KB. */
#define GB_RAM_BANK_SIZE (8 * 1024)

/* Uma ROM de GB válida tem pelo menos dois bancos: banco 0 (fixo) + banco 1 (comutável). */
#define GB_CART_MIN_SIZE (GB_ROM_BANK_SIZE * 2)

/*
 * O maior cartucho de GB licenciado tinha 8 MB (MBC5, ex.: Pokémon Ouro/Prata).
 * 32 MB dá margem para ROMs de homebrew maiores.
 */
#define GB_CART_MAX_SIZE (32U * 1024 * 1024)

/* Offsets de byte dentro do cabeçalho do cartucho (todos no primeiro banco de ROM). */
#define GB_CART_OFF_TITLE     0x134  /* Título do jogo, até 16 bytes ASCII */
#define GB_CART_OFF_GBC       0x143  /* Flag GBC: bit 7 = compatível com GBC */
#define GB_CART_OFF_TYPE      0x147  /* Tipo de cartucho / identificador do MBC */
#define GB_CART_OFF_ROM_BANKS 0x148  /* Código do tamanho da ROM (decodificado em gb_cart_load) */
#define GB_CART_OFF_RAM_BANKS 0x149  /* Código do tamanho da RAM (decodificado em gb_cart_load) */

static uint64_t gb_cart_system_time(void)
{
     return (uint64_t)time(NULL);
}

static void gb_cart_dump_u8(FILE *f, uint8_t v)
{
     if (fwrite(&v, 1, 1, f) != 1)
     {
          perror("fwrite failed");
          die();
     }
}

static bool gb_cart_load_u8(FILE *f, uint8_t *v)
{
     return fread(v, 1, 1, f) == 1;
}

static void gb_cart_dump_u16(FILE *f, uint16_t v)
{
     gb_cart_dump_u8(f, (uint8_t)(v >> 8));
     gb_cart_dump_u8(f, (uint8_t)v);
}

static bool gb_cart_load_u16(FILE *f, uint16_t *v)
{
     uint8_t hi, lo;

     if (!gb_cart_load_u8(f, &hi) || !gb_cart_load_u8(f, &lo))
     {
          return false;
     }

     *v = (uint16_t)((hi << 8) | lo);
     return true;
}

static void gb_cart_dump_u64(FILE *f, uint64_t v)
{
     gb_cart_dump_u8(f, (uint8_t)(v >> 56));
     gb_cart_dump_u8(f, (uint8_t)(v >> 48));
     gb_cart_dump_u8(f, (uint8_t)(v >> 40));
     gb_cart_dump_u8(f, (uint8_t)(v >> 32));
     gb_cart_dump_u8(f, (uint8_t)(v >> 24));
     gb_cart_dump_u8(f, (uint8_t)(v >> 16));
     gb_cart_dump_u8(f, (uint8_t)(v >> 8));
     gb_cart_dump_u8(f, (uint8_t)v);
}

static bool gb_cart_load_u64(FILE *f, uint64_t *v)
{
     uint8_t b;
     unsigned i;

     *v = 0;
     for (i = 0; i < 8; i++)
     {
          if (!gb_cart_load_u8(f, &b))
          {
               return false;
          }
          *v = (*v << 8) | b;
     }

     return true;
}

static void gb_huc3_init(struct gb *gb)
{
     struct gb_huc3 *h = &gb->cart.huc3;

     memset(h, 0, sizeof(*h));
     h->minutes = 0x0fff;
     h->days = 0xffff;
     h->last_rtc_second = gb_cart_system_time();
}

static void gb_huc3_sync_clock(struct gb *gb)
{
     struct gb_huc3 *h = &gb->cart.huc3;
     uint64_t now = gb_cart_system_time();

     if (h->last_rtc_second == 0 || h->last_rtc_second > now)
     {
          h->last_rtc_second = now;
          return;
     }

     while (h->last_rtc_second / 60 < now / 60)
     {
          h->last_rtc_second += 60;
          h->minutes = (uint16_t)(h->minutes + 1);
          if (h->minutes == 60 * 24)
          {
               h->minutes = 0;
               h->days = (uint16_t)(h->days + 1);
          }
     }
}

static void gb_huc3_reset_clock_epoch(struct gb *gb)
{
     gb->cart.huc3.last_rtc_second = gb_cart_system_time();
}

static void gb_huc3_dump(struct gb *gb, FILE *f)
{
     struct gb_huc3 *h = &gb->cart.huc3;

     gb_huc3_sync_clock(gb);
     gb_cart_dump_u64(f, h->last_rtc_second);
     gb_cart_dump_u16(f, h->minutes);
     gb_cart_dump_u16(f, h->days);
     gb_cart_dump_u16(f, h->alarm_minutes);
     gb_cart_dump_u16(f, h->alarm_days);
     gb_cart_dump_u8(f, h->alarm_enabled ? 1 : 0);
}

static bool gb_huc3_load(struct gb *gb, FILE *f)
{
     struct gb_huc3 *h = &gb->cart.huc3;
     uint8_t alarm_enabled;

     if (!gb_cart_load_u64(f, &h->last_rtc_second) ||
         !gb_cart_load_u16(f, &h->minutes) ||
         !gb_cart_load_u16(f, &h->days) ||
         !gb_cart_load_u16(f, &h->alarm_minutes) ||
         !gb_cart_load_u16(f, &h->alarm_days) ||
         !gb_cart_load_u8(f, &alarm_enabled))
     {
          fprintf(stderr, "HuC3 RTC state missing; resetting clock\n");
          gb_huc3_init(gb);
          return false;
     }

     h->alarm_enabled = alarm_enabled & 1;
     gb_huc3_sync_clock(gb);
     return true;
}

/*
 * gb_cart_get_rom_title — extrai o título terminado em nulo do cabeçalho da ROM.
 *
 * O campo de título fica em 0x0134 e tem até 16 bytes ASCII. Não há garantia
 * de terminação em nulo quando todos os 16 bytes são usados, então o laço é
 * limitado e sempre escrevemos um nulo no final. Bytes não imprimíveis são
 * substituídos por '?' para evitar lixo na saída de log.
 *
 * @title: buffer de saída de pelo menos 17 bytes (16 chars + terminador nulo).
 */
static void gb_cart_get_rom_title(struct gb *gb, char title[17])
{
     struct gb_cart *cart = &gb->cart;
     unsigned i;

     for (i = 0; i < 16; i++)
     {
          char c = cart->rom[GB_CART_OFF_TITLE + i];

          if (c == 0)
          {
               /* Fim do título */
               break;
          }

          if (!isprint(c))
          {
               c = '?';
          }

          title[i] = c;
     }

     /* Fim da string */
     title[i] = '\0';
}

/*
 * gb_cart_load — carrega um arquivo de ROM e inicializa o estado do cartucho.
 *
 * Etapas realizadas:
 *   1. Abre e valida o arquivo de ROM (verificação de limites de tamanho).
 *   2. Lê a ROM em um buffer no heap.
 *   3. Decodifica a quantidade de bancos de ROM do byte 0x148 do cabeçalho.
 *   4. Decodifica a quantidade de bancos de RAM do byte 0x149 do cabeçalho.
 *   5. Identifica o tipo de MBC do byte 0x147 do cabeçalho.
 *   6. Determina se o cartucho tem bateria (RAM de save persistente).
 *   7. Aloca o buffer de RAM e carrega o arquivo .sav, se existir.
 *   8. Imprime informações do cartucho na saída padrão.
 *
 * Em qualquer erro, a função salta para `error:`, libera alocações parciais
 * e chama die() para abortar o emulador.
 */
void gb_cart_load(struct gb *gb, const char *rom_path)
{
     struct gb_cart *cart = &gb->cart;
     FILE *f = fopen(rom_path, "rb");
     long l;
     size_t nread;
     char rom_title[17];
     bool has_battery_backup;

     /* Inicializa os campos do cartucho com valores seguros antes de qualquer alocação. */
     cart->rom = NULL;
     cart->cur_rom_bank = 1;      /* Banco 0 é fixo; banco 1 é o banco comutável padrão */
     cart->ram = NULL;
     cart->cur_ram_bank = 0;
     cart->ram_write_protected = true;  /* RAM começa bloqueada; o jogo deve habilitá-la explicitamente */
     cart->mbc1_bank_ram = false;       /* MBC1 inicia no modo de bancos de ROM */
     cart->mbc1_multicart = false;
     cart->save_file = NULL;
     cart->dirty_ram = false;
     cart->has_rtc    = false;
     cart->has_rumble = false;
     cart->has_eeprom = false;
     memset(&cart->mbc7, 0, sizeof(cart->mbc7));
     memset(&cart->huc1, 0, sizeof(cart->huc1));
     gb_huc3_init(gb);
     has_battery_backup = false;

     if (f == NULL)
     {
          perror("Can't open ROM file");
          goto error;
     }

     /* Determina o tamanho do arquivo usando seek/tell. */
     if (fseek(f, 0, SEEK_END) == -1 ||
         (l = ftell(f)) == -1 ||
         fseek(f, 0, SEEK_SET) == -1)
     {
          fclose(f);
          perror("Can't get ROM file length");
          goto error;
     }

     if (l == 0)
     {
          fprintf(stderr, "ROM file is empty!\n");
          goto error;
     }

     if (l > GB_CART_MAX_SIZE)
     {
          fprintf(stderr, "ROM file is too big!\n");
          goto error;
     }

     if (l < GB_CART_MIN_SIZE)
     {
          fprintf(stderr, "ROM file is too small!\n");
          goto error;
     }

     cart->rom_length = l;
     cart->rom = calloc(1, cart->rom_length);
     if (cart->rom == NULL)
     {
          perror("Can't allocate ROM buffer");
          goto error;
     }

     nread = fread(cart->rom, 1, cart->rom_length, f);
     if (nread < cart->rom_length)
     {
          fprintf(stderr,
                  "Failed to load ROM file (read %u bytes, expected %u)\n",
                  (unsigned)nread, cart->rom_length);
          goto error;
     }

     /*
      * Decodifica a quantidade de bancos de ROM do byte 0x148 do cabeçalho.
      *
      * Os códigos padrão seguem a fórmula: bancos = 2 << código (ou seja, 2^(código+1)).
      * Os códigos 0x52–0x54 são tamanhos não-potência-de-2 usados por alguns jogos
      * (ex.: 0x52 = 72 bancos = 1,125 MB, usado em alguns títulos japoneses).
      *
      * Os dados reais da ROM devem ser grandes o suficiente para comportar todos os
      * bancos declarados; uma divergência de tamanho indica uma imagem corrompida ou truncada.
      */
     switch (cart->rom[GB_CART_OFF_ROM_BANKS])
     {
     case 0:  cart->rom_banks = 2;   break;  /*  32 KB */
     case 1:  cart->rom_banks = 4;   break;  /*  64 KB */
     case 2:  cart->rom_banks = 8;   break;  /* 128 KB */
     case 3:  cart->rom_banks = 16;  break;  /* 256 KB */
     case 4:  cart->rom_banks = 32;  break;  /* 512 KB */
     case 5:  cart->rom_banks = 64;  break;  /*   1 MB */
     case 6:  cart->rom_banks = 128; break;  /*   2 MB */
     case 7:  cart->rom_banks = 256; break;  /*   4 MB */
     case 8:  cart->rom_banks = 512; break;  /*   8 MB */
     /* Tamanhos não-potência-de-2, usados por um pequeno número de lançamentos japoneses: */
     case 0x52: cart->rom_banks = 72;  break; /* 1,125 MB */
     case 0x53: cart->rom_banks = 80;  break; /* 1,25  MB */
     case 0x54: cart->rom_banks = 96;  break; /* 1,5   MB */
     default:
          fprintf(stderr, "Unknown ROM size configuration: %x\n",
                  cart->rom[GB_CART_OFF_ROM_BANKS]);
          goto error;
     }

     /* Garante que o tamanho do arquivo de ROM é coerente com a quantidade declarada de bancos. */
     if (cart->rom_length < cart->rom_banks * GB_ROM_BANK_SIZE)
     {
          fprintf(stderr, "ROM file is too small to hold the declared"
                          " %d ROM banks\n",
                  cart->rom_banks);
          goto error;
     }

     /*
      * Decodifica a quantidade de bancos de RAM do byte 0x149 do cabeçalho.
      *
      * O código 1 é um caso especial: o hardware tem apenas um chip de 2 KB, que
      * equivale a um quarto de um banco completo de 8 KB. Ele é espelhado na janela de 8 KB.
      * O MBC2 ignora este campo completamente (ele possui 512×4 bits de RAM embutida).
      */
     switch (cart->rom[GB_CART_OFF_RAM_BANKS])
     {
     case 0: /* Sem RAM */
          cart->ram_banks = 0;
          cart->ram_length = 0;
          break;
     case 1:
          /* Um banco, mas apenas 2 KB (ou seja, 1/4 de um banco) */
          cart->ram_banks = 1;
          cart->ram_length = GB_RAM_BANK_SIZE / 4;
          break;
     case 2:
          cart->ram_banks = 1;
          cart->ram_length = GB_RAM_BANK_SIZE;           /*  8 KB */
          break;
     case 3:
          cart->ram_banks = 4;
          cart->ram_length = GB_RAM_BANK_SIZE * 4;       /* 32 KB */
          break;
     case 4:
          cart->ram_banks = 16;
          cart->ram_length = GB_RAM_BANK_SIZE * 16;      /* 128 KB */
          break;
     case 5:
          cart->ram_banks = 8;
          cart->ram_length = GB_RAM_BANK_SIZE * 8;       /* 64 KB */
          break;
     default:
          fprintf(stderr, "Unknown RAM size configuration: %x\n",
                  cart->rom[GB_CART_OFF_RAM_BANKS]);
          goto error;
     }

     /*
      * Identifica o tipo de MBC do byte 0x147 do cabeçalho.
      *
      * O byte de tipo do cartucho codifica tanto o chip MBC quanto o hardware
      * opcional (RAM, bateria, rumble, etc.). Aqui precisamos apenas da parte do MBC;
      * a detecção de bateria é feita no próximo switch abaixo.
      *
      * Omissões notáveis: MBC6, Pocket Camera e TAMA5 não são emulados.
      */
     switch (cart->rom[GB_CART_OFF_TYPE])
     {
     case 0x00:                        /* ROM ONLY */
     case 0x08:                        /* ROM + RAM */
     case 0x09:                        /* ROM + RAM + BATERIA */
          cart->model = GB_CART_SIMPLE;
          break;
     case 0x01:                        /* MBC1 */
     case 0x02:                        /* MBC1 + RAM */
     case 0x03:                        /* MBC1 + RAM + BATERIA */
          cart->model = GB_CART_MBC1;
          break;
     case 0x05:                        /* MBC2 */
     case 0x06:                        /* MBC2 + BATERIA */
          cart->model = GB_CART_MBC2;
          /*
           * O MBC2 sempre tem 512 × 4 bits de RAM embutida, independente do byte 0x149.
           * Alocamos 512 bytes completos por conveniência; apenas o nibble baixo
           * de cada byte é válido (o nibble alto lê como 0xF).
           */
          cart->ram_banks = 1;
          cart->ram_length = 512;
          break;
     case 0x0f:                        /* MBC3 + TIMER + BATERIA */
     case 0x10:                        /* MBC3 + TIMER + RAM + BATERIA */
     case 0x11:                        /* MBC3 */
     case 0x12:                        /* MBC3 + RAM */
     case 0x13:                        /* MBC3 + RAM + BATERIA */
          cart->model = GB_CART_MBC3;
          break;
     case 0x19:                        /* MBC5 */
     case 0x1a:                        /* MBC5 + RAM */
     case 0x1b:                        /* MBC5 + RAM + BATERIA */
          cart->model = GB_CART_MBC5;
          break;
     case 0x1c:                        /* MBC5 + RUMBLE */
     case 0x1d:                        /* MBC5 + RUMBLE + RAM */
     case 0x1e:                        /* MBC5 + RUMBLE + RAM + BATERIA */
          cart->model = GB_CART_MBC5;
          cart->has_rumble = true;
          break;
     case 0x22:                        /* MBC7 + SENSOR + RUMBLE + EEPROM */
          cart->model = GB_CART_MBC7;
          cart->has_rumble = true;
          cart->has_eeprom = true;
          /* EEPROM: 128 palavras × 16 bits = 256 bytes (sem RAM convencional) */
          cart->ram_banks  = 0;
          cart->ram_length = 0;
          /* Inicializa EEPROM com 0xFF (estado apagado) */
          memset(cart->mbc7.eeprom, 0xff, sizeof(cart->mbc7.eeprom));
          cart->mbc7.dout  = 1;
          break;
     case 0xfe:                        /* HuC3 + RTC + IR + RAM + BATERIA */
          cart->model = GB_CART_HUC3;
          break;
     case 0xff:                        /* HuC1 + IR + RAM + BATERIA */
          cart->model = GB_CART_HUC1;
          cart->ram_write_protected = false;
          break;
     default:
          fprintf(stderr, "Unsupported cartridge type %x\n",
                  cart->rom[GB_CART_OFF_TYPE]);
          goto error;
     }

     /*
      * Verifica se o cartucho tem bateria para RAM não-volátil.
      *
      * A bateria permite que o conteúdo da RAM persista quando o console é desligado,
      * possibilitando saves de jogo. Emulamos isso persistindo a RAM em um arquivo .sav.
      *
      * Códigos de tipo com bateria (do Pan Docs):
      *   0x03  MBC1 + RAM + BATERIA
      *   0x06  MBC2 + BATERIA
      *   0x09  ROM + RAM + BATERIA (raro)
      *   0x0F  MBC3 + TIMER + BATERIA
      *   0x10  MBC3 + TIMER + RAM + BATERIA
      *   0x13  MBC3 + RAM + BATERIA
      *   0x1B  MBC5 + RAM + BATERIA
      *   0x1E  MBC5 + RUMBLE + RAM + BATERIA
      *   0xFE  HuC3 + TIMER + RAM + BATERIA
      *   0xFF  HuC1 + RAM + BATERIA
      */
     switch (cart->rom[GB_CART_OFF_TYPE])
     {
     case 0x03:
     case 0x06:
     case 0x09:
     case 0x0f:
     case 0x10:
     case 0x13:
     case 0x1b:
     case 0x1e:
     case 0x22: /* MBC7 */
     case 0xfe:
     case 0xff:
          has_battery_backup = true;
     }

     /* Verifica se o cartucho tem RTC */
     switch (cart->rom[GB_CART_OFF_TYPE])
     {
     case 0x0f:
     case 0x10:
     case 0xfe:
          cart->has_rtc = true;
     }

     /* Aloca o buffer de RAM (inicializado com zeros, imitando o estado de liga). */
     if (cart->ram_length > 0)
     {
          cart->ram = calloc(1, cart->ram_length);
          if (cart->ram == NULL)
          {
               perror("Can't allocate RAM buffer");
               goto error;
          }
     }
     else if (!cart->has_rtc && !cart->has_eeprom)
     {
          /* Backup de memória sem RAM, RTC ou EEPROM não faz sentido */
          has_battery_backup = false;
     }

     if (cart->model == GB_CART_SIMPLE && cart->ram_length > 0)
     {
          cart->ram_write_protected = false;
     }

     if (has_battery_backup)
     {
          /*
           * Constrói o caminho do arquivo de save substituindo a extensão do arquivo
           * de ROM por ".sav". Se não houver extensão, simplesmente acrescenta ".sav".
           * Exemplo: "roms/zelda.gb" → "roms/zelda.sav"
           */
          const size_t path_len = strlen(rom_path);
          FILE *f;
          size_t pos;

          /* +strlen(".sav")+1: espaco para extensao nova e terminador nulo.
           * Quando o path ja tem extensao, o ponto antigo e reaproveitado. */
          cart->save_file = malloc(path_len + strlen(".sav") + 1);
          if (cart->save_file == NULL)
          {
               perror("malloc failed");
               goto error;
          }

          strcpy(cart->save_file, rom_path);

          /* Varre de trás para frente procurando o último '.' para encontrar a extensão. */
          for (pos = path_len - 1; pos > 0; pos--)
          {
               if (cart->save_file[pos] == '.')
               {
                    /* Extensão encontrada; trunca aqui */
                    cart->save_file[pos] = '\0';
                    break;
               }
          }

          strcat(cart->save_file, ".sav");

          /* Primeiro tentamos carregar o arquivo de save, caso ele já exista */
          f = fopen(cart->save_file, "rb");
          if (f != NULL)
          {
               /* O arquivo existe; carrega o conteúdo da RAM */
               if (cart->ram_length > 0)
               {
                    nread = fread(cart->ram, 1, cart->ram_length, f);
                    if (nread != cart->ram_length)
                    {
                         fprintf(stderr, "RAM save file is too small!\n");
                         fclose(f);
                         goto error;
                    }
               }

               if (cart->model == GB_CART_HUC3)
               {
                    gb_huc3_load(gb, f);
               }
               else if (cart->has_rtc)
               {
                    gb_rtc_load(gb, f);
               }

               if (cart->has_eeprom)
               {
                    if (fread(cart->mbc7.eeprom,
                              1, sizeof(cart->mbc7.eeprom), f) !=
                        sizeof(cart->mbc7.eeprom))
                    {
                         fprintf(stderr, "EEPROM save file is too small!\n");
                         fclose(f);
                         goto error;
                    }
               }

               fclose(f);
               fprintf(stderr, "Loaded RAM save from '%s'\n", cart->save_file);
          }
          else if (cart->model == GB_CART_HUC3)
          {
               gb_huc3_init(gb);
          }
          else if (cart->has_rtc)
          {
               gb_rtc_init(gb);
          }
     }

     /* Sucesso */
     fclose(f);

     /*
      * Detecta compatibilidade com GBC pelo byte 0x143 do cabeçalho.
      * Bit 7 setado (0x80 ou 0xC0) indica jogo compatível com GBC ou exclusivo de GBC.
      * Jogos exclusivos de DMG deixam este byte como parte do título.
      */
     gb->gbc = (cart->rom[GB_CART_OFF_GBC] & 0x80);
     gb->hw_model = gb->gbc ? GB_HW_CGB : GB_HW_DMG;

     /* Detecta MBC1M (multi-cart): dois jogos de 4 MB num unico cartucho MBC1.
      * O sinal e que o logo/header em 0x0104 e identico ao em 0x40104.
      * Nesse modo o banco ROM usa apenas 4 bits baixos, e bits [5:4] selecionam
      * qual dos dois sub-jogos esta ativo. */
     if (cart->model == GB_CART_MBC1 &&
         cart->rom_length >= 0x44000 + 0x30 &&
         memcmp(cart->rom + 0x104, cart->rom + 0x40104, 0x30) == 0)
     {
          cart->mbc1_multicart = true;
     }

     gb_cart_get_rom_title(gb, rom_title);

     fprintf(stderr, "Succesfully Loaded %s\n", rom_path);
     fprintf(stderr, "Title: '%s'\n", rom_title);
     fprintf(stderr, "ROM banks: %u (%uKiB)\n", cart->rom_banks,
            cart->rom_banks * GB_ROM_BANK_SIZE / 1024);
     fprintf(stderr, "RAM banks: %u (%uKiB)\n", cart->ram_banks,
            cart->ram_length / 1024);
     return;

error:
     if (cart->rom)
     {
          free(cart->rom);
          cart->rom = NULL;
     }

     if (cart->ram)
     {
          free(cart->ram);
          cart->ram = NULL;
     }

     if (cart->save_file)
     {
          free(cart->save_file);
     }

     if (f)
     {
          fclose(f);
     }

     die();
}

/*
 * gb_cart_ram_save — descarrega a RAM do cartucho no arquivo .sav se houver modificações.
 *
 * Usamos um flag de sujeira (cart->dirty_ram) para evitar escritas desnecessárias em disco.
 * O save é disparado por gb_cart_sync, que é agendado alguns segundos após a última
 * escrita na RAM, dando tempo ao jogo de terminar uma operação de save multi-byte
 * antes de criarmos o snapshot.
 */
static void gb_cart_ram_save(struct gb *gb)
{
     struct gb_cart *cart = &gb->cart;
     FILE *f;

     if (cart->save_file == NULL)
     {
          /* Sem backup de bateria, nada a fazer */
          return;
     }

     if (!cart->dirty_ram && !cart->has_rtc)
     {
          /* Nenhuma alteração persistente desde o último save, nada a fazer */
          return;
     }

     f = fopen(cart->save_file, "wb");
     if (f == NULL)
     {
          fprintf(stderr, "Can't create or open save file '%s': %s",
                  cart->save_file, strerror(errno));
          die();
     }

     if (cart->ram_length > 0)
     {
          if (fwrite(cart->ram, 1, cart->ram_length, f) != cart->ram_length)
          {
               perror("fwrite failed");
               fclose(f);
               die();
          }
     }

     if (cart->model == GB_CART_HUC3)
     {
          gb_huc3_dump(gb, f);
     }
     else if (cart->has_rtc)
     {
          gb_rtc_dump(gb, f);
     }

     if (cart->has_eeprom)
     {
          if (fwrite(cart->mbc7.eeprom,
                     1, sizeof(cart->mbc7.eeprom), f) !=
              sizeof(cart->mbc7.eeprom))
          {
               perror("fwrite failed (EEPROM)");
               fclose(f);
               die();
          }
     }

     fflush(f);
     fclose(f);

     fprintf(stderr, "Saved RAM\n");
     cart->dirty_ram = false;
}

/*
 * gb_cart_unload — salva a RAM e libera todos os recursos do cartucho.
 *
 * Chamado quando o emulador é encerrado ou carrega uma nova ROM. Deve ser
 * seguro de chamar mesmo que gb_cart_load nunca tenha sido concluído com
 * sucesso (os campos são inicializados com NULL).
 */
void gb_cart_unload(struct gb *gb)
{
     struct gb_cart *cart = &gb->cart;

     gb_cart_ram_save(gb);

     if (cart->save_file)
     {
          free(cart->save_file);
     }

     if (cart->rom)
     {
          free(cart->rom);
          cart->rom = NULL;
     }

     if (cart->ram)
     {
          free(cart->ram);
          cart->ram = NULL;
     }
}

/*
 * gb_cart_sync — callback de sincronização periódica invocada pelo escalonador.
 *
 * Após uma escrita na RAM, gb_cart_ram_writeb agenda esta função para rodar
 * ~3 segundos depois (3 * GB_CPU_FREQ_HZ ciclos). Se o jogo continuar escrevendo,
 * o prazo é empurrado para frente. Quando finalmente chamada, descarregamos a RAM
 * e passamos GB_SYNC_NEVER para nos remover do escalonador até a próxima escrita.
 */
void gb_cart_sync(struct gb *gb)
{
     gb_cart_ram_save(gb);
     gb_sync_next(gb, GB_SYNC_CART, GB_SYNC_NEVER);
}

bool gb_cart_save_ram_now(struct gb *gb)
{
     struct gb_cart *cart = &gb->cart;

     if (cart->save_file == NULL)
     {
          return false;
     }

     cart->dirty_ram = true;
     gb_cart_ram_save(gb);
     gb_sync_next(gb, GB_SYNC_CART, GB_SYNC_NEVER);
     return !cart->dirty_ram;
}

bool gb_cart_load_ram_now(struct gb *gb)
{
     struct gb_cart *cart = &gb->cart;
     FILE *f;

     if (cart->save_file == NULL)
     {
          return false;
     }

     f = fopen(cart->save_file, "rb");
     if (f == NULL)
     {
          fprintf(stderr, "Can't open save file '%s': %s\n",
                  cart->save_file, strerror(errno));
          return false;
     }

     if (cart->ram_length > 0)
     {
          size_t nread = fread(cart->ram, 1, cart->ram_length, f);
          if (nread != cart->ram_length)
          {
               fprintf(stderr, "RAM save file '%s' is too small\n",
                       cart->save_file);
               fclose(f);
               return false;
          }
     }

     if (cart->model == GB_CART_HUC3)
     {
          gb_huc3_load(gb, f);
     }
     else if (cart->has_rtc)
     {
          gb_rtc_load(gb, f);
     }

     if (cart->has_eeprom)
     {
          if (fread(cart->mbc7.eeprom,
                    1, sizeof(cart->mbc7.eeprom), f) !=
              sizeof(cart->mbc7.eeprom))
          {
               fprintf(stderr, "EEPROM save file '%s' is too small\n",
                       cart->save_file);
               fclose(f);
               return false;
          }
     }

     fclose(f);
     cart->dirty_ram = false;
     gb_sync_next(gb, GB_SYNC_CART, GB_SYNC_NEVER);
     fprintf(stderr, "Loaded RAM save from '%s'\n", cart->save_file);
     return true;
}

/*
 * gb_mbc7_eeprom_write — processa uma escrita bit-banged no chip 93C56.
 *
 * O jogo controla CS (bit 7), CLK (bit 6) e DIN (bit 1) escrevendo em 0xA008.
 * Esta função detecta bordas de CS e CLK para avançar a máquina de estados.
 *
 * Protocolo do 93C56 (modo 16 bits):
 *   Após CS subir: jogo envia 11 bits = 1 start + 2 opcode + 8 endereço.
 *   READ  (op=10): após 11 bits, saem 16 bits de dados (MSB primeiro).
 *   WRITE (op=01): após 11 bits, chegam 16 bits de dados, então armazenados.
 *   ERASE (op=11): apaga a palavra no endereço (escreve 0xFFFF).
 *   EWEN  (op=00, addr[7:6]=11): habilita escrita.
 *   EWDS  (op=00, addr[7:6]=00): desabilita escrita.
 *   ERAL  (op=00, addr[7:6]=10): apaga todas as palavras.
 *   WRAL  (op=00, addr[7:6]=01): escreve a mesma palavra em todos os endereços.
 */
static void gb_mbc7_eeprom_write(struct gb *gb, uint8_t val)
{
     struct gb_mbc7 *m = &gb->cart.mbc7;
     uint8_t cs  = (val >> 7) & 1;
     uint8_t clk = (val >> 6) & 1;
     uint8_t din = (val >> 1) & 1;

     /* Borda de subida do CS: inicia nova transação */
     if (!m->cs && cs)
     {
          m->recv       = 0;
          m->recv_count = 0;
          m->send_count = 0;
          m->wdata_count = 0;
          m->dout       = 0;
     }

     /* Borda de descida do CS: encerra a transação */
     if (m->cs && !cs)
     {
          m->send_count  = 0;
          m->wdata_count = 0;
          m->dout        = 1;
     }

     /* Processa bit na borda de subida do CLK (com CS alto) */
     if (cs && clk && !m->clk)
     {
          if (m->send_count > 0)
          {
               /* Modo READ: desloca próximo bit de saída */
               m->dout = (m->send >> 15) & 1;
               m->send = (uint16_t)(m->send << 1);
               m->send_count--;
               if (m->send_count == 0)
               {
                    m->dout = 1; /* ready */
               }
          }
          else if (m->wdata_count > 0)
          {
               /* Recebendo dados para WRITE ou WRAL */
               m->wdata = (uint16_t)((m->wdata << 1) | din);
               m->wdata_count--;

               if (m->wdata_count == 0)
               {
                    /* Escrita concluída */
                    if (m->write_enabled)
                    {
                         if (m->wral)
                         {
                              unsigned i;
                              for (i = 0; i < 128; i++)
                              {
                                   gb->cart.mbc7.eeprom[i * 2]     = m->wdata >> 8;
                                   gb->cart.mbc7.eeprom[i * 2 + 1] = m->wdata & 0xff;
                              }
                         }
                         else
                         {
                              gb->cart.mbc7.eeprom[m->waddr * 2]     = m->wdata >> 8;
                              gb->cart.mbc7.eeprom[m->waddr * 2 + 1] = m->wdata & 0xff;
                         }
                         gb->cart.dirty_ram = true;
                    }
                    m->dout = 1; /* ready */
               }
          }
          else
          {
               /* Recebendo bits do comando (máximo 11) */
               m->recv = (m->recv << 1) | din;
               m->recv_count++;

               if (m->recv_count == 11)
               {
                    uint8_t start = (m->recv >> 10) & 1;
                    uint8_t op    = (m->recv >>  8) & 3;
                    uint8_t adr   = (uint8_t)(m->recv & 0xff);

                    m->recv_count = 0;

                    if (!start)
                    {
                         /* Start-bit inválido — ignora */
                    }
                    else if (op == 2) /* READ */
                    {
                         m->send = (uint16_t)((gb->cart.mbc7.eeprom[adr * 2] << 8) |
                                               gb->cart.mbc7.eeprom[adr * 2 + 1]);
                         m->send_count = 16;
                         m->dout = 0;
                    }
                    else if (op == 1) /* WRITE */
                    {
                         m->waddr      = adr;
                         m->wdata      = 0;
                         m->wdata_count = 16;
                         m->wral       = false;
                         m->dout       = 0;
                    }
                    else if (op == 3) /* ERASE */
                    {
                         if (m->write_enabled)
                         {
                              gb->cart.mbc7.eeprom[adr * 2]     = 0xff;
                              gb->cart.mbc7.eeprom[adr * 2 + 1] = 0xff;
                              gb->cart.dirty_ram = true;
                         }
                         m->dout = 1;
                    }
                    else /* op == 0: comandos especiais */
                    {
                         uint8_t sub = (adr >> 6) & 3;

                         if (sub == 3) /* EWEN */
                         {
                              m->write_enabled = true;
                         }
                         else if (sub == 0) /* EWDS */
                         {
                              m->write_enabled = false;
                         }
                         else if (sub == 2) /* ERAL */
                         {
                              if (m->write_enabled)
                              {
                                   memset(gb->cart.mbc7.eeprom, 0xff, 256);
                                   gb->cart.dirty_ram = true;
                              }
                         }
                         else /* sub == 1: WRAL */
                         {
                              m->wdata       = 0;
                              m->wdata_count = 16;
                              m->wral        = true;
                              m->dout        = 0;
                         }
                         if (sub != 1)
                         {
                              m->dout = 1;
                         }
                    }
               }
          }
     }

     m->cs  = cs;
     m->clk = clk;
     m->din = din;
}

static bool gb_huc3_write_io(struct gb *gb, uint8_t v)
{
     struct gb_huc3 *h = &gb->cart.huc3;

     switch (h->mode)
     {
     case 0x0:
     case 0x0a:
          return false;

     case 0x0b:
          switch (v >> 4)
          {
          case 1:
               gb_huc3_sync_clock(gb);
               if (h->access_index < 3)
               {
                    h->read = (h->minutes >> (h->access_index * 4)) & 0x0f;
               }
               else if (h->access_index < 7)
               {
                    h->read = (h->days >> ((h->access_index - 3) * 4)) & 0x0f;
               }
               h->access_index++;
               break;

          case 2:
          case 3:
               if (h->access_index < 3)
               {
                    h->minutes &= (uint16_t)~(0x0f << (h->access_index * 4));
                    h->minutes |= (uint16_t)((v & 0x0f) << (h->access_index * 4));
                    gb_huc3_reset_clock_epoch(gb);
                    gb->cart.dirty_ram = true;
               }
               else if (h->access_index < 7)
               {
                    h->days &= (uint16_t)~(0x0f << ((h->access_index - 3) * 4));
                    h->days |= (uint16_t)((v & 0x0f) << ((h->access_index - 3) * 4));
                    gb_huc3_reset_clock_epoch(gb);
                    gb->cart.dirty_ram = true;
               }
               else if (h->access_index >= 0x58 && h->access_index <= 0x5a)
               {
                    h->alarm_minutes &= (uint16_t)~(0x0f << ((h->access_index - 0x58) * 4));
                    h->alarm_minutes |= (uint16_t)((v & 0x0f) << ((h->access_index - 0x58) * 4));
                    gb->cart.dirty_ram = true;
               }
               else if (h->access_index >= 0x5b && h->access_index <= 0x5e)
               {
                    h->alarm_days &= (uint16_t)~(0x0f << ((h->access_index - 0x5b) * 4));
                    h->alarm_days |= (uint16_t)((v & 0x0f) << ((h->access_index - 0x5b) * 4));
                    gb->cart.dirty_ram = true;
               }
               else if (h->access_index == 0x5f)
               {
                    h->alarm_enabled = v & 1;
                    gb->cart.dirty_ram = true;
               }

               if ((v >> 4) == 3)
               {
                    h->access_index++;
               }
               break;

          case 4:
               h->access_index &= 0xf0;
               h->access_index |= v & 0x0f;
               break;

          case 5:
               h->access_index &= 0x0f;
               h->access_index |= (v & 0x0f) << 4;
               break;

          case 6:
               h->access_flags = v & 0x0f;
               break;

          default:
               break;
          }
          return true;

     case 0x0c:
     case 0x0d:
          return true;

     case 0x0e:
          h->ir_out = v & 1;
          return true;

     default:
          return true;
     }
}

/*
 * gb_cart_rom_readb — lê um byte da ROM do cartucho.
 *
 * A CPU enxerga a ROM em duas janelas de 16 KB:
 *   0x0000–0x3FFF  Banco 0 (sempre fixo, não comutável)
 *   0x4000–0x7FFF  Banco comutável (controlado pelo MBC)
 *
 * @addr: endereço da CPU no intervalo 0x0000–0x7FFF (bits altos removidos pelo chamador).
 */
uint8_t gb_cart_rom_readb(struct gb *gb, uint16_t addr)
{
     struct gb_cart *cart = &gb->cart;
     unsigned rom_off = addr;

     switch (cart->model)
     {
     case GB_CART_SIMPLE:
          /* Sem mapeador — o endereço mapeia diretamente para o offset na ROM. */
          break;

     case GB_CART_MBC1:
          if (addr < GB_ROM_BANK_SIZE)
          {
               /* No modo de bancos de RAM (modo 1), os bits altos do registrador
                * 0x4000-0x5FFF também afetam a janela "fixa" (0x0000-0x3FFF),
                * permitindo acessar os blocos 0x00/0x20/0x40/0x60.
                * No modo de bancos de ROM (modo 0) a janela fixa é sempre banco 0. */
               if (cart->mbc1_bank_ram)
               {
                    unsigned bank;
                    if (cart->mbc1_multicart)
                    {
                         /* MBC1M: bits [5:4] do registrador secundario selecionam
                          * o sub-jogo; janela fixa aponta para inicio desse sub-jogo. */
                         bank = (cart->cur_rom_bank >> 1) & 0x30;
                    }
                    else
                    {
                         bank = cart->cur_rom_bank & 0x60;
                    }
                    rom_off = (bank * GB_ROM_BANK_SIZE) + (addr & (GB_ROM_BANK_SIZE - 1));
                    rom_off %= cart->rom_length;
               }
          }
          else
          {
               /*
                * O MBC1 usa um número de banco de 7 bits (ou 5 bits no MBC1M):
                *   bits [4:0] — definidos por escritas em 0x2000–0x3FFF
                *   bits [6:5] — definidos por escritas em 0x4000–0x5FFF
                *
                * A janela comutável usa todos os bits do banco de ROM nos dois modos.
                * O modo de bancos de RAM muda apenas a janela fixa e o banco de RAM.
                *
                * O MBC1M usa apenas 4 bits baixos + 2 bits de seleção de sub-jogo.
                *
                * O banco 0 não pode ser mapeado na janela comutável; qualquer tentativa
                * de selecionar o banco 0 seleciona silenciosamente o banco 1.
                */
               unsigned bank = cart->cur_rom_bank;

               if (cart->mbc1_multicart)
               {
                    /* MBC1M: 4 bits baixos para banco dentro do sub-jogo,
                     * bits [5:4] para seleção do sub-jogo. */
                    bank = (cart->cur_rom_bank & 0x0f) |
                           ((cart->cur_rom_bank >> 1) & 0x30);
                    if ((cart->cur_rom_bank & 0x1f) == 0)
                    {
                         bank++;
                    }
               }
               else if (cart->mbc1_bank_ram)
               {
                    /* Mesmo no modo RAM, a janela 0x4000-0x7fff usa os
                     * bits altos; o modo só afeta a janela fixa e a RAM. */
                    bank &= 0x7f;
                    if ((bank & 0x1f) == 0)
                    {
                         bank++;
                    }
               }
               else
               {
                    /* Modo ROM: 7 bits completos (128 bancos max) */
                    bank &= 0x7f;
                    if ((bank & 0x1f) == 0)
                    {
                         /* Bancos 0x00/0x20/0x40/0x60 são inacessíveis na janela
                          * comutável; hardware força bit 0. */
                         bank++;
                     }
               }

               rom_off = (bank * GB_ROM_BANK_SIZE) + (addr & (GB_ROM_BANK_SIZE - 1));
               rom_off %= cart->rom_length;
          }
          break;

     case GB_CART_MBC2:
          if (addr >= GB_ROM_BANK_SIZE)
          {
               unsigned bank = cart->cur_rom_bank & 0x0f;
               if ((bank & 0x0f) == 0)
               {
                    bank = 1;
               }
               rom_off = (bank * GB_ROM_BANK_SIZE) + (addr & (GB_ROM_BANK_SIZE - 1));
               rom_off %= cart->rom_length;
          }
          break;

     case GB_CART_MBC3:
          if (addr >= GB_ROM_BANK_SIZE)
          {
               /* Bancos lineares simples: cur_rom_bank mapeia diretamente na ROM. */
               unsigned bank = cart->cur_rom_bank % cart->rom_banks;
               if (bank == 0)
               {
                    bank = 1;
               }
               rom_off += (bank - 1) * GB_ROM_BANK_SIZE;
          }
          break;

     case GB_CART_MBC5:
          if (addr >= GB_ROM_BANK_SIZE)
          {
               /*
                * O MBC5 é o único controlador que consegue mapear o banco 0 na janela
                * comutável (diferente do MBC1/3, onde banco 0 vira banco 1).
                * cur_rom_bank é um valor de 9 bits (bits [7:0] de 0x2000–0x2FFF e bit 8
                * de 0x3000–0x3FFF), suportando até 512 bancos.
                */
               unsigned bank = cart->cur_rom_bank % cart->rom_banks;

               rom_off -= GB_ROM_BANK_SIZE;
               rom_off += bank * GB_ROM_BANK_SIZE;
          }
          break;

     case GB_CART_MBC7:
          if (addr >= GB_ROM_BANK_SIZE)
          {
               /* MBC7: ROM banking simples de 8 bits, banco 0 permitido */
               unsigned bank = cart->cur_rom_bank % cart->rom_banks;

               rom_off -= GB_ROM_BANK_SIZE;
               rom_off += bank * GB_ROM_BANK_SIZE;
          }
          break;

     case GB_CART_HUC1:
     case GB_CART_HUC3:
          if (addr >= GB_ROM_BANK_SIZE)
          {
               /* HuC1/HuC3 permitem mapear banco 0 na janela comutável. */
               unsigned bank = cart->cur_rom_bank % cart->rom_banks;

               rom_off -= GB_ROM_BANK_SIZE;
               rom_off += bank * GB_ROM_BANK_SIZE;
          }
          break;

     default:
          /* Não deve ser atingido */
          die();
     }

     return cart->rom[rom_off];
}

/*
 * gb_cart_rom_writeb — trata uma escrita da CPU na região de ROM (0x0000–0x7FFF).
 *
 * A ROM do Game Boy é, evidentemente, somente leitura; escritas nessa região são
 * interceptadas pelo chip MBC para configurar registradores de banco, habilitar/desabilitar
 * a RAM ou mudar o modo de bancos. Cada MBC tem seu próprio mapa de registradores.
 *
 * @addr: endereço da CPU 0x0000–0x7FFF.
 * @v:    byte escrito pela CPU.
 *
 * Mapa de registradores do MBC1:
 *   0x0000–0x1FFF  Habilitar RAM    (escrever 0x0A para habilitar, qualquer outro desabilita)
 *   0x2000–0x3FFF  Banco de ROM baixo  (bits [4:0])
 *   0x4000–0x5FFF  Banco de ROM alto / banco de RAM  (bits [6:5] ou banco de RAM [1:0])
 *   0x6000–0x7FFF  Modo de bancos  (0 = bancos de ROM, 1 = bancos de RAM)
 *
 * Mapa de registradores do MBC2:
 *   0x0000–0x1FFF  Habilitar RAM   (bit 8 do endereço deve ser 0)
 *   0x2000–0x3FFF  Banco de ROM    (4 bits baixos, bit 8 do endereço deve ser 1)
 *
 * Mapa de registradores do MBC3:
 *   0x0000–0x1FFF  Habilitar RAM/RTC
 *   0x2000–0x3FFF  Banco de ROM    (7 bits)
 *   0x4000–0x5FFF  Banco de RAM / seleção de registrador RTC (0–3 = RAM, 0x08–0x0C = RTC)
 *   0x6000–0x7FFF  Trava do RTC    (não emulado)
 *
 * Mapa de registradores do MBC5:
 *   0x0000–0x1FFF  Habilitar RAM
 *   0x2000–0x2FFF  Banco de ROM baixo  (bits [7:0])
 *   0x3000–0x3FFF  Banco de ROM alto   (bit 8)
 *   0x4000–0x5FFF  Banco de RAM        (4 bits)
 */
void gb_cart_rom_writeb(struct gb *gb, uint16_t addr, uint8_t v)
{
     struct gb_cart *cart = &gb->cart;

     switch (cart->model)
     {
     case GB_CART_SIMPLE:
          /* Nada a fazer */
          break;

     case GB_CART_MBC1:
          if (addr < 0x2000)
          {
               /* Habilitar RAM: o nibble inferior deve ser igual a 0xA para desbloquear. */
               cart->ram_write_protected = ((v & 0xf) != 0xa);
          }
          else if (addr < 0x4000)
          {
               /* Define banco de ROM, bits [4:0] */
               cart->cur_rom_bank &= ~0x1f;
               cart->cur_rom_bank |= v & 0x1f;
          }
          else if (addr < 0x6000)
          {
               /*
                * Os bits [1:0] de v são usados para os bits altos do banco de ROM [6:5]
                * ou para a seleção do banco de RAM, dependendo de mbc1_bank_ram.
                * Armazenamos ambos simultaneamente e aplicamos o modo na leitura.
                */
               cart->cur_rom_bank &= 0x1f;
               cart->cur_rom_bank |= (v & 3) << 5;

               if (cart->ram_banks > 0)
               {
                    cart->cur_ram_bank = (v & 3) % cart->ram_banks;
               }
          }
          else
          {
               /* Altera o modo de bancos do MBC1 */
               cart->mbc1_bank_ram = v & 1;
          }
          break;

     case GB_CART_MBC2:
          /* O MBC2 so responde a escritas em 0x0000-0x3FFF.
           * Dentro desse range, o bit 8 do endereço (nao o range) determina a funcao:
           *   bit 8 == 1: seleciona banco de ROM (4 bits baixos, banco 0 vai para 1)
           *   bit 8 == 0: controla RAM enable (nibble baixo 0xA = habilita) */
          if (addr < 0x4000)
          {
               if (addr & 0x0100)
               {
                    cart->cur_rom_bank = v & 0xf;
                    if (cart->cur_rom_bank == 0)
                    {
                         cart->cur_rom_bank = 1;
                    }
               }
               else
               {
                    cart->ram_write_protected = ((v & 0xf) != 0xa);
               }
          }
          break;

     case GB_CART_MBC3:
          if (addr < 0x2000)
          {
               cart->ram_write_protected = ((v & 0xf) != 0xa);
          }
          else if (addr < 0x4000)
          {
               /* Define banco de ROM (7 bits; banco 0 vira banco 1). */
               cart->cur_rom_bank = (v & 0x7f) % cart->rom_banks;
               if (cart->cur_rom_bank == 0)
               {
                    cart->cur_rom_bank = 1;
               }
          }
          else if (addr < 0x6000)
          {
               /* Valores 0x00–0x03 = banco de RAM; 0x08–0x0C = registrador RTC */
               cart->cur_ram_bank = v;
          }
          else if (addr < 0x8000)
          {
               /* Latch do RTC: a transição 0→1 captura a hora atual */
               if (cart->has_rtc)
               {
                    gb_rtc_latch(gb, v == 1);
               }
          }
          break;

     case GB_CART_MBC5:
          if (addr < 0x2000)
          {
               cart->ram_write_protected = ((v & 0xf) != 0xa);
          }
          else if (addr < 0x3000)
          {
               /* Define banco de ROM, 8 bits baixos */
               cart->cur_rom_bank &= 0x100;
               cart->cur_rom_bank |= v;
          }
          else if (addr < 0x4000)
          {
               /* Define banco de ROM, MSB (apenas bit 8 — 9º bit para suporte a 512 bancos). */
               cart->cur_rom_bank &= 0xff;
               cart->cur_rom_bank |= (v & 1) << 8;
          }
          else if (addr < 0x6000)
          {
               /* Define banco de RAM */
               if (cart->ram_banks > 0)
               {
                    uint8_t bank_mask = cart->has_rumble ? 0x07 : 0x0f;
                    cart->cur_ram_bank = (v & bank_mask) % cart->ram_banks;
               }
          }
          break;

     case GB_CART_MBC7:
          if (addr < 0x2000)
          {
               /* Primeiro passo de desbloqueio: escrever 0x0A */
               cart->mbc7.ram_enable_1 = ((v & 0x0f) == 0x0a);
          }
          else if (addr < 0x4000)
          {
               /* Banco de ROM (8 bits, permite banco 0) */
               cart->cur_rom_bank = v % cart->rom_banks;
          }
          else if (addr < 0x6000)
          {
               /* Segundo passo de desbloqueio: escrever 0x40 */
               cart->mbc7.ram_enable_2 = (v == 0x40);
          }
          /* 0x6000-0x7FFF: não utilizado pelo MBC7 */
          break;

     case GB_CART_HUC1:
          if (addr < 0x2000)
          {
               /* HuC1 não desabilita RAM; este registrador alterna RAM/IR. */
               cart->huc1.ir_mode = ((v & 0x0f) == 0x0e);
               cart->ram_write_protected = false;
          }
          else if (addr < 0x4000)
          {
               cart->cur_rom_bank = (v & 0x3f) % cart->rom_banks;
          }
          else if (addr < 0x6000)
          {
               if (cart->ram_banks > 0)
               {
                    cart->cur_ram_bank = (v & 0x03) % cart->ram_banks;
               }
          }
          /* 0x6000-0x7FFF: sem efeito conhecido no HuC1 */
          break;

     case GB_CART_HUC3:
          if (addr < 0x2000)
          {
               cart->huc3.mode = v & 0x0f;
               cart->ram_write_protected = (cart->huc3.mode != 0x0a);
          }
          else if (addr < 0x4000)
          {
               cart->cur_rom_bank = (v & 0x7f) % cart->rom_banks;
          }
          else if (addr < 0x6000)
          {
               if (cart->ram_banks > 0)
               {
                    cart->cur_ram_bank = (v & 0x03) % cart->ram_banks;
               }
          }
          /* 0x6000-0x7FFF: sem efeito conhecido no HuC3 */
          break;

     default:
          /* Não deve ser atingido */
          die();
     }
}

/*
 * gb_cart_mbc1_ram_off — calcula o offset físico na RAM para o MBC1.
 *
 * O banqueamento de RAM do MBC1 é mais complexo que o de outros MBCs porque
 * o modo de bancos afeta quais bancos são acessíveis:
 *
 *  - Modo de bancos de ROM (mbc1_bank_ram == false): apenas o banco 0 é acessível.
 *  - Modo de bancos de RAM (mbc1_bank_ram == true):  até 4 bancos acessíveis.
 *
 * Cartuchos com um único banco e chip parcial de 2 KB são espelhados na janela de 8 KB.
 *
 * @addr: endereço dentro da janela 0xA000–0xBFFF, já convertido para base 0.
 * Retorna o offset em bytes dentro de cart->ram.
 */
unsigned gb_cart_mbc1_ram_off(struct gb *gb, uint16_t addr)
{
     struct gb_cart *cart = &gb->cart;
     unsigned bank;

     if (cart->ram_banks == 1)
     {
          /* Cartuchos com apenas um banco de RAM podem ter um chip parcial de
           * 2 KB que é espelhado 4 vezes na janela. */
          return addr % cart->ram_length;
     }

     bank = cart->cur_ram_bank;

     if (cart->mbc1_bank_ram)
     {
          bank %= 4;
     }
     else
     {
          /* Neste modo, apenas um banco é suportado */
          bank = 0;
     }

     return bank * GB_RAM_BANK_SIZE + addr;
}

/*
 * gb_cart_ram_readb — lê um byte da RAM do cartucho.
 *
 * A CPU acessa a RAM externa pela janela 0xA000–0xBFFF (8 KB).
 * Se a RAM estiver protegida contra escrita ou o cartucho não tiver RAM,
 * a leitura retorna 0xFF (valor de barramento aberto no hardware real).
 *
 * @addr: endereço dentro da janela, já convertido para offset base 0 pelo chamador.
 */
uint8_t gb_cart_ram_readb(struct gb *gb, uint16_t addr)
{
     struct gb_cart *cart = &gb->cart;
     unsigned ram_off;

     switch (cart->model)
     {
     case GB_CART_SIMPLE:
          if (cart->ram_banks == 0 || cart->ram_write_protected)
          {
               return 0xff;
          }

          ram_off = addr % cart->ram_length;
          break;

     case GB_CART_MBC1:
          if (cart->ram_banks == 0 || cart->ram_write_protected)
          {
               return 0xff;
          }

          ram_off = gb_cart_mbc1_ram_off(gb, addr);
          break;

     case GB_CART_MBC2:
          if (cart->ram_write_protected)
          {
               return 0xff;
          }
          /* A RAM de 512 nibbles do MBC2 envolve em 512 bytes.
           * Os 4 bits superiores de cada byte sempre leem como 1. */
          return cart->ram[addr % 512] | 0xf0;

     case GB_CART_MBC3:
          if (cart->cur_ram_bank <= 3)
          {
               /* Acesso à RAM */
               if (cart->ram_banks == 0)
               {
                    return 0xff;
               }
               ram_off = (cart->cur_ram_bank % cart->ram_banks) * GB_RAM_BANK_SIZE + addr;
          }
          else
          {
               /* Acesso ao RTC — só quando RAM não está protegida */
               if (cart->has_rtc && !cart->ram_write_protected)
               {
                    return gb_rtc_read(gb, cart->cur_ram_bank);
               }
               return 0xff;
          }
          break;

     case GB_CART_MBC5:
          if (cart->ram_banks == 0)
          {
               /* Sem RAM */
               return 0xff;
          }

          ram_off = cart->cur_ram_bank * GB_RAM_BANK_SIZE + addr;
          break;

     case GB_CART_MBC7:
     {
          struct gb_mbc7 *m = &cart->mbc7;

          /* Ambos os desbloqueios são necessários para acessar 0xA000-0xBFFF */
          if (!m->ram_enable_1 || !m->ram_enable_2)
          {
               return 0xff;
          }

          switch (addr)
          {
          case 0x00: /* Acelerômetro X, byte baixo */
               return m->accel_latched ? (uint8_t)(m->accel_x & 0xff) : 0xff;
          case 0x01: /* Acelerômetro X, byte alto */
               return m->accel_latched ? (uint8_t)(m->accel_x >> 8) : 0xff;
          case 0x02: /* Acelerômetro Y, byte baixo */
               return m->accel_latched ? (uint8_t)(m->accel_y & 0xff) : 0xff;
          case 0x03: /* Acelerômetro Y, byte alto */
               return m->accel_latched ? (uint8_t)(m->accel_y >> 8) : 0xff;
          case 0x04: return 0x00; /* reservado */
          case 0x05: return 0xff; /* reservado */
          case 0x06: return 0x00; /* reservado */
          case 0x07: return 0xff; /* reservado */
          case 0x08:
               /* Byte de controle da EEPROM:
                *   bit 7: CS (echo)   bit 6: CLK (echo)
                *   bit 4: DOUT        bit 1: DIN (echo)  */
               return (uint8_t)((m->cs << 7) | (m->clk << 6) |
                                (m->dout << 4) | (m->din << 1));
          default:
               return 0xff;
          }
     }

     case GB_CART_HUC1:
          if (cart->huc1.ir_mode)
          {
               /* Sem link IR externo conectado: receptor em repouso. */
               return 0xc0;
          }

          if (cart->ram_banks == 0)
          {
               return 0xff;
          }

          ram_off = (cart->cur_ram_bank % cart->ram_banks) * GB_RAM_BANK_SIZE + addr;
          break;

     case GB_CART_HUC3:
          switch (cart->huc3.mode)
          {
          case 0x00:
          case 0x0a:
               if (cart->ram_banks == 0)
               {
                    return 0xff;
               }
               ram_off = (cart->cur_ram_bank % cart->ram_banks) * GB_RAM_BANK_SIZE + addr;
               break;

          case 0x0c:
               if (cart->huc3.access_flags == 0x02)
               {
                    return 0x01;
               }
               return cart->huc3.read;

          case 0x0d:
               return 0x01;

          case 0x0e:
               return 0xc0;

          default:
               return 0xff;
          }
          break;

     default:
          /* Não deve ser atingido */
          die();
          return 0xff;
     }

     return cart->ram[ram_off];
}

/*
 * gb_cart_ram_writeb — escreve um byte na RAM do cartucho.
 *
 * Escritas são silenciosamente descartadas se a RAM estiver protegida (os jogos
 * devem primeiro desbloquear a RAM escrevendo 0x0A em 0x0000–0x1FFF).
 *
 * Após uma escrita bem-sucedida, marcamos a RAM como suja e agendamos um save
 * diferido (~3 segundos de tempo de CPU). Agendar a partir de cada escrita seria
 * custoso; a abordagem diferida deixa o jogo concluir um save multi-byte antes
 * de descarregarmos para o disco.
 *
 * @addr: endereço dentro da janela 0xA000–0xBFFF, já convertido para base 0.
 * @v:    byte a ser escrito.
 */
void gb_cart_ram_writeb(struct gb *gb, uint16_t addr, uint8_t v)
{
     struct gb_cart *cart = &gb->cart;
     unsigned ram_off;

     /* MBC7 e HuC3 têm mecanismos próprios de acesso à região 0xA000-0xBFFF;
      * a flag genérica ram_write_protected não pode bloquear seus registradores. */
     if (cart->ram_write_protected &&
         cart->model != GB_CART_MBC7 &&
         cart->model != GB_CART_HUC3)
     {
          return;
     }

     switch (cart->model)
     {
     case GB_CART_SIMPLE:
          if (cart->ram_banks == 0)
          {
               /* Sem RAM */
               return;
          }

          ram_off = addr % cart->ram_length;
          break;

     case GB_CART_MBC1:
          if (cart->ram_banks == 0)
          {
               /* Sem RAM */
               return;
          }

          ram_off = gb_cart_mbc1_ram_off(gb, addr);
          break;

     case GB_CART_MBC2:
          /* A RAM do MBC2 é de 512 nibbles; apenas os 4 bits baixos são armazenados. */
          ram_off = addr % 512;
          v &= 0x0f;
          break;

     case GB_CART_MBC3:
          if (cart->cur_ram_bank <= 3)
          {
               /* Acesso à RAM */
               if (cart->ram_banks == 0)
               {
                    return;
               }
               ram_off = (cart->cur_ram_bank % cart->ram_banks) * GB_RAM_BANK_SIZE + addr;
          }
          else
          {
               /* Acesso ao RTC */
               if (cart->has_rtc)
               {
                    gb_rtc_write(gb, cart->cur_ram_bank, v);
                    cart->dirty_ram = true;
               }
               goto write_done;
          }
          break;

     case GB_CART_MBC5:
          if (cart->ram_banks == 0)
          {
               /* Sem RAM */
               return;
          }

          ram_off = cart->cur_ram_bank * GB_RAM_BANK_SIZE + addr;
          break;

     case GB_CART_MBC7:
     {
          struct gb_mbc7 *m = &cart->mbc7;

          /* O MBC7 tem seu próprio protocolo de desbloqueio e ignora
           * a flag ram_write_protected do caminho convencional. */
          if (!m->ram_enable_1 || !m->ram_enable_2)
          {
               return;
          }

          if (addr == 0x00)
          {
               /* Passo 1 do latch do acelerômetro: escrever 0x55 */
               if (v == 0x55)
               {
                    m->accel_latch_prepare = true;
               }
          }
          else if (addr == 0x10)
          {
               /* Passo 2 do latch: escrever 0xAA confirma a captura */
               if (v == 0xaa && m->accel_latch_prepare)
               {
                    m->accel_latched       = true;
                    m->accel_x             = 0x8000; /* neutro — sem inclinação */
                    m->accel_y             = 0x8000;
                    m->accel_latch_prepare = false;
               }
          }
          else if (addr == 0x08)
          {
               /* Interface bit-banged da EEPROM 93C56 */
               gb_mbc7_eeprom_write(gb, v);
          }
          goto write_done;
     }

     case GB_CART_HUC1:
          if (cart->huc1.ir_mode)
          {
               cart->huc1.ir_out = v & 1;
               return;
          }

          if (cart->ram_banks == 0)
          {
               return;
          }

          ram_off = (cart->cur_ram_bank % cart->ram_banks) * GB_RAM_BANK_SIZE + addr;
          break;

     case GB_CART_HUC3:
          if (gb_huc3_write_io(gb, v))
          {
               goto write_done;
          }

          if (cart->huc3.mode == 0x00)
          {
               return;
          }

          if (cart->huc3.mode != 0x0a || cart->ram_banks == 0)
          {
               return;
          }

          ram_off = (cart->cur_ram_bank % cart->ram_banks) * GB_RAM_BANK_SIZE + addr;
          break;

     default:
          /* Não deve ser atingido */
          die();
     }

     cart->ram[ram_off] = v;

write_done:
     if (cart->save_file)
     {
          cart->dirty_ram = true;
          /*
           * Agenda um save diferido daqui a 3 segundos (em ciclos de CPU).
           * Se o jogo escrever novamente antes do prazo, gb_sync_next empurrará
           * o prazo para frente, evitando saves parciais prematuros.
           */
          gb_sync_next(gb, GB_SYNC_CART, 3 * GB_CPU_FREQ_HZ);
     }
}
