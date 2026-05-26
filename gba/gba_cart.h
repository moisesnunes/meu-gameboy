#ifndef _GBA_CART_H_
#define _GBA_CART_H_

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

struct gba;

enum gba_backup_type {
    GBA_BACKUP_NONE,
    GBA_BACKUP_SRAM,        /* 32KB static RAM at 0x0E000000 */
    GBA_BACKUP_EEPROM_512,  /* 512 bytes (4Kbit), serial DMA access */
    GBA_BACKUP_EEPROM_8K,   /* 8192 bytes (64Kbit), serial DMA access */
    GBA_BACKUP_FLASH_512,   /* 512KB flash (Atmel/Panasonic/Sanyo) */
    GBA_BACKUP_FLASH_1M,    /* 1MB flash, 2 banks of 512KB */
};

/* Flash state machine states */
enum gba_flash_state {
    GBA_FLASH_READY,
    GBA_FLASH_CMD1,
    GBA_FLASH_CMD2,
    GBA_FLASH_ID_MODE,
    GBA_FLASH_ERASE_CMD1,
    GBA_FLASH_ERASE_CMD2,
    GBA_FLASH_ERASE,
    GBA_FLASH_WRITE,
    GBA_FLASH_BANK_SWITCH,
};

struct gba_cart_flash {
    enum gba_flash_state state;
    uint8_t bank;          /* 0 or 1 (Flash 1M only) */
    bool    id_mode;
    uint8_t manufacturer;  /* 0x1F Atmel, 0xBF SST, 0x32 Panasonic */
    uint8_t device;
};

/* EEPROM serial access state */
struct gba_cart_eeprom {
    uint8_t  data[8192];   /* up to 8KB */
    uint16_t shift_reg;
    int      bit_count;
    uint16_t address;
    bool     writing;
};

enum gba_rtc_state {
    RTC_IDLE     = 0,
    RTC_COMMAND  = 1,
    RTC_DATA     = 2,
    RTC_READDATA = 3,
};

struct gba_rtc {
    enum gba_rtc_state state;
    uint8_t  byte0;    /* last GPIO value seen by RTC logic */
    uint8_t  command;  /* command byte being shifted in */
    int      bits;     /* bits shifted so far in current phase */
    int      data_len; /* bytes expected for current command */
    uint8_t  data[12]; /* data buffer clocked out to game */
    struct tm time;    /* time snapshot for current read command */
};

struct gba_cart {
    uint8_t *rom;
    uint32_t rom_size;

    enum gba_backup_type backup_type;

    /* SRAM */
    uint8_t sram[0x8000];  /* 32KB */

    /* Flash */
    uint8_t flash[0x20000]; /* 128KB per bank; 2 banks for 1M */
    struct gba_cart_flash flash_state;

    /* EEPROM */
    struct gba_cart_eeprom eeprom;

    /* Dirty flag for save persistence */
    bool dirty;
    FILE *save_file;

    /* GPIO (RTC at 0x080000C4–0x080000C8) */
    uint16_t gpio_data;
    uint16_t gpio_direction;
    uint16_t gpio_control;
    bool     has_rtc;

    /* RTC state machine */
    struct gba_rtc rtc;
};

bool gba_cart_load(struct gba *gba, const char *path);
void gba_cart_unload(struct gba *gba);

uint8_t  gba_cart_read8(struct gba *gba, uint32_t addr);
uint16_t gba_cart_read16(struct gba *gba, uint32_t addr);
uint32_t gba_cart_read32(struct gba *gba, uint32_t addr);

void gba_cart_write8(struct gba *gba, uint32_t addr, uint8_t val);
void gba_cart_write16(struct gba *gba, uint32_t addr, uint16_t val);

void gba_cart_save(struct gba *gba);
void gba_cart_sync(struct gba *gba);

#endif /* _GBA_CART_H_ */
