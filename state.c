#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gb.h"
#include "state.h"

#define GB_STATE_MAGIC "GBST0001"
#define GB_STATE_VERSION 5u

struct gb_state_header
{
     char magic[8];
     uint32_t version;
     uint32_t gbc;
     uint32_t ram_length;
     uint32_t vram_size;
     uint32_t iram_size;
     uint32_t rom_length;
     uint32_t rom_hash;
};

struct gb_state_cart
{
     unsigned rom_length;
     unsigned rom_banks;
     unsigned cur_rom_bank;
     unsigned ram_length;
     unsigned ram_banks;
     unsigned cur_ram_bank;
     bool ram_write_protected;
     enum gb_cart_model model;
     bool mbc1_bank_ram;
     bool dirty_ram;
     bool has_rtc;
     bool has_rumble;
     bool has_eeprom;
     struct gb_rtc rtc;
     struct gb_mbc7 mbc7;
};

struct gb_state_spu
{
     bool enable;
     uint8_t sample_period_frac;
     uint16_t frame_seq_counter;
     uint8_t frame_seq_step;
     uint8_t output_level;
     uint8_t sound_mux;
     int16_t sound_amp[4][2];
     bool frontend_mute[4];
     struct gb_spu_nr1 nr1;
     struct gb_spu_nr2 nr2;
     struct gb_spu_nr3 nr3;
     struct gb_spu_nr4 nr4;
};

#define WRITE_FIELD(f, ptr, size)                          \
     do                                                     \
     {                                                      \
          if (fwrite((ptr), 1, (size), (f)) != (size))      \
               return false;                               \
     } while (0)

#define READ_FIELD(f, ptr, size)                           \
     do                                                     \
     {                                                      \
          if (fread((ptr), 1, (size), (f)) != (size))       \
               return false;                               \
     } while (0)

static bool state_write(FILE *f, const void *ptr, size_t size)
{
     WRITE_FIELD(f, ptr, size);
     return true;
}

static bool state_read(FILE *f, void *ptr, size_t size)
{
     READ_FIELD(f, ptr, size);
     return true;
}

static bool state_cpu_pc_valid(uint16_t pc)
{
     return !(pc >= 0xff00 && pc < 0xff80);
}

static uint32_t state_rom_hash(struct gb *gb)
{
     uint32_t h = 2166136261u;

     for (unsigned i = 0; i < gb->cart.rom_length; i++)
     {
          h ^= gb->cart.rom[i];
          h *= 16777619u;
     }

     return h;
}

static void state_capture_cart(struct gb *gb, struct gb_state_cart *s)
{
     struct gb_cart *cart = &gb->cart;

     s->rom_length = cart->rom_length;
     s->rom_banks = cart->rom_banks;
     s->cur_rom_bank = cart->cur_rom_bank;
     s->ram_length = cart->ram_length;
     s->ram_banks = cart->ram_banks;
     s->cur_ram_bank = cart->cur_ram_bank;
     s->ram_write_protected = cart->ram_write_protected;
     s->model = cart->model;
     s->mbc1_bank_ram = cart->mbc1_bank_ram;
     s->dirty_ram = cart->dirty_ram;
     s->has_rtc = cart->has_rtc;
     s->has_rumble = cart->has_rumble;
     s->has_eeprom = cart->has_eeprom;
     s->rtc = cart->rtc;
     s->mbc7 = cart->mbc7;
}

static void state_restore_cart(struct gb *gb, const struct gb_state_cart *s)
{
     struct gb_cart *cart = &gb->cart;

     cart->rom_length = s->rom_length;
     cart->rom_banks = s->rom_banks;
     cart->cur_rom_bank = s->cur_rom_bank;
     cart->ram_length = s->ram_length;
     cart->ram_banks = s->ram_banks;
     cart->cur_ram_bank = s->cur_ram_bank;
     cart->ram_write_protected = s->ram_write_protected;
     cart->model = s->model;
     cart->mbc1_bank_ram = s->mbc1_bank_ram;
     cart->dirty_ram = s->dirty_ram;
     cart->has_rtc = s->has_rtc;
     cart->has_rumble = s->has_rumble;
     cart->has_eeprom = s->has_eeprom;
     cart->rtc = s->rtc;
     cart->mbc7 = s->mbc7;
}

static void state_capture_spu(struct gb *gb, struct gb_state_spu *s)
{
     struct gb_spu *spu = &gb->spu;

     s->enable = spu->enable;
     s->sample_period_frac = spu->sample_period_frac;
     s->frame_seq_counter = spu->frame_seq_counter;
     s->frame_seq_step = spu->frame_seq_step;
     s->output_level = spu->output_level;
     s->sound_mux = spu->sound_mux;
     memcpy(s->sound_amp, spu->sound_amp, sizeof(s->sound_amp));
     memcpy(s->frontend_mute, spu->frontend_mute, sizeof(s->frontend_mute));
     s->nr1 = spu->nr1;
     s->nr2 = spu->nr2;
     s->nr3 = spu->nr3;
     s->nr4 = spu->nr4;
}

