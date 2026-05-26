#include "debug_ui_actions.h"

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

extern "C"
{
#include "debug.h"
#include "gb.h"
#include "sdl.h"
#include "state.h"

     void gb_cpu_reset(struct gb *);
     void gb_gpu_reset(struct gb *);
     void gb_sync_reset(struct gb *);
     void gb_irq_reset(struct gb *);
}

static void write_message(char *message, size_t message_len, const char *fmt, ...)
{
     if (!message || message_len == 0)
          return;

     va_list ap;
     va_start(ap, fmt);
     vsnprintf(message, message_len, fmt, ap);
     va_end(ap);
}

const char *debug_ui_action_rom_title(struct gb *gb)
{
     static char title[17];

     if (!gb->cart.rom || gb->cart.rom_length < 0x150)
          return "Sem ROM";

     memcpy(title, &gb->cart.rom[0x0134], 16);
     title[16] = '\0';

     for (int i = 15; i >= 0; --i)
     {
          if (title[i] == '\0' || title[i] == ' ')
               title[i] = '\0';
          else
               break;
     }

     return title[0] ? title : "ROM carregada";
}

static std::string safe_rom_title(struct gb *gb)
{
     const char *title = debug_ui_action_rom_title(gb);
     std::string out;

     for (const char *p = title; *p && out.size() < 48; ++p)
     {
          char c = *p;
          if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')
               out += c;
          else if (c == ' ')
               out += '_';
     }

     return out.empty() ? "gaembuoy" : out;
}

bool debug_ui_action_save_screenshot(struct gb *gb, char *message, size_t message_len)
{
     const uint32_t *pixels = gb_sdl_get_pixels(gb);
     if (!pixels)
     {
          write_message(message, message_len, "Frame indispon\xc3\xadvel para screenshot");
          return false;
     }

     if (mkdir("screenshots", 0755) != 0 && errno != EEXIST)
     {
          write_message(message, message_len, "Falha ao criar screenshots/: %s", strerror(errno));
          fprintf(stderr, "screenshot: failed to create screenshots directory: %s\n", strerror(errno));
          return false;
     }

     time_t now = time(nullptr);
     struct tm tm_now;
     localtime_r(&now, &tm_now);

     char timestamp[32];
     strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &tm_now);

     std::string path = "screenshots/";
     path += safe_rom_title(gb);
     path += "-";
     path += timestamp;
     path += ".ppm";

     FILE *f = fopen(path.c_str(), "wb");
     if (!f)
     {
          write_message(message, message_len, "Falha ao abrir screenshot: %s", strerror(errno));
          fprintf(stderr, "screenshot: failed to open '%s': %s\n", path.c_str(), strerror(errno));
          return false;
     }

     fprintf(f, "P6\n%d %d\n255\n", GB_LCD_WIDTH, GB_LCD_HEIGHT);
     for (int y = 0; y < GB_LCD_HEIGHT; ++y)
     {
          for (int x = 0; x < GB_LCD_WIDTH; ++x)
          {
               uint32_t p = pixels[y * GB_LCD_WIDTH + x];
               uint8_t rgb[3] = {
                   (uint8_t)((p >> 16) & 0xff),
                   (uint8_t)((p >> 8) & 0xff),
                   (uint8_t)(p & 0xff),
               };
               fwrite(rgb, 1, sizeof(rgb), f);
          }
     }

     fclose(f);
     write_message(message, message_len, "Screenshot salvo: %s", path.c_str());
     printf("Saved screenshot: %s\n", path.c_str());
     return true;
}

static std::string state_slot_path(struct gb *gb, int slot)
{
     char suffix[32];
     std::string path = "states/";

     path += safe_rom_title(gb);
     snprintf(suffix, sizeof(suffix), "-slot%d.gbst", slot + 1);
     path += suffix;
     return path;
}

bool debug_ui_action_state_slot_exists(struct gb *gb, int slot)
{
     struct stat st;
     std::string path = state_slot_path(gb, slot);
     return stat(path.c_str(), &st) == 0;
}

bool debug_ui_action_save_state_slot(struct gb *gb, int slot, char *message, size_t message_len)
{
     if (mkdir("states", 0755) != 0 && errno != EEXIST)
     {
          write_message(message, message_len, "Falha ao criar diret\xc3\xb3rio states/");
          fprintf(stderr, "save state: failed to create states directory: %s\n", strerror(errno));
          return false;
     }

     std::string path = state_slot_path(gb, slot);
     if (gb_state_save(gb, path.c_str()))
     {
          write_message(message, message_len, "State salvo no slot %d", slot + 1);
          return true;
     }

     write_message(message, message_len, "Falha ao salvar state no slot %d", slot + 1);
     return false;
}

bool debug_ui_action_load_state_slot(struct gb *gb, int slot, char *message, size_t message_len)
{
     std::string path = state_slot_path(gb, slot);
     if (gb_state_load(gb, path.c_str()))
     {
          write_message(message, message_len, "State carregado do slot %d", slot + 1);
          return true;
     }

     write_message(message, message_len, "Falha ao carregar state do slot %d", slot + 1);
     return false;
}

void debug_ui_action_reset(struct gb *gb, bool start_paused)
{
     gb_sync_reset(gb);
     gb_irq_reset(gb);
     gb_cpu_reset(gb);
     gb_gpu_reset(gb);
     gb->debug.instruction_count = 0;
     gb->debug.state = start_paused ? GB_DEBUG_PAUSED : GB_DEBUG_RUNNING;
}
