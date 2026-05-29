#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "gba.h"

/* Detect backup type by scanning ROM for Nintendo SDK strings */
static enum gba_backup_type detect_backup(const uint8_t *rom, uint32_t size)
{
     static const struct
     {
          const char *str;
          enum gba_backup_type type;
     } probes[] = {
         {"EEPROM_V", GBA_BACKUP_EEPROM_512},
         {"SRAM_V", GBA_BACKUP_SRAM},
         {"SRAM_F_V", GBA_BACKUP_SRAM},
         {"FLASH_V", GBA_BACKUP_FLASH_512},
         {"FLASH512_V", GBA_BACKUP_FLASH_512},
         {"FLASH1M_V", GBA_BACKUP_FLASH_1M},
         {NULL, GBA_BACKUP_NONE}};
     unsigned i;
     uint32_t end = size > 0 ? size - 8 : 0;
     uint32_t a;

     for (a = 0; a < end; a++)
     {
          for (i = 0; probes[i].str; i++)
          {
               size_t len = strlen(probes[i].str);
               if (a + len <= size && memcmp(rom + a, probes[i].str, len) == 0)
                    return probes[i].type;
          }
     }
     return GBA_BACKUP_NONE;
}

static bool detect_rtc(const uint8_t *rom, uint32_t size)
{
     static const char rtc_str[] = "SIIRTC_V";
     uint32_t end = size > (uint32_t)(sizeof(rtc_str) - 1)
                        ? size - (uint32_t)(sizeof(rtc_str) - 1)
                        : 0;
     for (uint32_t a = 0; a < end; a++)
          if (memcmp(rom + a, rtc_str, sizeof(rtc_str) - 1) == 0)
               return true;
     return false;
}

static void eeprom_reset(struct gba_cart *cart)
{
     memset(cart->eeprom.data, 0xFF, sizeof(cart->eeprom.data));
     cart->eeprom.command = 0;
     cart->eeprom.read_bits_remaining = 0;
     cart->eeprom.read_address = 0;
     cart->eeprom.write_address = 0;
     cart->eeprom.settling_until = 0;
}

static void eeprom_ensure_size(struct gba_cart *cart, uint32_t byte_addr)
{
     if (byte_addr >= 512 && cart->backup_type == GBA_BACKUP_EEPROM_512)
          cart->backup_type = GBA_BACKUP_EEPROM_8K;
}

/* -------------------------------------------------------------------------
 * RTC helpers
 * ---------------------------------------------------------------------- */

static uint8_t rtc_to_bcd(uint8_t v)
{
     v = v % 100;
     return (uint8_t)((v / 10) * 16 + (v % 10));
}

/*
 * Called on every write to 0x080000C4 when the RTC is present.
 * Implements the Seiko S-3511A serial protocol used on GBA carts:
 *   - CS  = bit 2 of gpio_direction / value
 *   - SCK = bit 0
 *   - SIO = bit 1
 *
 * On each rising edge of SCK (bit 0 goes 0→1) one bit of data is clocked
 * in or out MSB-first.
 */