static void state_restore_spu(struct gb *gb, const struct gb_state_spu *s)
{
     struct gb_spu *spu = &gb->spu;

     spu->enable = s->enable;
     spu->sample_period_frac = s->sample_period_frac;
     spu->frame_seq_counter = s->frame_seq_counter;
     spu->frame_seq_step = s->frame_seq_step;
     spu->output_level = s->output_level;
     spu->sound_mux = s->sound_mux;
     memcpy(spu->sound_amp, s->sound_amp, sizeof(spu->sound_amp));
     memcpy(spu->frontend_mute, s->frontend_mute, sizeof(spu->frontend_mute));
     spu->nr1 = s->nr1;
     spu->nr2 = s->nr2;
     spu->nr3 = s->nr3;
     spu->nr4 = s->nr4;

     /* Keep the live audio ring/semaphores owned by the frontend. */
     spu->sample_index = 0;
}

bool gb_state_save(struct gb *gb, const char *path)
{
     struct gb_state_header h;
     struct gb_state_cart cart;
     struct gb_state_spu spu;
     FILE *f;

     if (!gb->cart.rom)
          return false;

     memset(&h, 0, sizeof(h));
     memcpy(h.magic, GB_STATE_MAGIC, sizeof(h.magic));
     h.version = GB_STATE_VERSION;
     h.gbc = gb->gbc ? 1u : 0u;
     h.ram_length = gb->cart.ram_length;
     h.vram_size = sizeof(gb->vram);
     h.iram_size = sizeof(gb->iram);
     h.rom_length = gb->cart.rom_length;
     h.rom_hash = state_rom_hash(gb);

     state_capture_cart(gb, &cart);
     state_capture_spu(gb, &spu);

     f = fopen(path, "wb");
     if (!f)
     {
          fprintf(stderr, "save state: can't open '%s': %s\n", path, strerror(errno));
          return false;
     }

     if (!state_write(f, &h, sizeof(h)) ||
         !state_write(f, &gb->gbc, sizeof(gb->gbc)) ||
         !state_write(f, &gb->speed_switch_pending, sizeof(gb->speed_switch_pending)) ||
         !state_write(f, &gb->double_speed, sizeof(gb->double_speed)) ||
         !state_write(f, &gb->timestamp, sizeof(gb->timestamp)) ||
         !state_write(f, &gb->serial_data, sizeof(gb->serial_data)) ||
         !state_write(f, &gb->serial_control, sizeof(gb->serial_control)) ||
         !state_write(f, &gb->irq, sizeof(gb->irq)) ||
         !state_write(f, &gb->sync, sizeof(gb->sync)) ||
         !state_write(f, &gb->cpu, sizeof(gb->cpu)) ||
         !state_write(f, &cart, sizeof(cart)) ||
         !state_write(f, &gb->gpu, sizeof(gb->gpu)) ||
         !state_write(f, &gb->input, sizeof(gb->input)) ||
         !state_write(f, &gb->dma, sizeof(gb->dma)) ||
         !state_write(f, &gb->hdma, sizeof(gb->hdma)) ||
         !state_write(f, &gb->timer, sizeof(gb->timer)) ||
         !state_write(f, &spu, sizeof(spu)) ||
         !state_write(f, &gb->iram, sizeof(gb->iram)) ||
         !state_write(f, &gb->iram_high_bank, sizeof(gb->iram_high_bank)) ||
	         !state_write(f, &gb->zram, sizeof(gb->zram)) ||
	         !state_write(f, &gb->vram, sizeof(gb->vram)) ||
	         !state_write(f, &gb->vram_high_bank, sizeof(gb->vram_high_bank)) ||
	         !state_write(f, &gb->cgb_reg_ff72, sizeof(gb->cgb_reg_ff72)) ||
	         !state_write(f, &gb->cgb_reg_ff73, sizeof(gb->cgb_reg_ff73)) ||
	         !state_write(f, &gb->cgb_reg_ff75, sizeof(gb->cgb_reg_ff75)) ||
	         !state_write(f, &gb->bootrom_mapped, sizeof(gb->bootrom_mapped)) ||
         (gb->cart.ram_length > 0 &&
          !state_write(f, gb->cart.ram, gb->cart.ram_length)))
     {
          fprintf(stderr, "save state: failed writing '%s'\n", path);
          fclose(f);
          return false;
     }

     fclose(f);
     printf("Saved state: %s\n", path);
     return true;
}

