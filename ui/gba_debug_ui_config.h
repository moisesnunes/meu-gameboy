#ifndef GBA_DEBUG_UI_CONFIG_H
#define GBA_DEBUG_UI_CONFIG_H

#include <string>
#include <vector>

struct gba_debug_ui_config
{
     bool show_screen;
     bool show_cpu;
     bool show_disasm;
     bool show_memory;
     bool show_gpu;
     bool show_oam;
     bool show_apu;
     bool show_profiler;
     bool show_status_bar;

     bool vsync;
     bool bilinear;
     bool show_fps;
     int video_scale;
     bool scanlines;
     float scanlines_intensity;
     bool mix_frames;
     float mix_frames_intensity;
     float background_color[3];

     bool audio_muted;
     float audio_volume;

     float fast_forward_speed;
     int debug_font_size;
     bool start_paused;
     int save_slot;

     std::vector<std::string> recent_roms;
};

void gba_debug_ui_config_defaults(gba_debug_ui_config *cfg);
bool gba_debug_ui_config_load(gba_debug_ui_config *cfg, const char *path);
bool gba_debug_ui_config_save(const gba_debug_ui_config *cfg, const char *path);

#endif /* GBA_DEBUG_UI_CONFIG_H */
