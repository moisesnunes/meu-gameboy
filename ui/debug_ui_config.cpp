#include "debug_ui_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>

#include <SDL3/SDL.h>

static std::string trim(const std::string &s)
{
     size_t begin = 0;
     while (begin < s.size() && std::isspace((unsigned char)s[begin]))
          ++begin;

     size_t end = s.size();
     while (end > begin && std::isspace((unsigned char)s[end - 1]))
          --end;

     return s.substr(begin, end - begin);
}

static bool parse_bool(const std::string &value, bool fallback)
{
     std::string v;
     for (char c : value)
          v.push_back((char)std::tolower((unsigned char)c));

     if (v == "1" || v == "true" || v == "yes" || v == "on")
          return true;
     if (v == "0" || v == "false" || v == "no" || v == "off")
          return false;
     return fallback;
}

static int parse_int(const std::string &value, int fallback)
{
     char *end = nullptr;
     long parsed = std::strtol(value.c_str(), &end, 10);
     return end && *end == '\0' ? (int)parsed : fallback;
}

static float parse_float(const std::string &value, float fallback)
{
     char *end = nullptr;
     float parsed = std::strtof(value.c_str(), &end);
     return end && *end == '\0' ? parsed : fallback;
}

static int clamp_int(int value, int min_value, int max_value)
{
     return std::max(min_value, std::min(max_value, value));
}

static float clamp_float(float value, float min_value, float max_value)
{
     return std::max(min_value, std::min(max_value, value));
}

static void push_recent(std::vector<std::string> *recent, const std::string &path)
{
     if (path.empty())
          return;

     recent->erase(std::remove(recent->begin(), recent->end(), path), recent->end());
     if (recent->size() < 10)
          recent->push_back(path);
}

void debug_ui_config_defaults(debug_ui_config *cfg)
{
     cfg->show_screen = true;
     cfg->show_cpu = true;
     cfg->show_disasm = true;
     cfg->show_memory = true;
     cfg->show_gpu = false;
     cfg->show_oam = false;
     cfg->show_tiles = false;
     cfg->show_tilemap = false;
     cfg->show_profiler = false;
     cfg->show_call_stack = false;
     cfg->show_status_bar = true;
     cfg->show_hw_viz          = false;
     cfg->show_cpu_viz         = false;
     cfg->show_transistor_viz  = false;
     cfg->show_hw_schematic    = false;

     cfg->vsync = true;
     cfg->bilinear = false;
     cfg->show_fps = false;
     cfg->video_scale = 0;
     cfg->scanlines = false;
     cfg->scanlines_intensity = 0.5f;
     cfg->mix_frames = false;
     cfg->mix_frames_intensity = 0.3f;
     cfg->background_color[0] = 0.10f;
     cfg->background_color[1] = 0.10f;
     cfg->background_color[2] = 0.10f;

     cfg->audio_muted = false;
     cfg->audio_volume = 1.0f;

     cfg->start_paused = false;
     cfg->fast_forward_speed = 2.0f;
     cfg->debug_font_size = 1;
     cfg->save_slot = 0;
     cfg->input_keys[0] = SDLK_RIGHT;
     cfg->input_keys[1] = SDLK_LEFT;
     cfg->input_keys[2] = SDLK_UP;
     cfg->input_keys[3] = SDLK_DOWN;
     cfg->input_keys[4] = SDLK_LCTRL;
     cfg->input_keys[5] = SDLK_LSHIFT;
     cfg->input_keys[6] = SDLK_RSHIFT;
     cfg->input_keys[7] = SDLK_RETURN;

     cfg->recent_roms.clear();
}