bool gb_state_load(struct gb *gb, const char *path)
{
     struct gb_state_header h;
     struct gb_state_cart cart;
     struct gb_state_spu spu;
     bool gbc;
     bool speed_switch_pending;
     bool double_speed;
     int32_t timestamp;
     uint8_t serial_data;
     uint8_t serial_control;
     struct gb_irq irq;
     struct gb_sync sync;
     struct gb_cpu cpu;
     struct gb_gpu gpu;
     struct gb_input input;
     struct gb_dma dma;
     struct gb_hdma hdma;
     struct gb_timer timer;
     uint8_t iram[sizeof(gb->iram)];
     uint8_t iram_high_bank;
     uint8_t zram[sizeof(gb->zram)];
     uint8_t vram[sizeof(gb->vram)];
     bool vram_high_bank;
     uint8_t cgb_reg_ff72;
     uint8_t cgb_reg_ff73;
     uint8_t cgb_reg_ff75;
     bool bootrom_mapped;
     uint8_t *cart_ram = NULL;
     bool ok;
     FILE *f;

     if (!gb->cart.rom)
          return false;

     f = fopen(path, "rb");
     if (!f)
     {
          fprintf(stderr, "load state: can't open '%s': %s\n", path, strerror(errno));
          return false;
     }

     if (!state_read(f, &h, sizeof(h)) ||
         memcmp(h.magic, GB_STATE_MAGIC, sizeof(h.magic)) != 0 ||
         h.version != GB_STATE_VERSION ||
         h.gbc != (gb->gbc ? 1u : 0u) ||
         h.ram_length != gb->cart.ram_length ||
         h.vram_size != sizeof(gb->vram) ||
         h.iram_size != sizeof(gb->iram) ||
         h.rom_length != gb->cart.rom_length ||
         h.rom_hash != state_rom_hash(gb))
     {
          fprintf(stderr, "load state: incompatible state '%s'\n", path);
          fclose(f);
          return false;
     }

     if (gb->cart.ram_length > 0)
     {
          cart_ram = malloc(gb->cart.ram_length);
          if (!cart_ram)
          {
               fprintf(stderr, "load state: can't allocate cart RAM snapshot\n");
               fclose(f);
               return false;
          }
     }

     ok = state_read(f, &gbc, sizeof(gbc)) &&
          state_read(f, &speed_switch_pending, sizeof(speed_switch_pending)) &&
          state_read(f, &double_speed, sizeof(double_speed)) &&
          state_read(f, &timestamp, sizeof(timestamp)) &&
          state_read(f, &serial_data, sizeof(serial_data)) &&
          state_read(f, &serial_control, sizeof(serial_control)) &&
          state_read(f, &irq, sizeof(irq)) &&
          state_read(f, &sync, sizeof(sync)) &&
          state_read(f, &cpu, sizeof(cpu)) &&
          state_read(f, &cart, sizeof(cart)) &&
          state_read(f, &gpu, sizeof(gpu)) &&
          state_read(f, &input, sizeof(input)) &&
          state_read(f, &dma, sizeof(dma)) &&
          state_read(f, &hdma, sizeof(hdma)) &&
          state_read(f, &timer, sizeof(timer)) &&
          state_read(f, &spu, sizeof(spu)) &&
          state_read(f, iram, sizeof(iram)) &&
	          state_read(f, &iram_high_bank, sizeof(iram_high_bank)) &&
	          state_read(f, zram, sizeof(zram)) &&
	          state_read(f, vram, sizeof(vram)) &&
	          state_read(f, &vram_high_bank, sizeof(vram_high_bank)) &&
	          state_read(f, &cgb_reg_ff72, sizeof(cgb_reg_ff72)) &&
	          state_read(f, &cgb_reg_ff73, sizeof(cgb_reg_ff73)) &&
	          state_read(f, &cgb_reg_ff75, sizeof(cgb_reg_ff75)) &&
	          state_read(f, &bootrom_mapped, sizeof(bootrom_mapped)) &&
          (gb->cart.ram_length == 0 ||
           state_read(f, cart_ram, gb->cart.ram_length));

     if (!ok)
     {
          fprintf(stderr, "load state: failed reading '%s'\n", path);
          free(cart_ram);
          fclose(f);
          return false;
     }

     fclose(f);

     if (!state_cpu_pc_valid(cpu.pc))
     {
          fprintf(stderr, "load state: refusing invalid PC 0x%04x in '%s'\n",
                  cpu.pc, path);
          free(cart_ram);
          return false;
     }

     gb->gbc = gbc;
     gb->speed_switch_pending = speed_switch_pending;
     gb->double_speed = double_speed;
     gb->timestamp = timestamp;
     gb->serial_data = serial_data;
     gb->serial_control = serial_control;
     gb->irq = irq;
     gb->sync = sync;
     gb->cpu = cpu;
     gb->gpu = gpu;
     gb->input = input;
     gb->dma = dma;
     gb->hdma = hdma;
     gb->timer = timer;
     memcpy(gb->iram, iram, sizeof(iram));
     gb->iram_high_bank = iram_high_bank;
     memcpy(gb->zram, zram, sizeof(zram));
     memcpy(gb->vram, vram, sizeof(vram));
     gb->vram_high_bank = vram_high_bank;
     gb->cgb_reg_ff72 = cgb_reg_ff72;
     gb->cgb_reg_ff73 = cgb_reg_ff73;
     gb->cgb_reg_ff75 = cgb_reg_ff75;
     gb->bootrom_mapped = bootrom_mapped;
     state_restore_cart(gb, &cart);
     state_restore_spu(gb, &spu);
     if (gb->cart.ram_length > 0)
     {
          memcpy(gb->cart.ram, cart_ram, gb->cart.ram_length);
          free(cart_ram);
     }

     printf("Loaded state: %s\n", path);
     return true;
}