static void rtc_gpio_write(struct gba_cart *cart, uint16_t new_val)
{
     struct gba_rtc *rtc = &cart->rtc;
     uint16_t old_val = rtc->byte0;

     /* CS (bit 2): low→high starts a transaction, high→low resets to IDLE */
     bool old_cs = (old_val & 4) != 0;
     bool new_cs = (new_val & 4) != 0;
     bool old_sck = (old_val & 1) != 0;
     bool new_sck = (new_val & 1) != 0;

     rtc->byte0 = (uint8_t)new_val;

     /* CS falling edge → reset */
     if (old_cs && !new_cs)
     {
          rtc->state = RTC_IDLE;
          rtc->bits = 0;
          rtc->command = 0;
          return;
     }

     /* Start condition: CS high, SCK was low, value has SCK+CS set */
     if (!old_cs && new_cs)
     {
          /* beginning of a new transaction */
          rtc->state = RTC_COMMAND;
          rtc->bits = 0;
          rtc->command = 0;
          return;
     }

     if (!new_cs)
          return; /* CS not asserted, ignore */

     /* Rising edge of SCK = clock one bit */
     if (!old_sck && new_sck)
     {
          bool sio = (new_val & 2) != 0;

          switch (rtc->state)
          {

          case RTC_COMMAND:
               /* shift command byte MSB-first */
               rtc->command |= (uint8_t)((sio ? 1 : 0) << (7 - rtc->bits));
               rtc->bits++;
               if (rtc->bits == 8)
               {
                    rtc->bits = 0;
                    switch (rtc->command)
                    {
                    case 0x60: /* reset */
                         rtc->state = RTC_IDLE;
                         break;
                    case 0x62: /* write control (1 byte) */
                         rtc->state = RTC_READDATA;
                         rtc->data_len = 1;
                         break;
                    case 0x63: /* read control */
                         rtc->data[0] = 0x40;
                         rtc->data_len = 1;
                         rtc->state = RTC_DATA;
                         break;
                    case 0x65:
                    { /* read date+time (7 bytes) */
                         time_t t = time(NULL);
                         rtc->time = *localtime(&t);
                         rtc->data[0] = rtc_to_bcd((uint8_t)(rtc->time.tm_year % 100));
                         rtc->data[1] = rtc_to_bcd((uint8_t)(rtc->time.tm_mon + 1));
                         rtc->data[2] = rtc_to_bcd((uint8_t)rtc->time.tm_mday);
                         rtc->data[3] = rtc_to_bcd((uint8_t)rtc->time.tm_wday);
                         rtc->data[4] = rtc_to_bcd((uint8_t)rtc->time.tm_hour);
                         rtc->data[5] = rtc_to_bcd((uint8_t)rtc->time.tm_min);
                         rtc->data[6] = rtc_to_bcd((uint8_t)rtc->time.tm_sec);
                         rtc->data_len = 7;
                         rtc->state = RTC_DATA;
                         break;
                    }
                    case 0x67:
                    { /* read time only (3 bytes) */
                         time_t t = time(NULL);
                         rtc->time = *localtime(&t);
                         rtc->data[0] = rtc_to_bcd((uint8_t)rtc->time.tm_hour);
                         rtc->data[1] = rtc_to_bcd((uint8_t)rtc->time.tm_min);
                         rtc->data[2] = rtc_to_bcd((uint8_t)rtc->time.tm_sec);
                         rtc->data_len = 3;
                         rtc->state = RTC_DATA;
                         break;
                    }
                    default:
                         rtc->state = RTC_IDLE;
                         break;
                    }
               }
               break;

          case RTC_DATA:
               /* shift data out to game (SIO is output, bit 1) — update byte0 */
               {
                    int bit_idx = rtc->bits;
                    int byte_idx = bit_idx >> 3;
                    int bit_in_byte = bit_idx & 7;
                    uint8_t out = (rtc->data[byte_idx] >> bit_in_byte) & 1;
                    /* put the output bit on SIO (bit 1) */
                    rtc->byte0 = (uint8_t)((rtc->byte0 & ~2u) | (out << 1));
                    rtc->bits++;
                    if (rtc->bits == 8 * rtc->data_len)
                    {
                         rtc->bits = 0;
                         rtc->state = RTC_IDLE;
                    }
               }
               break;

          case RTC_READDATA:
               /* shift data in from game (SIO is input, bit 1) */
               {
                    int byte_idx = rtc->bits >> 3;
                    rtc->data[byte_idx] = (uint8_t)((rtc->data[byte_idx] >> 1) |
                                                    ((sio ? 1 : 0) << 7));
                    rtc->bits++;
                    if (rtc->bits == 8 * rtc->data_len)
                    {
                         rtc->bits = 0;
                         rtc->state = RTC_IDLE;
                    }
               }
               break;

          default:
               break;
          }
     }
}

/* Return the current SIO bit when game reads 0x080000C4 with RTC present */
static uint16_t rtc_gpio_read(const struct gba_cart *cart)
{
     return cart->rtc.byte0 & 0x0F;
}

