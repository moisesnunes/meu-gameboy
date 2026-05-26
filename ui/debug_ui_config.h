#ifndef DEBUG_UI_CONFIG_H
#define DEBUG_UI_CONFIG_H

#include <string>
#include <vector>

struct debug_ui_config
{
     bool show_screen;
     bool show_cpu;
     bool show_disasm;
     bool show_memory;
     bool show_gpu;
     bool show_oam;
     bool show_tiles;
     bool show_tilemap;
     bool show_profiler;
     bool show_call_stack;
     bool show_status_bar;
     bool show_hw_viz;
     bool show_cpu_viz;
     bool show_transistor_viz;
     bool show_hw_schematic;

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

     bool start_paused;
     float fast_forward_speed;
     int debug_font_size;
     int save_slot;
     int input_keys[8];

     std::vector<std::string> recent_roms;
};

void debug_ui_config_defaults(debug_ui_config *cfg);
bool debug_ui_config_load(debug_ui_config *cfg, const char *path);
bool debug_ui_config_save(const debug_ui_config *cfg, const char *path);

#endif /* DEBUG_UI_CONFIG_H */