bool debug_ui_config_load(debug_ui_config *cfg, const char *path)
{
     debug_ui_config_defaults(cfg);

     std::ifstream in(path);
     if (!in)
          return false;

     cfg->recent_roms.clear();

     std::string line;
     while (std::getline(in, line))
     {
          std::string stripped = trim(line);
          if (stripped.empty() || stripped[0] == '#')
               continue;

          size_t eq = stripped.find('=');
          if (eq == std::string::npos)
               continue;

          std::string key = trim(stripped.substr(0, eq));
          std::string value = trim(stripped.substr(eq + 1));

          if (key == "show_screen")
               cfg->show_screen = parse_bool(value, cfg->show_screen);
          else if (key == "show_cpu")
               cfg->show_cpu = parse_bool(value, cfg->show_cpu);
          else if (key == "show_disasm")
               cfg->show_disasm = parse_bool(value, cfg->show_disasm);
          else if (key == "show_memory")
               cfg->show_memory = parse_bool(value, cfg->show_memory);
          else if (key == "show_gpu")
               cfg->show_gpu = parse_bool(value, cfg->show_gpu);
          else if (key == "show_oam")
               cfg->show_oam = parse_bool(value, cfg->show_oam);
          else if (key == "show_tiles")
               cfg->show_tiles = parse_bool(value, cfg->show_tiles);
          else if (key == "show_tilemap")
               cfg->show_tilemap = parse_bool(value, cfg->show_tilemap);
          else if (key == "show_profiler")
               cfg->show_profiler = parse_bool(value, cfg->show_profiler);
          else if (key == "show_call_stack")
               cfg->show_call_stack = parse_bool(value, cfg->show_call_stack);
          else if (key == "show_status_bar")
               cfg->show_status_bar = parse_bool(value, cfg->show_status_bar);
          else if (key == "show_hw_viz")
               cfg->show_hw_viz = parse_bool(value, cfg->show_hw_viz);
          else if (key == "show_cpu_viz")
               cfg->show_cpu_viz = parse_bool(value, cfg->show_cpu_viz);
          else if (key == "show_transistor_viz")
               cfg->show_transistor_viz = parse_bool(value, cfg->show_transistor_viz);
          else if (key == "show_hw_schematic")
               cfg->show_hw_schematic = parse_bool(value, cfg->show_hw_schematic);
          else if (key == "vsync")
               cfg->vsync = parse_bool(value, cfg->vsync);
          else if (key == "bilinear")
               cfg->bilinear = parse_bool(value, cfg->bilinear);
          else if (key == "show_fps")
               cfg->show_fps = parse_bool(value, cfg->show_fps);
          else if (key == "video_scale")
               cfg->video_scale = parse_int(value, cfg->video_scale);
          else if (key == "scanlines")
               cfg->scanlines = parse_bool(value, cfg->scanlines);
          else if (key == "scanlines_intensity")
               cfg->scanlines_intensity = parse_float(value, cfg->scanlines_intensity);
          else if (key == "mix_frames")
               cfg->mix_frames = parse_bool(value, cfg->mix_frames);
          else if (key == "mix_frames_intensity")
               cfg->mix_frames_intensity = parse_float(value, cfg->mix_frames_intensity);
          else if (key == "background_r")
               cfg->background_color[0] = parse_float(value, cfg->background_color[0]);
          else if (key == "background_g")
               cfg->background_color[1] = parse_float(value, cfg->background_color[1]);
          else if (key == "background_b")
               cfg->background_color[2] = parse_float(value, cfg->background_color[2]);
          else if (key == "audio_muted")
               cfg->audio_muted = parse_bool(value, cfg->audio_muted);
          else if (key == "audio_volume")
               cfg->audio_volume = parse_float(value, cfg->audio_volume);
          else if (key == "start_paused")
               cfg->start_paused = parse_bool(value, cfg->start_paused);
          else if (key == "fast_forward_speed")
               cfg->fast_forward_speed = parse_float(value, cfg->fast_forward_speed);
          else if (key == "debug_font_size")
               cfg->debug_font_size = parse_int(value, cfg->debug_font_size);
          else if (key == "save_slot")
               cfg->save_slot = parse_int(value, cfg->save_slot);
          else if (key == "input_right")
               cfg->input_keys[0] = parse_int(value, cfg->input_keys[0]);
          else if (key == "input_left")
               cfg->input_keys[1] = parse_int(value, cfg->input_keys[1]);
          else if (key == "input_up")
               cfg->input_keys[2] = parse_int(value, cfg->input_keys[2]);
          else if (key == "input_down")
               cfg->input_keys[3] = parse_int(value, cfg->input_keys[3]);
          else if (key == "input_a")
               cfg->input_keys[4] = parse_int(value, cfg->input_keys[4]);
          else if (key == "input_b")
               cfg->input_keys[5] = parse_int(value, cfg->input_keys[5]);
          else if (key == "input_select")
               cfg->input_keys[6] = parse_int(value, cfg->input_keys[6]);
          else if (key == "input_start")
               cfg->input_keys[7] = parse_int(value, cfg->input_keys[7]);
          else if (key == "recent_rom")
               push_recent(&cfg->recent_roms, value);
     }

     cfg->video_scale = clamp_int(cfg->video_scale, -3, 8);
     cfg->scanlines_intensity = clamp_float(cfg->scanlines_intensity, 0.0f, 1.0f);
     cfg->mix_frames_intensity = clamp_float(cfg->mix_frames_intensity, 0.0f, 1.0f);
     cfg->background_color[0] = clamp_float(cfg->background_color[0], 0.0f, 1.0f);
     cfg->background_color[1] = clamp_float(cfg->background_color[1], 0.0f, 1.0f);
     cfg->background_color[2] = clamp_float(cfg->background_color[2], 0.0f, 1.0f);
     cfg->audio_volume = clamp_float(cfg->audio_volume, 0.0f, 2.0f);
     cfg->fast_forward_speed = clamp_float(cfg->fast_forward_speed, 1.5f, 8.0f);
     cfg->debug_font_size = clamp_int(cfg->debug_font_size, 0, 3);
     cfg->save_slot = clamp_int(cfg->save_slot, 0, 4);

     return true;
}

