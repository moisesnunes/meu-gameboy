#include "gba_debug_ui_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void gba_debug_ui_config_defaults(gba_debug_ui_config *cfg)
{
     cfg->show_screen = true;
     cfg->show_cpu = true;
     cfg->show_disasm = true;
     cfg->show_memory = true;
     cfg->show_gpu = false;
     cfg->show_oam = false;
     cfg->show_apu = false;
     cfg->show_profiler = false;
     cfg->show_status_bar = true;

     cfg->vsync = true;
     cfg->bilinear = false;
     cfg->show_fps = false;
     cfg->video_scale = 0; /* fit */
     cfg->scanlines = false;
     cfg->scanlines_intensity = 0.5f;
     cfg->background_color[0] = 0.10f;
     cfg->background_color[1] = 0.10f;
     cfg->background_color[2] = 0.10f;

     cfg->fast_forward_speed = 2.0f;
     cfg->debug_font_size = 1;
     cfg->start_paused = false;

     cfg->recent_roms.clear();
}

bool gba_debug_ui_config_load(gba_debug_ui_config *cfg, const char *path)
{
     gba_debug_ui_config_defaults(cfg);

     FILE *f = fopen(path, "r");
     if (!f)
          return false;

     char line[512];
     while (fgets(line, sizeof(line), f))
     {
          char key[64];
          char sval[256];
          int ival;
          float fval;

          if (sscanf(line, "show_screen=%d", &ival) == 1)
               cfg->show_screen = ival;
          else if (sscanf(line, "show_cpu=%d", &ival) == 1)
               cfg->show_cpu = ival;
          else if (sscanf(line, "show_disasm=%d", &ival) == 1)
               cfg->show_disasm = ival;
          else if (sscanf(line, "show_memory=%d", &ival) == 1)
               cfg->show_memory = ival;
          else if (sscanf(line, "show_gpu=%d", &ival) == 1)
               cfg->show_gpu = ival;
          else if (sscanf(line, "show_oam=%d", &ival) == 1)
               cfg->show_oam = ival;
          else if (sscanf(line, "show_apu=%d", &ival) == 1)
               cfg->show_apu = ival;
          else if (sscanf(line, "show_profiler=%d", &ival) == 1)
               cfg->show_profiler = ival;
          else if (sscanf(line, "show_status_bar=%d", &ival) == 1)
               cfg->show_status_bar = ival;
          else if (sscanf(line, "vsync=%d", &ival) == 1)
               cfg->vsync = ival;
          else if (sscanf(line, "bilinear=%d", &ival) == 1)
               cfg->bilinear = ival;
          else if (sscanf(line, "show_fps=%d", &ival) == 1)
               cfg->show_fps = ival;
          else if (sscanf(line, "video_scale=%d", &ival) == 1)
               cfg->video_scale = ival;
          else if (sscanf(line, "scanlines=%d", &ival) == 1)
               cfg->scanlines = ival;
          else if (sscanf(line, "scanlines_intensity=%f", &fval) == 1)
               cfg->scanlines_intensity = fval;
          else if (sscanf(line, "bg_r=%f", &fval) == 1)
               cfg->background_color[0] = fval;
          else if (sscanf(line, "bg_g=%f", &fval) == 1)
               cfg->background_color[1] = fval;
          else if (sscanf(line, "bg_b=%f", &fval) == 1)
               cfg->background_color[2] = fval;
          else if (sscanf(line, "fast_forward_speed=%f", &fval) == 1)
               cfg->fast_forward_speed = fval;
          else if (sscanf(line, "debug_font_size=%d", &ival) == 1)
               cfg->debug_font_size = ival;
          else if (sscanf(line, "start_paused=%d", &ival) == 1)
               cfg->start_paused = ival;
          else if (sscanf(line, "recent=%255[^\n]", sval) == 1)
               cfg->recent_roms.push_back(sval);
          (void)key;
     }

     fclose(f);
     return true;
}

bool gba_debug_ui_config_save(const gba_debug_ui_config *cfg, const char *path)
{
     FILE *f = fopen(path, "w");
     if (!f)
          return false;

     fprintf(f, "show_screen=%d\n", (int)cfg->show_screen);
     fprintf(f, "show_cpu=%d\n", (int)cfg->show_cpu);
     fprintf(f, "show_disasm=%d\n", (int)cfg->show_disasm);
     fprintf(f, "show_memory=%d\n", (int)cfg->show_memory);
     fprintf(f, "show_gpu=%d\n", (int)cfg->show_gpu);
     fprintf(f, "show_oam=%d\n", (int)cfg->show_oam);
     fprintf(f, "show_apu=%d\n", (int)cfg->show_apu);
     fprintf(f, "show_profiler=%d\n", (int)cfg->show_profiler);
     fprintf(f, "show_status_bar=%d\n", (int)cfg->show_status_bar);
     fprintf(f, "vsync=%d\n", (int)cfg->vsync);
     fprintf(f, "bilinear=%d\n", (int)cfg->bilinear);
     fprintf(f, "show_fps=%d\n", (int)cfg->show_fps);
     fprintf(f, "video_scale=%d\n", cfg->video_scale);
     fprintf(f, "scanlines=%d\n", (int)cfg->scanlines);
     fprintf(f, "scanlines_intensity=%f\n", cfg->scanlines_intensity);
     fprintf(f, "bg_r=%f\n", cfg->background_color[0]);
     fprintf(f, "bg_g=%f\n", cfg->background_color[1]);
     fprintf(f, "bg_b=%f\n", cfg->background_color[2]);
     fprintf(f, "fast_forward_speed=%f\n", cfg->fast_forward_speed);
     fprintf(f, "debug_font_size=%d\n", cfg->debug_font_size);
     fprintf(f, "start_paused=%d\n", (int)cfg->start_paused);
     for (auto &r : cfg->recent_roms)
          fprintf(f, "recent=%s\n", r.c_str());

     fclose(f);
     return true;
}