bool gba_cart_load(struct gba *gba, const char *path)
{
     struct gba_cart *cart = &gba->cart;
     FILE *f;
     long size;

     f = fopen(path, "rb");
     if (!f)
     {
          perror(path);
          return false;
     }

     fseek(f, 0, SEEK_END);
     size = ftell(f);
     fseek(f, 0, SEEK_SET);

     if (size <= 0 || size > (long)GBA_ROM_MAX_SIZE)
     {
          fprintf(stderr, "GBA cart: invalid ROM size %ld\n", size);
          fclose(f);
          return false;
     }

     cart->rom = malloc((size_t)size);
     if (!cart->rom)
     {
          fclose(f);
          return false;
     }
     if (fread(cart->rom, 1, (size_t)size, f) != (size_t)size)
     {
          free(cart->rom);
          cart->rom = NULL;
          fclose(f);
          return false;
     }
     fclose(f);

     cart->rom_size = (uint32_t)size;
     cart->backup_type = detect_backup(cart->rom, cart->rom_size);
     cart->has_rtc = detect_rtc(cart->rom, cart->rom_size);
     cart->dirty = false;

     memset(cart->sram, 0xFF, sizeof(cart->sram));
     memset(cart->flash, 0xFF, sizeof(cart->flash));
     memset(&cart->flash_state, 0, sizeof(cart->flash_state));
     eeprom_reset(cart);
     memset(&cart->rtc, 0, sizeof(cart->rtc));

     /* Flash device ID: Panasonic 512KB */
     cart->flash_state.manufacturer = 0x32;
     cart->flash_state.device = 0x1B;

     fprintf(stderr, "GBA cart: loaded %u KB, backup=%d\n",
             cart->rom_size / 1024, cart->backup_type);

     /* Try to load save file */
     {
          char save_path[512];
          snprintf(save_path, sizeof(save_path), "%s.sav", path);
          cart->save_file = fopen(save_path, "r+b");
          if (!cart->save_file)
               cart->save_file = fopen(save_path, "w+b");
          if (cart->save_file)
          {
               switch (cart->backup_type)
               {
               case GBA_BACKUP_SRAM:
               {
                    size_t r = fread(cart->sram, 1, sizeof(cart->sram), cart->save_file);
                    (void)r;
                    break;
               }
               case GBA_BACKUP_FLASH_512:
               case GBA_BACKUP_FLASH_1M:
               {
                    size_t r = fread(cart->flash, 1, sizeof(cart->flash), cart->save_file);
                    (void)r;
                    break;
               }
               case GBA_BACKUP_EEPROM_512:
               {
                    size_t r = fread(cart->eeprom.data, 1, 512, cart->save_file);
                    (void)r;
                    break;
               }
               case GBA_BACKUP_EEPROM_8K:
               {
                    size_t r = fread(cart->eeprom.data, 1, 8192, cart->save_file);
                    (void)r;
                    break;
               }
               default:
                    break;
               }
          }
     }

     return true;
}

void gba_cart_unload(struct gba *gba)
{
     struct gba_cart *cart = &gba->cart;
     gba_cart_save(gba);
     if (cart->save_file)
     {
          fclose(cart->save_file);
          cart->save_file = NULL;
     }
     free(cart->rom);
     cart->rom = NULL;
     cart->rom_size = 0;
}

void gba_cart_save(struct gba *gba)
{
     struct gba_cart *cart = &gba->cart;
     if (!cart->dirty || !cart->save_file)
          return;
     rewind(cart->save_file);
     switch (cart->backup_type)
     {
     case GBA_BACKUP_SRAM:
          fwrite(cart->sram, 1, sizeof(cart->sram), cart->save_file);
          break;
     case GBA_BACKUP_FLASH_512:
     case GBA_BACKUP_FLASH_1M:
          fwrite(cart->flash, 1, sizeof(cart->flash), cart->save_file);
          break;
     case GBA_BACKUP_EEPROM_512:
          fwrite(cart->eeprom.data, 1, 512, cart->save_file);
          break;
     case GBA_BACKUP_EEPROM_8K:
          fwrite(cart->eeprom.data, 1, 8192, cart->save_file);
          break;
     default:
          break;
     }
     fflush(cart->save_file);
     cart->dirty = false;
}

void gba_cart_sync(struct gba *gba)
{
     gba_cart_save(gba);
     gba_sync_next(gba, GBA_SYNC_CART, GBA_SYNC_NEVER);
}

bool gba_cart_is_eeprom(struct gba *gba)
{
     return gba->cart.backup_type == GBA_BACKUP_EEPROM_512 ||
            gba->cart.backup_type == GBA_BACKUP_EEPROM_8K;
}

uint16_t gba_cart_eeprom_read(struct gba *gba)
{
     struct gba_cart *cart = &gba->cart;
     struct gba_cart_eeprom *ee = &cart->eeprom;

     if (ee->command != 4)
          return (ee->settling_until && gba->timestamp < ee->settling_until) ? 0 : 1;

     ee->read_bits_remaining--;
     if (ee->read_bits_remaining < 64)
     {
          int step = 63 - ee->read_bits_remaining;
          uint32_t byte_addr = (ee->read_address + (uint32_t)step) >> 3;
          eeprom_ensure_size(cart, byte_addr);
          if (byte_addr >= sizeof(ee->data))
               return 1;

          uint8_t data = ee->data[byte_addr];
          uint16_t bit = (uint16_t)((data >> (7 - (step & 7))) & 1);
          if (ee->read_bits_remaining == 0)
               ee->command = 0;
          return bit;
     }

     return 0;
}