bool debug_ui_config_save(const debug_ui_config *cfg, const char *path)
{
     std::ofstream out(path);
     if (!out)
          return false;

     out << "# Gaembuoy desktop UI configuration\n";
     out << "show_screen=" << cfg->show_screen << "\n";
     out << "show_cpu=" << cfg->show_cpu << "\n";
     out << "show_disasm=" << cfg->show_disasm << "\n";
     out << "show_memory=" << cfg->show_memory << "\n";
     out << "show_gpu=" << cfg->show_gpu << "\n";
     out << "show_oam=" << cfg->show_oam << "\n";
     out << "show_tiles=" << cfg->show_tiles << "\n";
     out << "show_tilemap=" << cfg->show_tilemap << "\n";
     out << "show_profiler=" << cfg->show_profiler << "\n";
     out << "show_call_stack=" << cfg->show_call_stack << "\n";
     out << "show_status_bar=" << cfg->show_status_bar << "\n";
     out << "show_hw_viz=" << cfg->show_hw_viz << "\n";
     out << "show_cpu_viz=" << cfg->show_cpu_viz << "\n";
     out << "show_transistor_viz=" << cfg->show_transistor_viz << "\n";
     out << "show_hw_schematic=" << cfg->show_hw_schematic << "\n";
     out << "vsync=" << cfg->vsync << "\n";
     out << "bilinear=" << cfg->bilinear << "\n";
     out << "show_fps=" << cfg->show_fps << "\n";
     out << "video_scale=" << cfg->video_scale << "\n";
     out << "scanlines=" << cfg->scanlines << "\n";
     out << "scanlines_intensity=" << cfg->scanlines_intensity << "\n";
     out << "mix_frames=" << cfg->mix_frames << "\n";
     out << "mix_frames_intensity=" << cfg->mix_frames_intensity << "\n";
     out << "background_r=" << cfg->background_color[0] << "\n";
     out << "background_g=" << cfg->background_color[1] << "\n";
     out << "background_b=" << cfg->background_color[2] << "\n";
     out << "audio_muted=" << cfg->audio_muted << "\n";
     out << "audio_volume=" << cfg->audio_volume << "\n";
     out << "start_paused=" << cfg->start_paused << "\n";
     out << "fast_forward_speed=" << cfg->fast_forward_speed << "\n";
     out << "debug_font_size=" << cfg->debug_font_size << "\n";
     out << "save_slot=" << cfg->save_slot << "\n";
     out << "input_right=" << cfg->input_keys[0] << "\n";
     out << "input_left=" << cfg->input_keys[1] << "\n";
     out << "input_up=" << cfg->input_keys[2] << "\n";
     out << "input_down=" << cfg->input_keys[3] << "\n";
     out << "input_a=" << cfg->input_keys[4] << "\n";
     out << "input_b=" << cfg->input_keys[5] << "\n";
     out << "input_select=" << cfg->input_keys[6] << "\n";
     out << "input_start=" << cfg->input_keys[7] << "\n";

     for (const std::string &rom : cfg->recent_roms)
          out << "recent_rom=" << rom << "\n";

     return (bool)out;
}