void gba_cart_eeprom_write(struct gba *gba, uint16_t value, uint32_t write_size)
{
     struct gba_cart *cart = &gba->cart;
     struct gba_cart_eeprom *ee = &cart->eeprom;
     uint32_t bit = value & 1;

     switch (ee->command)
     {
     case 0: /* first command bit */
          ee->command = (uint8_t)bit;
          break;
     case 1: /* second command bit */
          ee->command = (uint8_t)((ee->command << 1) | bit);
          if (ee->command == 2)
               ee->write_address = 0;
          else if (ee->command == 3)
               ee->read_address = 0;
          else
               ee->command = 0;
          break;
     case 2: /* write command: address, 64 data bits, stop bit */
          if (write_size > 65)
          {
               ee->write_address <<= 1;
               ee->write_address |= bit << 6;
          }
          else if (write_size == 1)
          {
               ee->command = 0;
          }
          else
          {
               uint32_t byte_addr = ee->write_address >> 3;
               eeprom_ensure_size(cart, byte_addr);
               if (byte_addr < sizeof(ee->data))
               {
                    uint8_t mask = (uint8_t)(1u << (7 - (ee->write_address & 7)));
                    if (bit)
                         ee->data[byte_addr] |= mask;
                    else
                         ee->data[byte_addr] &= (uint8_t)~mask;
                    cart->dirty = true;
                    ee->settling_until = gba->timestamp + 115000;
               }
               ee->write_address++;
          }
          break;
     case 3: /* read command: address then one final bit before output */
          if (write_size > 1)
          {
               ee->read_address <<= 1;
               if (bit)
                    ee->read_address |= 0x40;
          }
          else
          {
               ee->read_bits_remaining = 68;
               ee->command = 4;
          }
          break;
     default:
          ee->command = 0;
          break;
     }
}

/* --- ROM reads --- */

uint8_t gba_cart_read8(struct gba *gba, uint32_t addr)
{
     struct gba_cart *cart = &gba->cart;

     if (addr >= GBA_SRAM_BASE)
     {
          uint32_t offset = addr - GBA_SRAM_BASE;
          switch (cart->backup_type)
          {
          case GBA_BACKUP_SRAM:
               return cart->sram[offset & (sizeof(cart->sram) - 1)];
          case GBA_BACKUP_FLASH_512:
          case GBA_BACKUP_FLASH_1M:
          {
               struct gba_cart_flash *fs = &cart->flash_state;
               offset &= 0xFFFF;
               if (fs->state == GBA_FLASH_ID_MODE)
               {
                    if (offset == 0)
                         return fs->manufacturer;
                    if (offset == 1)
                         return fs->device;
               }
               uint32_t bank_off = (cart->backup_type == GBA_BACKUP_FLASH_1M)
                                       ? (uint32_t)fs->bank * 0x10000
                                       : 0;
               return cart->flash[bank_off + offset];
          }
          default:
               return 0xFF;
          }
     }

     /* GPIO reads (only when gpio_control bit 0 is set = readable) */
     if (cart->gpio_control & 1)
     {
          switch (addr & ~1u)
          {
          case 0x080000C4:
               if (cart->has_rtc)
               {
                    uint16_t v = rtc_gpio_read(cart);
                    return (addr & 1) ? (uint8_t)(v >> 8) : (uint8_t)(v & 0xFF);
               }
               return (addr & 1) ? (uint8_t)(cart->gpio_data >> 8)
                                 : (uint8_t)(cart->gpio_data & 0xFF);
          case 0x080000C6:
               return (addr & 1) ? (uint8_t)(cart->gpio_direction >> 8)
                                 : (uint8_t)(cart->gpio_direction & 0xFF);
          case 0x080000C8:
               return (addr & 1) ? (uint8_t)(cart->gpio_control >> 8)
                                 : (uint8_t)(cart->gpio_control & 0xFF);
          default:
               break;
          }
     }

     /* ROM mirrors: 0x08, 0x0A, 0x0C → same data */
     uint32_t offset = addr & (GBA_ROM_MAX_SIZE - 1);
     if (!cart->rom || offset >= cart->rom_size)
     {
          /* Open bus on the 16-bit Game Pak bus exposes address bits /2. */
          return (uint8_t)((addr >> ((addr & 1) ? 9 : 1)) & 0xFF);
     }
     return cart->rom[offset];
}

uint16_t gba_cart_read16(struct gba *gba, uint32_t addr)
{
     return (uint16_t)(gba_cart_read8(gba, addr) |
                       ((uint16_t)gba_cart_read8(gba, addr + 1) << 8));
}

uint32_t gba_cart_read32(struct gba *gba, uint32_t addr)
{
     return (uint32_t)(gba_cart_read16(gba, addr) |
                       ((uint32_t)gba_cart_read16(gba, addr + 2) << 16));
}

/* --- Backup writes --- */

static void flash_write(struct gba_cart *cart, uint32_t offset, uint8_t val)
{
     struct gba_cart_flash *fs = &cart->flash_state;
     uint32_t bank_off = fs->bank * 0x10000;

     switch (fs->state)
     {
     case GBA_FLASH_READY:
          if (offset == 0x5555 && val == 0xAA)
          {
               fs->state = GBA_FLASH_CMD1;
               return;
          }
          break;
     case GBA_FLASH_CMD1:
          if (offset == 0x2AAA && val == 0x55)
          {
               fs->state = GBA_FLASH_CMD2;
               return;
          }
          fs->state = GBA_FLASH_READY;
          break;
     case GBA_FLASH_CMD2:
          if (offset == 0x5555)
          {
               switch (val)
               {
               case 0x90:
                    fs->state = GBA_FLASH_ID_MODE;
                    return;
               case 0xF0:
                    fs->state = GBA_FLASH_READY;
                    return;
               case 0x80:
                    fs->state = GBA_FLASH_ERASE_CMD1;
                    return;
               case 0xA0:
                    fs->state = GBA_FLASH_WRITE;
                    return;
               case 0xB0:
                    fs->state = GBA_FLASH_BANK_SWITCH;
                    return;
               }
          }
          fs->state = GBA_FLASH_READY;
          break;
     case GBA_FLASH_ERASE_CMD1:
          if (offset == 0x5555 && val == 0xAA)
          {
               fs->state = GBA_FLASH_ERASE_CMD2;
               return;
          }
          fs->state = GBA_FLASH_READY;
          break;
     case GBA_FLASH_ERASE_CMD2:
          if (offset == 0x2AAA && val == 0x55)
          {
               fs->state = GBA_FLASH_ERASE;
               return;
          }
          fs->state = GBA_FLASH_READY;
          break;
     case GBA_FLASH_ERASE:
          if (offset == 0x5555 && val == 0x10)
          {
               memset(cart->flash, 0xFF, sizeof(cart->flash));
               cart->dirty = true;
          }
          if (val == 0x30)
          {
               /* Sector erase: 4KB sector */
               uint32_t sector = bank_off + (offset & 0xF000);
               if (sector + 0x1000 <= sizeof(cart->flash))
                    memset(cart->flash + sector, 0xFF, 0x1000);
               cart->dirty = true;
          }
          fs->state = GBA_FLASH_READY;
          break;
     case GBA_FLASH_WRITE:
          if (bank_off + offset < sizeof(cart->flash))
          {
               cart->flash[bank_off + offset] &= val; /* flash can only clear bits */
               cart->dirty = true;
          }
          fs->state = GBA_FLASH_READY;
          break;
     case GBA_FLASH_BANK_SWITCH:
          if (offset == 0)
               fs->bank = val & 1;
          fs->state = GBA_FLASH_READY;
          break;
     case GBA_FLASH_ID_MODE:
          if (offset == 0x5555 && val == 0xAA)
          {
               fs->state = GBA_FLASH_CMD1;
               return;
          }
          if (val == 0xF0)
               fs->state = GBA_FLASH_READY;
          break;
     }
}

void gba_cart_write8(struct gba *gba, uint32_t addr, uint8_t val)
{
     struct gba_cart *cart = &gba->cart;

     if (addr >= GBA_SRAM_BASE)
     {
          uint32_t offset = addr - GBA_SRAM_BASE;
          switch (cart->backup_type)
          {
          case GBA_BACKUP_SRAM:
               cart->sram[offset & (sizeof(cart->sram) - 1)] = val;
               cart->dirty = true;
               break;
          case GBA_BACKUP_FLASH_512:
          case GBA_BACKUP_FLASH_1M:
               flash_write(cart, offset & 0xFFFF, val);
               break;
          default:
               break;
          }
          return;
     }

     /* GPIO writes (0x080000C4–0x080000C8) */
     switch (addr)
     {
     case 0x080000C4:
          cart->gpio_data = (cart->gpio_data & ~cart->gpio_direction) |
                            (val & cart->gpio_direction);
          if (cart->has_rtc)
               rtc_gpio_write(cart, val & 0x0F);
          break;
     case 0x080000C6:
          cart->gpio_direction = val;
          break;
     case 0x080000C8:
          cart->gpio_control = val;
          break;
     default:
          break;
     }
}

void gba_cart_write16(struct gba *gba, uint32_t addr, uint16_t val)
{
     gba_cart_write8(gba, addr, (uint8_t)(val & 0xFF));
     gba_cart_write8(gba, addr + 1, (uint8_t)(val >> 8));
}
