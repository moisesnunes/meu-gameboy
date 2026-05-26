/*
 * debug_ui.cpp — Janela de Debugger com Dear ImGui + SDL2 + OpenGL3
 *
 * Painéis:
 *   - Controle de Execução (Pause / Continue / Step)
 *   - Registradores da CPU e Flags
 *   - Disassembly (rolando para manter o PC visível, click = toggle breakpoint)
 *   - Estado da GPU/PPU (LCDC, STAT, LY, SCX/SCY, paletas)
 *   - Memory Viewer (hex dump em qualquer endereço)
 *   - Breakpoints (adicionar, remover, habilitar/desabilitar)
 *   - OAM (tabela de 40 sprites)
 */

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#include "misc/fonts/RobotoMedium.h"
#include "debug_ui_actions.h"
#include "debug_ui_config.h"
#include "debug_ui_menus.h"
#include "debug_ui_panels.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <stdlib.h>
#include <string>
#include <vector>

extern "C"
{
#include "gb.h"
#include "debug.h"
#include "debug_ui.h"
#include "disasm.h"
#include "sdl.h"
#include "cart.h"
#include "state.h"
}

/* ────────────────────────────────────────────────────────── */
/* Estado interno da janela de debug                          */
/* ────────────────────────────────────────────────────────── */

/* Janela principal (pertence ao sdl.c — não destruímos ela aqui) */
static SDL_Window *s_window = nullptr;
static SDL_GLContext s_gl_context = nullptr;
static bool s_rewind_enabled = false;
static int s_rewind_head = 0;
static int s_rewind_count = 0;
static int s_rewind_frame_counter = 0;

/* Textura OpenGL do frame do jogo (160×144) */
static GLuint s_game_tex = 0;
static GLuint s_prev_game_tex = 0;
static bool s_prev_game_valid = false;

/* ── Flags de visibilidade dos painéis (menu Debug) ── */
static bool s_show_screen = true;
static bool s_show_cpu = true;
static bool s_show_disasm = true;
static bool s_show_memory = true;
static bool s_show_gpu = false;
static bool s_show_oam = false;
static bool s_show_tiles = false;
static bool s_show_tilemap = false;
static bool s_show_profiler = false;
static bool s_show_status_bar = true;

/* ── Configurações de vídeo ── */
static bool s_vsync = true;
static bool s_bilinear = false;
static bool s_fullscreen = false;
static bool s_show_fps = false;

/* ── Configurações de áudio ── */
static bool s_audio_muted = false;
static float s_audio_volume = 1.0f;

/* ── FPS counter ── */
static uint64_t s_fps_last_ns = 0;
static int s_fps_frames = 0;
static float s_fps_current = 0.0f;

/* ── About popup ── */
static bool s_show_about = false;
static char s_status_message[160] = "";
static uint64_t s_status_message_until_ms = 0;

/* ── File dialog (SDL3 assíncrono) ── */
static char s_pending_rom[4096] = "";
static bool s_dialog_active = false;

/* ── ROMs recentes ── */
static std::vector<std::string> s_recent_roms;

/* ────────────────────────────────────────────────────────── */
/* Estado: Emulador                                            */
/* ────────────────────────────────────────────────────────── */
static bool s_fast_forward = false;
static float s_ff_speed_factor = 2.0f; /* multiplicador de velocidade */
static bool s_start_paused = false;
static int s_save_slot = 0;

/* ────────────────────────────────────────────────────────── */
/* Estado: Vídeo avançado                                      */
/* ────────────────────────────────────────────────────────── */
/* Escala: negativos = modos de ajuste, positivos = escala inteira manual. */
static const int VIDEO_SCALE_FIT = 0;
static const int VIDEO_SCALE_INTEGER = -1;
static const int VIDEO_SCALE_FIT_WIDTH = -2;
static const int VIDEO_SCALE_FIT_HEIGHT = -3;
static int s_video_scale = VIDEO_SCALE_FIT;
static bool s_scanlines = false;
static float s_scanlines_intensity = 0.5f;
static bool s_mix_frames = false;
static float s_mix_frames_int = 0.3f;
static float s_bg_color[3] = {0.10f, 0.10f, 0.10f};
static bool s_show_call_stack = false;
static bool s_show_hw_viz = false;
static bool s_show_cpu_viz = false;
static bool s_show_transistor_viz = false;
static bool s_show_hw_schematic = false;

/* ────────────────────────────────────────────────────────── */
/* Estado: Debug UI — fonte                                    */
/* ────────────────────────────────────────────────────────── */
static int s_debug_font_size = 1; /* 0=pequena, 1=normal, 2=grande, 3=enorme */

static const char *UI_CONFIG_PATH = "gaembuoy_ui.ini";

static float debug_font_scale(void)
{
     switch (s_debug_font_size)
     {
     case 0:
          return 0.75f;
     case 2:
          return 1.30f;
     case 3:
          return 1.60f;
     case 1:
     default:
          return 1.00f;
     }
}

static void apply_debug_font_scale(void)
{
     ImGuiIO &io = ImGui::GetIO();
     io.FontGlobalScale = debug_font_scale();
}

static void apply_texture_filter(void)
{
     if (!s_game_tex)
          return;

     GLint f = s_bilinear ? GL_LINEAR : GL_NEAREST;
     glBindTexture(GL_TEXTURE_2D, s_game_tex);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
     glBindTexture(GL_TEXTURE_2D, 0);
}

static void apply_ui_config(const debug_ui_config &cfg)
{
     s_show_screen = cfg.show_screen;
     s_show_cpu = cfg.show_cpu;
     s_show_disasm = cfg.show_disasm;
     s_show_memory = cfg.show_memory;
     s_show_gpu = cfg.show_gpu;
     s_show_oam = cfg.show_oam;
     s_show_tiles = cfg.show_tiles;
     s_show_tilemap = cfg.show_tilemap;
     s_show_profiler = cfg.show_profiler;
     s_show_call_stack = cfg.show_call_stack;
     s_show_status_bar = cfg.show_status_bar;
     s_show_hw_viz          = cfg.show_hw_viz;
     s_show_cpu_viz         = cfg.show_cpu_viz;
     s_show_transistor_viz  = cfg.show_transistor_viz;
     s_show_hw_schematic    = cfg.show_hw_schematic;

     s_vsync = cfg.vsync;
     s_bilinear = cfg.bilinear;
     s_show_fps = cfg.show_fps;
     s_video_scale = cfg.video_scale;
     s_scanlines = cfg.scanlines;
     s_scanlines_intensity = cfg.scanlines_intensity;
     s_mix_frames = cfg.mix_frames;
     s_mix_frames_int = cfg.mix_frames_intensity;
     s_bg_color[0] = cfg.background_color[0];
     s_bg_color[1] = cfg.background_color[1];
     s_bg_color[2] = cfg.background_color[2];

     s_audio_muted = cfg.audio_muted;
     s_audio_volume = cfg.audio_volume;

     s_start_paused = cfg.start_paused;
     s_ff_speed_factor = cfg.fast_forward_speed;
     s_debug_font_size = cfg.debug_font_size;
     s_save_slot = cfg.save_slot;
     for (unsigned i = 0; i < 8; i++)
          gb_sdl_set_key_mapping(i, (SDL_Keycode)cfg.input_keys[i]);

     s_recent_roms = cfg.recent_roms;
}

static void capture_ui_config(debug_ui_config *cfg)
{
     debug_ui_config_defaults(cfg);

     cfg->show_screen = s_show_screen;
     cfg->show_cpu = s_show_cpu;
     cfg->show_disasm = s_show_disasm;
     cfg->show_memory = s_show_memory;
     cfg->show_gpu = s_show_gpu;
     cfg->show_oam = s_show_oam;
     cfg->show_tiles = s_show_tiles;
     cfg->show_tilemap = s_show_tilemap;
     cfg->show_profiler = s_show_profiler;
     cfg->show_call_stack = s_show_call_stack;
     cfg->show_status_bar = s_show_status_bar;
     cfg->show_hw_viz          = s_show_hw_viz;
     cfg->show_cpu_viz         = s_show_cpu_viz;
     cfg->show_transistor_viz  = s_show_transistor_viz;
     cfg->show_hw_schematic    = s_show_hw_schematic;

     cfg->vsync = s_vsync;
     cfg->bilinear = s_bilinear;
     cfg->show_fps = s_show_fps;
     cfg->video_scale = s_video_scale;
     cfg->scanlines = s_scanlines;
     cfg->scanlines_intensity = s_scanlines_intensity;
     cfg->mix_frames = s_mix_frames;
     cfg->mix_frames_intensity = s_mix_frames_int;
     cfg->background_color[0] = s_bg_color[0];
     cfg->background_color[1] = s_bg_color[1];
     cfg->background_color[2] = s_bg_color[2];

     cfg->audio_muted = s_audio_muted;
     cfg->audio_volume = s_audio_volume;

     cfg->start_paused = s_start_paused;
     cfg->fast_forward_speed = s_ff_speed_factor;
     cfg->debug_font_size = s_debug_font_size;
     cfg->save_slot = s_save_slot;
     for (unsigned i = 0; i < 8; i++)
          cfg->input_keys[i] = (int)gb_sdl_get_key_mapping(i);

     cfg->recent_roms = s_recent_roms;
}

static void rewind_slot_path(int slot, char *path, size_t path_len)
{
     snprintf(path, path_len, "/tmp/gaembuoy-rewind-%02d.gbst", slot);
}

static void rewind_reset(void)
{
     s_rewind_head = 0;
     s_rewind_count = 0;
     s_rewind_frame_counter = 0;
}

static void rewind_capture(struct gb *gb)
{
     if (!s_rewind_enabled || !gb->cart.rom ||
         (gb->debug.enabled && gb->debug.state == GB_DEBUG_PAUSED))
          return;

     if (++s_rewind_frame_counter < 30)
          return;
     s_rewind_frame_counter = 0;

     char path[64];
     rewind_slot_path(s_rewind_head, path, sizeof(path));
     if (gb_state_save(gb, path))
     {
          s_rewind_head = (s_rewind_head + 1) % 120;
          if (s_rewind_count < 120)
               s_rewind_count++;
     }
}

static bool rewind_step(struct gb *gb)
{
     if (!gb->cart.rom || s_rewind_count <= 0)
          return false;

     s_rewind_head = (s_rewind_head + 119) % 120;
     char path[64];
     rewind_slot_path(s_rewind_head, path, sizeof(path));
     if (!gb_state_load(gb, path))
          return false;

     s_rewind_count--;
     s_rewind_frame_counter = 0;
     return true;
}

static bool load_ui_config(void)
{
     debug_ui_config cfg;
     bool loaded = debug_ui_config_load(&cfg, UI_CONFIG_PATH);
     apply_ui_config(cfg);
     return loaded;
}

static bool reload_ui_config(void)
{
     debug_ui_config cfg;
     if (!debug_ui_config_load(&cfg, UI_CONFIG_PATH))
          return false;

     apply_ui_config(cfg);
     return true;
}

static bool save_ui_config(void)
{
     debug_ui_config cfg;
     capture_ui_config(&cfg);
     return debug_ui_config_save(&cfg, UI_CONFIG_PATH);
}

static void set_status_message(const char *fmt, ...)
{
     va_list ap;
     va_start(ap, fmt);
     vsnprintf(s_status_message, sizeof(s_status_message), fmt, ap);
     va_end(ap);
     s_status_message_until_ms = SDL_GetTicks() + 3000;
}

/* Altura fixa do painel de memória na parte inferior */
static const int MEM_PANEL_H = 320;

/* Flags comuns para janelas fixas (não movíveis/redimensionáveis) */
static const ImGuiWindowFlags FIXED_WIN_FLAGS =
    ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoCollapse;

/* ────────────────────────────────────────────────────────── */
/* Cores                                                      */
/* ────────────────────────────────────────────────────────── */

static ImVec4 color_green = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
static ImVec4 color_red = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
static ImVec4 color_yellow = ImVec4(0.9f, 0.9f, 0.1f, 1.0f);
static ImVec4 color_gray = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
static ImVec4 color_cyan = ImVec4(0.2f, 0.9f, 0.9f, 1.0f);
static ImVec4 color_orange = ImVec4(1.0f, 0.55f, 0.1f, 1.0f);
static ImVec4 color_white = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
/* Azul-céu para BC — distinto do cyan usado no PC */
static ImVec4 color_skyblue = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);

static ImVec4 ui_lerp(const ImVec4 &a, const ImVec4 &b, float t)
{
     return ImVec4(a.x + (b.x - a.x) * t,
                   a.y + (b.y - a.y) * t,
                   a.z + (b.z - a.z) * t,
                   a.w + (b.w - a.w) * t);
}

static void set_style(void)
{
     ImGuiStyle &style = ImGui::GetStyle();

     // --- Layout & Shape ---
     style.Alpha = 1.0f;
     style.DisabledAlpha = 0.45f;
     style.WindowPadding = ImVec2(12.0f, 10.0f);
     style.WindowRounding = 6.0f;
     style.WindowBorderSize = 1.0f;
     style.WindowMinSize = ImVec2(64.0f, 48.0f);
     style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
     style.WindowMenuButtonPosition = ImGuiDir_Left;
     style.ChildRounding = 4.0f;
     style.ChildBorderSize = 1.0f;
     style.PopupRounding = 6.0f;
     style.PopupBorderSize = 1.0f;
     style.FramePadding = ImVec2(8.0f, 4.0f);
     style.FrameRounding = 4.0f;
     style.FrameBorderSize = 0.0f;
     style.ItemSpacing = ImVec2(8.0f, 6.0f);
     style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
     style.CellPadding = ImVec2(6.0f, 4.0f);
     style.IndentSpacing = 18.0f;
     style.ColumnsMinSpacing = 8.0f;
     style.ScrollbarSize = 12.0f;
     style.ScrollbarRounding = 4.0f;
     style.GrabMinSize = 10.0f;
     style.GrabRounding = 4.0f;
     style.TabRounding = 4.0f;
     style.TabBorderSize = 0.0f;
     style.ColorButtonPosition = ImGuiDir_Right;
     style.ButtonTextAlign = ImVec2(0.5f, 0.5f);
     style.SelectableTextAlign = ImVec2(0.0f, 0.5f);

     // --- Palette ---
     // Backgrounds: escala de cinza fria, característica de IDEs
     const ImVec4 bg = ImVec4(0.122f, 0.125f, 0.133f, 1.0f);       // editor bg
     const ImVec4 chrome = ImVec4(0.098f, 0.101f, 0.109f, 1.0f);   // titlebar / menubar
     const ImVec4 panel = ImVec4(0.145f, 0.149f, 0.157f, 1.0f);    // sidebar / popup
     const ImVec4 panel2 = ImVec4(0.180f, 0.185f, 0.196f, 1.0f);   // inputs / headers
     const ImVec4 panel3 = ImVec4(0.215f, 0.221f, 0.234f, 1.0f);   // hover surface
     const ImVec4 border = ImVec4(0.255f, 0.260f, 0.278f, 1.0f);   // bordas sutis
     const ImVec4 text = ImVec4(0.918f, 0.925f, 0.937f, 1.0f);     // texto principal
     const ImVec4 text_dim = ImVec4(0.520f, 0.535f, 0.570f, 1.0f); // texto secundário

     // Accent: azul corporativo (similar ao VS Code / Fluent Design)
     const ImVec4 accent = ImVec4(0.239f, 0.525f, 0.878f, 1.0f);       // #3D86E0
     const ImVec4 accent_light = ImVec4(0.380f, 0.635f, 0.940f, 1.0f); // hover mais claro
     const ImVec4 accent_dim = ImVec4(0.239f, 0.525f, 0.878f, 0.35f);  // accent transparente

     // --- Colors ---
     style.Colors[ImGuiCol_Text] = text;
     style.Colors[ImGuiCol_TextDisabled] = text_dim;

     style.Colors[ImGuiCol_WindowBg] = bg;
     style.Colors[ImGuiCol_ChildBg] = ImVec4(0.110f, 0.112f, 0.120f, 1.0f);
     style.Colors[ImGuiCol_PopupBg] = panel;

     style.Colors[ImGuiCol_Border] = border;
     style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

     style.Colors[ImGuiCol_FrameBg] = panel2;
     style.Colors[ImGuiCol_FrameBgHovered] = panel3;
     style.Colors[ImGuiCol_FrameBgActive] = ui_lerp(panel2, accent, 0.40f);

     style.Colors[ImGuiCol_TitleBg] = chrome;
     style.Colors[ImGuiCol_TitleBgActive] = chrome;
     style.Colors[ImGuiCol_TitleBgCollapsed] = chrome;
     style.Colors[ImGuiCol_MenuBarBg] = chrome;

     style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
     style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.310f, 0.320f, 0.340f, 1.0f);
     style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.420f, 0.430f, 0.460f, 1.0f);
     style.Colors[ImGuiCol_ScrollbarGrabActive] = accent;

     style.Colors[ImGuiCol_CheckMark] = accent_light;
     style.Colors[ImGuiCol_SliderGrab] = accent;
     style.Colors[ImGuiCol_SliderGrabActive] = accent_light;

     style.Colors[ImGuiCol_Button] = panel2;
     style.Colors[ImGuiCol_ButtonHovered] = ui_lerp(panel2, accent, 0.40f);
     style.Colors[ImGuiCol_ButtonActive] = accent;

     style.Colors[ImGuiCol_Header] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
     style.Colors[ImGuiCol_HeaderHovered] = panel3;
     style.Colors[ImGuiCol_HeaderActive] = ui_lerp(panel2, accent, 0.50f);

     style.Colors[ImGuiCol_Separator] = border;
     style.Colors[ImGuiCol_SeparatorHovered] = ui_lerp(border, accent, 0.60f);
     style.Colors[ImGuiCol_SeparatorActive] = accent;

     style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
     style.Colors[ImGuiCol_ResizeGripHovered] = accent_dim;
     style.Colors[ImGuiCol_ResizeGripActive] = accent;

     style.Colors[ImGuiCol_Tab] = chrome;
     style.Colors[ImGuiCol_TabHovered] = ui_lerp(chrome, accent, 0.30f);
     style.Colors[ImGuiCol_TabSelected] = bg;
     style.Colors[ImGuiCol_TabSelectedOverline] = accent;
     style.Colors[ImGuiCol_TabDimmed] = chrome;
     style.Colors[ImGuiCol_TabDimmedSelected] = ui_lerp(chrome, bg, 0.50f);
     style.Colors[ImGuiCol_TabDimmedSelectedOverline] = border;

     style.Colors[ImGuiCol_PlotLines] = accent;
     style.Colors[ImGuiCol_PlotLinesHovered] = accent_light;
     style.Colors[ImGuiCol_PlotHistogram] = accent;
     style.Colors[ImGuiCol_PlotHistogramHovered] = accent_light;

     style.Colors[ImGuiCol_TableHeaderBg] = panel;
     style.Colors[ImGuiCol_TableBorderStrong] = border;
     style.Colors[ImGuiCol_TableBorderLight] = ImVec4(0.200f, 0.205f, 0.218f, 1.0f);
     style.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
     style.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.030f);

     style.Colors[ImGuiCol_TextSelectedBg] = accent_dim;
     style.Colors[ImGuiCol_DragDropTarget] = accent_light;
     style.Colors[ImGuiCol_NavCursor] = accent;
     style.Colors[ImGuiCol_NavWindowingHighlight] = ImVec4(1.0f, 1.0f, 1.0f, 0.60f);
     style.Colors[ImGuiCol_NavWindowingDimBg] = ImVec4(0.06f, 0.06f, 0.07f, 0.40f);
     style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.04f, 0.04f, 0.05f, 0.70f);
}

static const char *gpu_mode_name(int mode)
{
     switch (mode)
     {
     case 0:
          return "HBlank (0)";
     case 1:
          return "VBlank (1)";
     case 2:
          return "OAM Scan (2)";
     case 3:
          return "Drawing (3)";
     }
     return "???";
}

/* ────────────────────────────────────────────────────────── */
/* File dialog (SDL3 nativo, assíncrono)                      */
/* ────────────────────────────────────────────────────────── */

static void SDLCALL file_dialog_callback(void * /*userdata*/,
                                         const char *const *filelist,
                                         int /*filter*/)
{
     s_dialog_active = false;
     if (!filelist || !filelist[0])
          return;
     SDL_strlcpy(s_pending_rom, filelist[0], sizeof(s_pending_rom));
}

static void open_rom_dialog(void)
{
     if (s_dialog_active)
          return;
     s_dialog_active = true;
     SDL_DialogFileFilter filters[] = {{"ROM Files", "gb;gbc;rom;bin"}};
     SDL_ShowOpenFileDialog(file_dialog_callback, nullptr, s_window, filters, 1, nullptr, false);
}

static void push_recent_rom(const char *path)
{
     std::string p(path);
     for (auto it = s_recent_roms.begin(); it != s_recent_roms.end(); ++it)
          if (*it == p)
          {
               s_recent_roms.erase(it);
               break;
          }
     s_recent_roms.insert(s_recent_roms.begin(), p);
     if (s_recent_roms.size() > 10)
          s_recent_roms.resize(10);
}

static const char *run_state_name(struct gb *gb)
{
     if (!gb->cart.rom)
          return "Idle";
     if (!gb->debug.enabled)
          return "Rodando";

     switch (gb->debug.state)
     {
     case GB_DEBUG_PAUSED:
          return "Pausado";
     case GB_DEBUG_STEPPING:
          return "Step";
     case GB_DEBUG_RUNNING:
          return "Rodando";
     default:
          return "Debug";
     }
}

static void draw_status_bar(struct gb *gb)
{
     if (!s_show_status_bar)
          return;

     ImGuiViewport *vp = ImGui::GetMainViewport();
     const float h = 25.0f;
     ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - h), ImGuiCond_Always);
     ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h), ImGuiCond_Always);
     ImGui::SetNextWindowBgAlpha(1.0f);

     ImGuiWindowFlags flags =
         ImGuiWindowFlags_NoTitleBar |
         ImGuiWindowFlags_NoResize |
         ImGuiWindowFlags_NoMove |
         ImGuiWindowFlags_NoScrollbar |
         ImGuiWindowFlags_NoScrollWithMouse |
         ImGuiWindowFlags_NoSavedSettings |
         ImGuiWindowFlags_NoBringToFrontOnFocus;

     if (!ImGui::Begin("##status_bar", nullptr, flags))
     {
          ImGui::End();
          return;
     }

     ImGui::TextColored(color_green, "%s", debug_ui_action_rom_title(gb));
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "|");
     ImGui::SameLine();
     ImGui::Text("%s", run_state_name(gb));
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "|");
     ImGui::SameLine();
     ImGui::Text("%s", gb->gbc ? "CGB" : "DMG");

     if (gb->cart.rom)
     {
          ImGui::SameLine();
          ImGui::TextColored(color_gray, "|");
          ImGui::SameLine();
          ImGui::Text("LY %03u", gb->gpu.ly);
          ImGui::SameLine();
          ImGui::TextColored(color_gray, "|");
          ImGui::SameLine();
          ImGui::Text("%s", gpu_mode_name(gb_gpu_get_mode(gb)));
          ImGui::SameLine();
          ImGui::TextColored(color_gray, "|");
          ImGui::SameLine();
          ImGui::Text("ROM bank %u/%u", gb->cart.cur_rom_bank, gb->cart.rom_banks);
     }

     if (s_fast_forward)
     {
          ImGui::SameLine();
          ImGui::TextColored(color_yellow, "| FF %.1fx", s_ff_speed_factor);
     }

     if (s_status_message[0] && SDL_GetTicks() < s_status_message_until_ms)
     {
          ImGui::SameLine();
          ImGui::TextColored(color_gray, "|");
          ImGui::SameLine();
          ImGui::TextColored(color_cyan, "%s", s_status_message);
     }

     char right[48];
     snprintf(right, sizeof(right), "%.0f FPS", s_fps_current);
     float right_w = ImGui::CalcTextSize(right).x;
     float avail = ImGui::GetContentRegionAvail().x;
     if (avail > right_w)
          ImGui::SameLine(ImGui::GetCursorPosX() + avail - right_w);
     ImGui::TextColored(s_fps_current >= 55.0f ? color_green : color_yellow, "%s", right);

     ImGui::End();
}

/* ────────────────────────────────────────────────────────── */
/* Menu principal                                             */
/* ────────────────────────────────────────────────────────── */

static void draw_main_menu(struct gb *gb)
{
     if (!ImGui::BeginMainMenuBar())
          return;

     bool has_rom = gb->cart.rom != nullptr;
     bool has_save_ram = has_rom && gb->cart.save_file != nullptr;
     bool paused = gb->debug.enabled && gb->debug.state == GB_DEBUG_PAUSED;
     bool dbg = gb->debug.enabled;

     /* ═══════════════════════════════════════════════════ */
     /* Arquivo                                             */
     /* ═══════════════════════════════════════════════════ */
     if (ImGui::BeginMenu("Arquivo"))
     {
          if (ImGui::MenuItem("Abrir ROM...", "Ctrl+O"))
               open_rom_dialog();

          if (ImGui::BeginMenu("Recentes", !s_recent_roms.empty()))
          {
               for (auto &r : s_recent_roms)
               {
                    const char *name = r.c_str();
                    const char *slash = strrchr(name, '/');
                    if (slash)
                         name = slash + 1;
                    if (ImGui::MenuItem(name))
                         SDL_strlcpy(s_pending_rom, r.c_str(), sizeof(s_pending_rom));
               }
               ImGui::Separator();
               if (ImGui::MenuItem("Limpar Recentes"))
               {
                    s_recent_roms.clear();
                    set_status_message("ROMs recentes limpas");
               }
               ImGui::EndMenu();
          }

          ImGui::Separator();

          if (ImGui::MenuItem("Recarregar ROM", "Ctrl+R", false, has_rom))
          {
               if (!s_recent_roms.empty())
                    SDL_strlcpy(s_pending_rom, s_recent_roms[0].c_str(), sizeof(s_pending_rom));
          }

          ImGui::Separator();

          if (ImGui::MenuItem("Salvar Screenshot", "Ctrl+P", false, has_rom))
          {
               char message[160];
               debug_ui_action_save_screenshot(gb, message, sizeof(message));
               set_status_message("%s", message);
          }

          ImGui::Separator();

          if (ImGui::MenuItem("Salvar Configura\xc3\xa7\xc3\xa3o"))
          {
               if (save_ui_config())
                    set_status_message("Configura\xc3\xa7\xc3\xa3o salva em %s", UI_CONFIG_PATH);
               else
                    set_status_message("Falha ao salvar %s", UI_CONFIG_PATH);
          }

          if (ImGui::MenuItem("Recarregar Configura\xc3\xa7\xc3\xa3o"))
          {
               if (reload_ui_config())
               {
                    apply_debug_font_scale();
                    SDL_GL_SetSwapInterval(s_vsync ? 1 : 0);
                    apply_texture_filter();
                    gb_sdl_set_audio_gain(gb, s_audio_muted ? 0.0f : s_audio_volume);
                    set_status_message("Configura\xc3\xa7\xc3\xa3o recarregada");
               }
               else
                    set_status_message("%s ainda n\xc3\xa3o existe", UI_CONFIG_PATH);
          }

          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
               ImGui::SetTooltip("Prefer\xc3\xaancias da UI ficam em %s; layout das janelas fica em imgui.ini.", UI_CONFIG_PATH);

          ImGui::Separator();

          if (ImGui::MenuItem("Salvar RAM Agora", nullptr, false, has_save_ram))
               gb_cart_save_ram_now(gb);

          if (ImGui::MenuItem("Recarregar RAM do .sav", nullptr, false, has_save_ram))
               gb_cart_load_ram_now(gb);

          if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !has_save_ram)
               ImGui::SetTooltip("Dispon\xc3\xadvel apenas para cartuchos com bateria/arquivo .sav.");

          ImGui::Separator();

          if (ImGui::BeginMenu("Save State", has_rom))
          {
               if (ImGui::BeginMenu("Slot"))
               {
                    for (int i = 0; i < 5; ++i)
                    {
                         char label[32];
                         snprintf(label, sizeof(label), "Slot %d%s", i + 1,
                                  debug_ui_action_state_slot_exists(gb, i) ? "  *" : "");
                         if (ImGui::MenuItem(label, nullptr, s_save_slot == i))
                              s_save_slot = i;
                    }
                    ImGui::EndMenu();
               }

               ImGui::Separator();

               if (ImGui::MenuItem("Salvar State", "F6", false, has_rom))
               {
                    char message[160];
                    debug_ui_action_save_state_slot(gb, s_save_slot, message, sizeof(message));
                    set_status_message("%s", message);
               }

               if (ImGui::MenuItem("Carregar State", "F8", false,
                                   has_rom && debug_ui_action_state_slot_exists(gb, s_save_slot)))
               {
                    char message[160];
                    debug_ui_action_load_state_slot(gb, s_save_slot, message, sizeof(message));
                    set_status_message("%s", message);
               }

               ImGui::Separator();
               ImGui::TextColored(color_gray, "Slot atual: %d", s_save_slot + 1);
               ImGui::EndMenu();
          }

          ImGui::Separator();

          if (ImGui::MenuItem("Informa\xc3\xa7\xc3\xb5"
                              "es da ROM...",
                              nullptr, false, has_rom))
               g_show_rom_info = true;

          ImGui::Separator();

          if (ImGui::MenuItem("Sair", "Alt+F4"))
               gb->quit = true;

          ImGui::EndMenu();
     }

     /* ═══════════════════════════════════════════════════ */
     /* Emulador  (novo — inspirado no Gearsystem)          */
     /* ═══════════════════════════════════════════════════ */
     if (ImGui::BeginMenu("Emulador"))
     {
          /* ── Pausar / Continuar ── */
          if (ImGui::MenuItem("Pausar", "F5", paused, has_rom))
          {
               if (paused)
                    gb->debug.state = GB_DEBUG_RUNNING;
               else
               {
                    gb->debug.enabled = true;
                    gb->debug.state = GB_DEBUG_PAUSED;
               }
          }

          if (ImGui::MenuItem("Resetar", nullptr, false, has_rom))
               debug_ui_action_reset(gb, s_start_paused);

          ImGui::Separator();

          /* ── Avanço Rápido ── */
          if (ImGui::MenuItem("Avan\xc3\xa7o R\xc3\xa1"
                              "pido",
                              "Tab", s_fast_forward, has_rom))
               s_fast_forward = !s_fast_forward;
          if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Multiplica a velocidade de emula\xc3\xa7\xc3\xa3o.\nMantenha Tab pressionado para ativar temporariamente.");

          if (ImGui::BeginMenu("Velocidade do Avan\xc3\xa7o R\xc3\xa1"
                               "pido",
                               has_rom))
          {
               ImGui::SetNextItemWidth(160.0f);
               ImGui::SliderFloat("##ff_speed", &s_ff_speed_factor, 1.5f, 8.0f, "%.1fx",
                                  ImGuiSliderFlags_AlwaysClamp);
               ImGui::Separator();
               if (ImGui::MenuItem("1.5x", nullptr, s_ff_speed_factor == 1.5f))
                    s_ff_speed_factor = 1.5f;
               if (ImGui::MenuItem("2x", nullptr, s_ff_speed_factor == 2.0f))
                    s_ff_speed_factor = 2.0f;
               if (ImGui::MenuItem("3x", nullptr, s_ff_speed_factor == 3.0f))
                    s_ff_speed_factor = 3.0f;
               if (ImGui::MenuItem("4x", nullptr, s_ff_speed_factor == 4.0f))
                    s_ff_speed_factor = 4.0f;
               if (ImGui::MenuItem("Ilimitado (8x)", nullptr, s_ff_speed_factor == 8.0f))
                    s_ff_speed_factor = 8.0f;
               ImGui::EndMenu();
          }

          ImGui::Separator();

          /* ── Modo do console ── */
          if (ImGui::BeginMenu("Modo do Console", has_rom))
          {
               if (ImGui::MenuItem("DMG (Game Boy)", nullptr, !gb->gbc))
               {
                    if (gb->gbc)
                    {
                         gb->gbc = false; /* aviso: requer reset */
                    }
               }
               if (ImGui::MenuItem("CGB (Game Boy Color)", nullptr, gb->gbc))
               {
                    if (!gb->gbc)
                    {
                         gb->gbc = true;
                    }
               }
               ImGui::Separator();
               ImGui::TextColored(color_gray, "  Modo atual: %s", gb->gbc ? "GBC" : "DMG");
               ImGui::TextColored(color_yellow, "  (Recarregue a ROM para aplicar)");
               ImGui::EndMenu();
          }

          ImGui::Separator();

          /* ── Opções de inicialização ── */
          ImGui::MenuItem("Iniciar Pausado", nullptr, &s_start_paused);
          if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Quando ativado, a emula\xc3\xa7\xc3\xa3o inicia em modo pausa ao carregar uma ROM.");

          ImGui::Separator();

          if (ImGui::MenuItem("Informa\xc3\xa7\xc3\xb5"
                              "es da ROM...",
                              nullptr, false, has_rom))
               g_show_rom_info = true;

          ImGui::EndMenu();
     }

     /* ═══════════════════════════════════════════════════ */
     /* Vídeo  (enriquecido)                               */
     /* ═══════════════════════════════════════════════════ */
     if (ImGui::BeginMenu("V\xc3\xad"
                          "deo"))
     {
          /* ── Tela Cheia ── */
          if (ImGui::MenuItem("Tela Cheia", "F11", s_fullscreen))
          {
               s_fullscreen = !s_fullscreen;
               SDL_SetWindowFullscreen(s_window, s_fullscreen);
          }

          ImGui::Separator();

          /* ── Escala ── */
          if (ImGui::BeginMenu("Escala"))
          {
               if (ImGui::MenuItem("Ajustar \xc3\xa0 janela", nullptr, s_video_scale == VIDEO_SCALE_FIT))
                    s_video_scale = VIDEO_SCALE_FIT;
               if (ImGui::MenuItem("Inteira autom\xc3\xa1tica", nullptr, s_video_scale == VIDEO_SCALE_INTEGER))
                    s_video_scale = VIDEO_SCALE_INTEGER;
               if (ImGui::MenuItem("Ajustar \xc3\xa0 largura", nullptr, s_video_scale == VIDEO_SCALE_FIT_WIDTH))
                    s_video_scale = VIDEO_SCALE_FIT_WIDTH;
               if (ImGui::MenuItem("Ajustar \xc3\xa0 altura", nullptr, s_video_scale == VIDEO_SCALE_FIT_HEIGHT))
                    s_video_scale = VIDEO_SCALE_FIT_HEIGHT;

               ImGui::Separator();
               for (int scale = 1; scale <= 8; ++scale)
               {
                    char label[48];
                    snprintf(label, sizeof(label), "%d\xc3\x97  (%d \xc3\x97 %d)",
                             scale, GB_LCD_WIDTH * scale, GB_LCD_HEIGHT * scale);
                    if (ImGui::MenuItem(label, nullptr, s_video_scale == scale))
                         s_video_scale = scale;
               }
               ImGui::EndMenu();
          }

          ImGui::Separator();

          /* ── VSync / FPS ── */
          if (ImGui::MenuItem("VSync", nullptr, s_vsync))
          {
               s_vsync = !s_vsync;
               SDL_GL_SetSwapInterval(s_vsync ? 1 : 0);
          }
          if (ImGui::MenuItem("Mostrar FPS", nullptr, s_show_fps))
               s_show_fps = !s_show_fps;
          ImGui::MenuItem("Barra de Status", nullptr, &s_show_status_bar);

          ImGui::Separator();

          /* ── Filtros de imagem ── */
          if (ImGui::MenuItem("Filtro Bilinear", nullptr, s_bilinear))
          {
               s_bilinear = !s_bilinear;
               apply_texture_filter();
          }
          if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Suaviza a imagem ao escalar (Nearest = pixels n\xc3\xad"
                                 "tidos).");

          /* ── Scanlines ── */
          if (ImGui::BeginMenu("Scanlines"))
          {
               ImGui::MenuItem("Ativar Scanlines", nullptr, &s_scanlines);
               ImGui::SetNextItemWidth(200.0f);
               ImGui::SliderFloat("Intensidade##sl", &s_scanlines_intensity,
                                  0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
               ImGui::EndMenu();
          }

          /* ── Ghosting de Frame ── */
          if (ImGui::BeginMenu("Persist\xc3\xaa"
                               "ncia de Frame"))
          {
               ImGui::MenuItem("Ativar Ghosting", nullptr, &s_mix_frames);
               if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Efeito visual de persist\xc3\xaa"
                                      "ncia — simula o LCD do DMG.");
               ImGui::SetNextItemWidth(200.0f);
               ImGui::SliderFloat("Intensidade##gf", &s_mix_frames_int,
                                  0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
               ImGui::EndMenu();
          }

          ImGui::Separator();

          /* ── Cor de fundo ── */
          if (ImGui::BeginMenu("Cor de Fundo"))
          {
               ImGui::ColorEdit3("##bg_col", s_bg_color,
                                 ImGuiColorEditFlags_NoInputs |
                                     ImGuiColorEditFlags_Float);
               ImGui::SameLine();
               ImGui::Text("Cor de fundo da janela");
               ImGui::Separator();
               if (ImGui::MenuItem("Preto"))
               {
                    s_bg_color[0] = 0.0f;
                    s_bg_color[1] = 0.0f;
                    s_bg_color[2] = 0.0f;
               }
               if (ImGui::MenuItem("Cinza escuro (padr\xc3\xa3o)"))
               {
                    s_bg_color[0] = 0.1f;
                    s_bg_color[1] = 0.1f;
                    s_bg_color[2] = 0.1f;
               }
               if (ImGui::MenuItem("Azul marinho"))
               {
                    s_bg_color[0] = 0.05f;
                    s_bg_color[1] = 0.05f;
                    s_bg_color[2] = 0.15f;
               }
               if (ImGui::MenuItem("Verde escuro"))
               {
                    s_bg_color[0] = 0.02f;
                    s_bg_color[1] = 0.08f;
                    s_bg_color[2] = 0.02f;
               }
               ImGui::EndMenu();
          }

          ImGui::EndMenu();
     }

     /* ═══════════════════════════════════════════════════ */
     /* Entrada  (novo — inspirado no Gearsystem)           */
     /* ═══════════════════════════════════════════════════ */
     draw_input_menu();

     /* ═══════════════════════════════════════════════════ */
     /* Áudio  (enriquecido)                               */
     /* ═══════════════════════════════════════════════════ */
     if (ImGui::BeginMenu("\xc3\x81"
                          "udio"))
     {
          if (ImGui::MenuItem("Mudo", nullptr, s_audio_muted))
          {
               s_audio_muted = !s_audio_muted;
               gb_sdl_set_audio_gain(gb, s_audio_muted ? 0.0f : s_audio_volume);
          }

          ImGui::Separator();

          if (ImGui::BeginMenu("Volume Mestre"))
          {
               ImGui::SetNextItemWidth(200.0f);
               if (ImGui::SliderFloat("##master_vol", &s_audio_volume, 0.0f, 2.0f, "%.2f",
                                      ImGuiSliderFlags_AlwaysClamp))
               {
                    s_audio_muted = false;
                    gb_sdl_set_audio_gain(gb, s_audio_volume);
               }
               if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Acima de 1.00 pode causar distor\xc3\xa7\xc3\xa3o (clipping).");
               ImGui::Separator();
               if (ImGui::MenuItem("25%"))
               {
                    s_audio_volume = 0.25f;
                    gb_sdl_set_audio_gain(gb, s_audio_volume);
                    s_audio_muted = false;
               }
               if (ImGui::MenuItem("50%"))
               {
                    s_audio_volume = 0.50f;
                    gb_sdl_set_audio_gain(gb, s_audio_volume);
                    s_audio_muted = false;
               }
               if (ImGui::MenuItem("100%"))
               {
                    s_audio_volume = 1.00f;
                    gb_sdl_set_audio_gain(gb, s_audio_volume);
                    s_audio_muted = false;
               }
               if (ImGui::MenuItem("150%"))
               {
                    s_audio_volume = 1.50f;
                    gb_sdl_set_audio_gain(gb, s_audio_volume);
                    s_audio_muted = false;
               }
               ImGui::EndMenu();
          }

          ImGui::Separator();

          if (ImGui::BeginMenu("Canais"))
          {
               ImGui::TextDisabled("Silenciar no mixer:");
               ImGui::Separator();
               ImGui::MenuItem("CH1 Pulse A", nullptr, &gb->spu.frontend_mute[0], gb->spu.enable);
               ImGui::MenuItem("CH2 Pulse B", nullptr, &gb->spu.frontend_mute[1], gb->spu.enable);
               ImGui::MenuItem("CH3 Wave", nullptr, &gb->spu.frontend_mute[2], gb->spu.enable);
               ImGui::MenuItem("CH4 Noise", nullptr, &gb->spu.frontend_mute[3], gb->spu.enable);
               ImGui::Separator();
               if (ImGui::MenuItem("Ativar todos"))
               {
                    for (int i = 0; i < 4; ++i)
                         gb->spu.frontend_mute[i] = false;
               }
               if (ImGui::MenuItem("Silenciar todos"))
               {
                    for (int i = 0; i < 4; ++i)
                         gb->spu.frontend_mute[i] = true;
               }
               ImGui::EndMenu();
          }

          ImGui::Separator();

          /* ── Canais do SPU ── */
          ImGui::TextDisabled("Canais SPU (estado atual):");
          auto ch_indicator = [](const char *name, bool active, bool muted)
          {
               ImGui::TextColored(muted ? color_yellow : (active ? color_green : color_gray),
                                  "  %s  %s", name, muted ? "mudo" : (active ? "ativo" : "inativo"));
          };
          ch_indicator("CH1 Pulse A:", gb->spu.nr1.running, gb->spu.frontend_mute[0]);
          ch_indicator("CH2 Pulse B:", gb->spu.nr2.running, gb->spu.frontend_mute[1]);
          ch_indicator("CH3 Wave:   ", gb->spu.nr3.running, gb->spu.frontend_mute[2]);
          ch_indicator("CH4 Noise:  ", gb->spu.nr4.running, gb->spu.frontend_mute[3]);
          ImGui::TextColored(gb->spu.enable ? color_green : color_red,
                             "  APU: %s", gb->spu.enable ? "habilitado" : "desabilitado");

          ImGui::Separator();
          ImGui::TextColored(color_gray, "Sample Rate: %d Hz", GB_SPU_SAMPLE_RATE_HZ);

          ImGui::EndMenu();
     }

     /* ═══════════════════════════════════════════════════ */
     /* Debug  (enriquecido)                               */
     /* ═══════════════════════════════════════════════════ */
     if (ImGui::BeginMenu("Debug"))
     {
          if (ImGui::MenuItem("Ativar Debug", nullptr, &gb->debug.enabled))
          {
               if (gb->debug.enabled)
                    gb->debug.state = GB_DEBUG_PAUSED;
               else
                    gb->debug.state = GB_DEBUG_RUNNING;
          }

          ImGui::Separator();

          ImGui::MenuItem("Sa\xc3\xad"
                          "da do Jogo",
                          nullptr, &s_show_screen);

          ImGui::Separator();

          /* ── Painéis de debug ── */
          ImGui::MenuItem("CPU / Controle", nullptr, &s_show_cpu, dbg);
          ImGui::MenuItem("Disassembly", nullptr, &s_show_disasm, dbg);
          ImGui::MenuItem("Pilha de Chamadas", nullptr, &s_show_call_stack, dbg);
          ImGui::MenuItem("Mem\xc3\xb3"
                          "ria",
                          nullptr, &s_show_memory, dbg);

          ImGui::Separator();

          if (ImGui::BeginMenu("V\xc3\xad"
                               "deo",
                               dbg))
          {
               ImGui::MenuItem("GPU / PPU", nullptr, &s_show_gpu);
               ImGui::MenuItem("OAM (Sprites)", nullptr, &s_show_oam);
               ImGui::MenuItem("VRAM \xe2\x80\x93 Tiles", nullptr, &s_show_tiles);
               ImGui::MenuItem("VRAM \xe2\x80\x93 Tilemap", nullptr, &s_show_tilemap);
               ImGui::EndMenu();
          }

          ImGui::MenuItem("Profiler", nullptr, &s_show_profiler, dbg);

          ImGui::Separator();

          if (ImGui::BeginMenu("Hardware Visualization", dbg))
          {
               ImGui::MenuItem("HW Diagram", nullptr, &s_show_hw_viz);
               if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Diagrama animado de todo o hardware do Game Boy\ncom atividade nos barramentos em tempo real.");
               ImGui::MenuItem("CPU Datapath", nullptr, &s_show_cpu_viz);
               if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Microarquitetura da CPU LR35902:\nfetch/decode/execute, ALU, barramentos de dados.");
               ImGui::MenuItem("Transistor Die", nullptr, &s_show_transistor_viz);
               if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Visualizador fisico do die SM83/LR35902\ncom transistores individuais extraidos do dmg-schematics.");
               ImGui::MenuItem("HW Schematic", nullptr, &s_show_hw_schematic);
               if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Esquematico do hardware do Game Boy DMG\nextraido de gb-schematics (Gekkio, CC-BY 4.0).\nComponentes com highlight de atividade em tempo real.");
               ImGui::EndMenu();
          }

          ImGui::Separator();

          {
               bool trace = gb_debug_trace_enabled();
               if (ImGui::MenuItem("Trace logger", nullptr, &trace, dbg))
               {
                    if (gb_debug_trace_set_enabled(gb, trace, "trace.log"))
                         set_status_message("Trace logger %s", trace ? "ativado" : "desativado");
                    else
                         set_status_message("Falha ao abrir trace.log");
               }
          }

          if (ImGui::MenuItem("Rewind ativo", nullptr, &s_rewind_enabled, has_rom))
          {
               rewind_reset();
               set_status_message("Rewind %s", s_rewind_enabled ? "ativado" : "desativado");
          }
          if (ImGui::MenuItem("Voltar um snapshot", "F9", false, has_rom && s_rewind_count > 0))
          {
               if (rewind_step(gb))
                    set_status_message("Rewind: snapshot restaurado");
               else
                    set_status_message("Rewind: nenhum snapshot valido");
          }

          ImGui::Separator();

          /* ── Opções avançadas ── */
          if (ImGui::BeginMenu("Tamanho de Fonte"))
          {
               if (ImGui::MenuItem("Pequena", nullptr, s_debug_font_size == 0))
               {
                    s_debug_font_size = 0;
                    apply_debug_font_scale();
               }
               if (ImGui::MenuItem("Normal", nullptr, s_debug_font_size == 1))
               {
                    s_debug_font_size = 1;
                    apply_debug_font_scale();
               }
               if (ImGui::MenuItem("Grande", nullptr, s_debug_font_size == 2))
               {
                    s_debug_font_size = 2;
                    apply_debug_font_scale();
               }
               if (ImGui::MenuItem("Enorme", nullptr, s_debug_font_size == 3))
               {
                    s_debug_font_size = 3;
                    apply_debug_font_scale();
               }
               ImGui::EndMenu();
          }

          ImGui::Separator();

          if (ImGui::MenuItem("Resetar Profiler", nullptr, false, dbg))
               gb_debug_reset_profiler(gb);

          ImGui::MenuItem("Mostrar FPS", nullptr, &s_show_fps);

          ImGui::EndMenu();
     }

     /* ═══════════════════════════════════════════════════ */
     /* Sobre                                               */
     /* ═══════════════════════════════════════════════════ */
     if (ImGui::BeginMenu("Sobre"))
     {
          if (ImGui::MenuItem("Sobre Gaembuoy..."))
               s_show_about = true;
          ImGui::EndMenu();
     }

     /* ── FPS no lado direito ── */
     if (s_show_fps)
     {
          char fps_str[32];
          snprintf(fps_str, sizeof(fps_str), "%.0f FPS", s_fps_current);
          float w = ImGui::GetContentRegionAvail().x;
          float tw = ImGui::CalcTextSize(fps_str).x + 8.0f;
          if (w > tw)
               ImGui::SameLine(ImGui::GetCursorPosX() + w - tw);
          ImGui::TextColored(s_fps_current >= 55.0f ? color_green : color_yellow,
                             "%s", fps_str);
     }

     ImGui::EndMainMenuBar();

     /* ── Popups ── */
     if (s_show_about)
     {
          ImGui::OpenPopup("Sobre##modal");
          s_show_about = false;
     }
     if (ImGui::BeginPopupModal("Sobre##modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
     {
          ImGui::Text("Gaembuoy");
          ImGui::TextColored(color_gray, "Emulador de Game Boy / Game Boy Color");
          ImGui::Separator();
          ImGui::Text("Frontend:  SDL3 + Dear ImGui");
          ImGui::Text("Renderer:  OpenGL 3.3 Core");
          ImGui::Separator();
          ImGui::TextDisabled("Atalhos de teclado:");
          ImGui::BulletText("Ctrl+O     Abrir ROM");
          ImGui::BulletText("Ctrl+R     Recarregar ROM");
          ImGui::BulletText("F5         Pausar / Continuar");
          ImGui::BulletText("F10        Step (modo pausado)");
          ImGui::BulletText("F11        Tela Cheia");
          ImGui::BulletText("Tab        Avan\xc3\xa7o R\xc3\xa1"
                            "pido (segurar)");
          ImGui::Separator();
          if (ImGui::Button("Fechar", ImVec2(120, 0)))
               ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
     }

     /* ── Popup de ROM info ── */
     draw_popup_rom_info(gb);
}

/* ────────────────────────────────────────────────────────── */
/* Tela de boas-vindas (sem ROM carregada)                   */
/* ────────────────────────────────────────────────────────── */

static void draw_launcher(void)
{
     ImGuiIO &io = ImGui::GetIO();
     ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                             ImGuiCond_Always, ImVec2(0.5f, 0.5f));
     ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
     ImGui::Begin("##launcher", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

     ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Gaembuoy").x) * 0.5f);
     ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Gaembuoy");
     ImGui::Spacing();
     ImGui::Separator();
     ImGui::Spacing();

     float btn_w = ImGui::GetContentRegionAvail().x;
     if (ImGui::Button("Abrir ROM...", ImVec2(btn_w, 36)))
          open_rom_dialog();

     if (!s_recent_roms.empty())
     {
          ImGui::Spacing();
          ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Recentes:");
          ImGui::Separator();
          for (auto &r : s_recent_roms)
          {
               const char *name = r.c_str();
               const char *slash = strrchr(name, '/');
               if (slash)
                    name = slash + 1;
               if (ImGui::Selectable(name))
                    SDL_strlcpy(s_pending_rom, r.c_str(), sizeof(s_pending_rom));
          }
     }

     ImGui::Spacing();
     ImGui::Separator();
     ImGui::TextColored(ImVec4(0.45f, 0.45f, 0.45f, 1.0f),
                        "Arraste uma ROM para a janela ou use Ctrl+O");

     ImGui::End();
}

/* ────────────────────────────────────────────────────────── */
/* Janela de saída do jogo                                    */
/* ────────────────────────────────────────────────────────── */

static void draw_game_output(void)
{
     ImGui::SetNextWindowSize(ImVec2(340, 330), ImGuiCond_FirstUseEver);
     ImGui::Begin("Sa"
                  "\xc3\xad"
                  "da##game_out",
                  &s_show_screen,
                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

     float avail_w = ImGui::GetContentRegionAvail().x;
     float avail_h = ImGui::GetContentRegionAvail().y;

     float scale = 1.0f;
     float fit_scale = avail_w / (float)GB_LCD_WIDTH < avail_h / (float)GB_LCD_HEIGHT
                           ? avail_w / (float)GB_LCD_WIDTH
                           : avail_h / (float)GB_LCD_HEIGHT;

     if (s_video_scale == VIDEO_SCALE_FIT)
     {
          scale = fit_scale;
          if (scale < 1.0f)
               scale = 1.0f;
     }
     else if (s_video_scale == VIDEO_SCALE_INTEGER)
     {
          scale = floorf(fit_scale);
          if (scale < 1.0f)
               scale = 1.0f;
     }
     else if (s_video_scale == VIDEO_SCALE_FIT_WIDTH)
     {
          scale = avail_w / (float)GB_LCD_WIDTH;
          if (scale < 1.0f)
               scale = 1.0f;
     }
     else if (s_video_scale == VIDEO_SCALE_FIT_HEIGHT)
     {
          scale = avail_h / (float)GB_LCD_HEIGHT;
          if (scale < 1.0f)
               scale = 1.0f;
     }
     else
     {
          scale = (float)s_video_scale;
     }

     ImVec2 img((float)GB_LCD_WIDTH * scale, (float)GB_LCD_HEIGHT * scale);

     /* Centraliza horizontalmente e verticalmente */
     float off_x = (avail_w - img.x) * 0.5f + ImGui::GetCursorPosX();
     float off_y = (avail_h - img.y) * 0.5f + ImGui::GetCursorPosY();
     if (off_x < ImGui::GetCursorPosX())
          off_x = ImGui::GetCursorPosX();
     if (off_y < ImGui::GetCursorPosY())
          off_y = ImGui::GetCursorPosY();
     ImGui::SetCursorPos(ImVec2(off_x, off_y));

     ImVec2 img_screen = ImGui::GetCursorScreenPos();
     ImGui::Image((ImTextureID)(intptr_t)s_game_tex, img);

     if (s_mix_frames && s_prev_game_valid)
     {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          int alpha = (int)(s_mix_frames_int * 160.0f);
          if (alpha < 0)
               alpha = 0;
          if (alpha > 220)
               alpha = 220;
          dl->AddImage((ImTextureID)(intptr_t)s_prev_game_tex,
                       img_screen,
                       ImVec2(img_screen.x + img.x, img_screen.y + img.y),
                       ImVec2(0, 0),
                       ImVec2(1, 1),
                       IM_COL32(255, 255, 255, alpha));
     }

     /* Overlay de Fast Forward */
     if (s_fast_forward)
     {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          char ff_str[16];
          snprintf(ff_str, sizeof(ff_str), ">> %.0fx", s_ff_speed_factor);
          ImVec2 ts = ImGui::CalcTextSize(ff_str);
          float tx = img_screen.x + img.x - ts.x - 6.0f;
          float ty = img_screen.y + 4.0f;
          dl->AddRectFilled(ImVec2(tx - 2, ty - 1),
                            ImVec2(tx + ts.x + 2, ty + ts.y + 1),
                            IM_COL32(0, 0, 0, 160), 3.0f);
          dl->AddText(ImVec2(tx, ty), IM_COL32(255, 220, 50, 255), ff_str);
     }

     /* Scanlines */
     if (s_scanlines)
     {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImU32 sc_col = IM_COL32(0, 0, 0, (int)(s_scanlines_intensity * 210.0f));
          float step = scale * 2.0f;
          if (step < 2.0f)
               step = 2.0f;
          for (float y = img_screen.y; y < img_screen.y + img.y; y += step)
               dl->AddRectFilled(ImVec2(img_screen.x, y),
                                 ImVec2(img_screen.x + img.x, y + scale),
                                 sc_col);
     }

     ImGui::End();
}

/* ────────────────────────────────────────────────────────── */
/* API pública                                                */
/* ────────────────────────────────────────────────────────── */

extern "C" void debug_ui_init(struct gb *gb)
{
     /* Usa a janela principal criada pelo sdl.c */
     s_window = gb_sdl_get_window(gb);
     if (!s_window)
     {
          fprintf(stderr, "debug_ui: janela principal não disponível\n");
          return;
     }

     load_ui_config();

     s_gl_context = SDL_GL_CreateContext(s_window);
     if (!s_gl_context)
     {
          fprintf(stderr, "debug_ui: SDL_GL_CreateContext: %s\n", SDL_GetError());
          s_window = nullptr;
          return;
     }

     SDL_GL_MakeCurrent(s_window, s_gl_context);
     SDL_GL_SetSwapInterval(s_vsync ? 1 : 0);

     IMGUI_CHECKVERSION();
     ImGui::CreateContext();

     ImGuiIO &io = ImGui::GetIO();
     io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
     io.IniFilename = "imgui.ini";
     ImFont *roboto = io.Fonts->AddFontFromMemoryCompressedTTF(
         RobotoMedium_compressed_data,
         RobotoMedium_compressed_size,
         16.0f,
         nullptr,
         io.Fonts->GetGlyphRangesCyrillic());
     if (roboto)
          io.FontDefault = roboto;
     apply_debug_font_scale();

     ImGui::StyleColorsDark();
     set_style();

     ImGui_ImplSDL3_InitForOpenGL(s_window, s_gl_context);
     ImGui_ImplOpenGL3_Init("#version 330 core");

     /* Cria textura do frame do jogo (160×144) */
     auto make_tex = [](GLuint *id, int w, int h)
     {
          glGenTextures(1, id);
          glBindTexture(GL_TEXTURE_2D, *id);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                       GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
          glBindTexture(GL_TEXTURE_2D, 0);
     };
     make_tex(&s_game_tex, GB_LCD_WIDTH, GB_LCD_HEIGHT);
     make_tex(&s_prev_game_tex, GB_LCD_WIDTH, GB_LCD_HEIGHT);
     debug_ui_panels_init();
     apply_texture_filter();
     gb_sdl_set_audio_gain(gb, s_audio_muted ? 0.0f : s_audio_volume);
}

extern "C" void debug_ui_process_event(struct gb *gb, SDL_Event *e)
{
     if (!s_window)
          return;

     ImGui_ImplSDL3_ProcessEvent(e);

     /* Drag-and-drop de arquivo ROM */
     if (e->type == SDL_EVENT_DROP_FILE)
     {
          SDL_strlcpy(s_pending_rom, e->drop.data, sizeof(s_pending_rom));
          SDL_free((void *)e->drop.data);
     }

     if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat)
     {
          if (e->key.key == SDLK_F10 && gb->debug.state == GB_DEBUG_PAUSED)
               gb->debug.state = GB_DEBUG_STEPPING;

          if (e->key.key == SDLK_F5)
          {
               if (gb->debug.state == GB_DEBUG_RUNNING)
                    gb->debug.state = GB_DEBUG_PAUSED;
               else
                    gb->debug.state = GB_DEBUG_RUNNING;
          }

          if (e->key.key == SDLK_F6 && gb->cart.rom)
          {
               char message[160];
               debug_ui_action_save_state_slot(gb, s_save_slot, message, sizeof(message));
               set_status_message("%s", message);
          }

          if (e->key.key == SDLK_F8 && gb->cart.rom)
          {
               if (debug_ui_action_state_slot_exists(gb, s_save_slot))
               {
                    char message[160];
                    debug_ui_action_load_state_slot(gb, s_save_slot, message, sizeof(message));
                    set_status_message("%s", message);
               }
               else
                    set_status_message("Slot %d vazio", s_save_slot + 1);
          }

          if (e->key.key == SDLK_F9 && gb->cart.rom)
          {
               if (rewind_step(gb))
                    set_status_message("Rewind: snapshot restaurado");
               else
                    set_status_message("Rewind: nenhum snapshot disponivel");
          }

          if (e->key.key == SDLK_F11)
          {
               s_fullscreen = !s_fullscreen;
               SDL_SetWindowFullscreen(s_window, s_fullscreen);
          }

          if (e->key.key == SDLK_O &&
              (SDL_GetModState() & SDL_KMOD_CTRL))
               open_rom_dialog();

          if (e->key.key == SDLK_P &&
              (SDL_GetModState() & SDL_KMOD_CTRL) &&
              gb->cart.rom)
          {
               char message[160];
               debug_ui_action_save_screenshot(gb, message, sizeof(message));
               set_status_message("%s", message);
          }

          if (e->key.key == SDLK_R &&
              (SDL_GetModState() & SDL_KMOD_CTRL) &&
              !s_recent_roms.empty())
               SDL_strlcpy(s_pending_rom, s_recent_roms[0].c_str(), sizeof(s_pending_rom));

          /* Tab: ativa/desativa avanço rápido */
          if (e->key.key == SDLK_TAB)
               s_fast_forward = !s_fast_forward;
     }

     /* Tab mantido pressionado = FF ativo enquanto segura */
     if (e->type == SDL_EVENT_KEY_UP && e->key.key == SDLK_TAB)
     {
          /* Se o usuário apenas segurou Tab, desativa ao soltar */
          s_fast_forward = false;
     }
}

extern "C" const char *debug_ui_pending_rom(void)
{
     if (s_pending_rom[0] == '\0')
          return nullptr;
     return s_pending_rom;
}

extern "C" void debug_ui_clear_pending_rom(const char *loaded_path)
{
     push_recent_rom(loaded_path);
     s_pending_rom[0] = '\0';
}

extern "C" bool debug_ui_start_paused(void)
{
     return s_start_paused;
}

extern "C" void debug_ui_render(struct gb *gb)
{
     if (!s_window || !s_gl_context)
          return;

     rewind_capture(gb);

     /* Atualiza contador de FPS a cada segundo */
     {
          uint64_t now = SDL_GetTicksNS();
          s_fps_frames++;
          if (s_fps_last_ns == 0)
               s_fps_last_ns = now;
          uint64_t elapsed = now - s_fps_last_ns;
          if (elapsed >= 1000000000ULL)
          {
               s_fps_current = (float)s_fps_frames * 1e9f / (float)elapsed;
               s_fps_frames = 0;
               s_fps_last_ns = now;
          }
     }

     SDL_GL_MakeCurrent(s_window, s_gl_context);

     /* Faz upload do frame atual do jogo para a textura OpenGL */
     const uint32_t *pixels = gb_sdl_get_pixels(gb);
     glBindTexture(GL_TEXTURE_2D, s_game_tex);
     glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                     GB_LCD_WIDTH, GB_LCD_HEIGHT,
                     GL_BGRA, GL_UNSIGNED_BYTE, pixels);
     glBindTexture(GL_TEXTURE_2D, 0);

     ImGui_ImplOpenGL3_NewFrame();
     ImGui_ImplSDL3_NewFrame();
     ImGui::NewFrame();

     ImGuiViewport *vp = ImGui::GetMainViewport();

     draw_main_menu(gb);

     if (gb->cart.rom == nullptr)
     {
          draw_launcher();
     }
     else
     {
          if (s_show_screen)
               draw_game_output();

          if (gb->debug.enabled)
          {
               if (s_show_cpu)
               {
                    ImGui::SetNextWindowSize(ImVec2(430, 520), ImGuiCond_FirstUseEver);
                    ImGui::Begin("CPU / Controle", &s_show_cpu);
                    draw_panel_cpu_control(gb);
                    ImGui::End();
               }

               if (s_show_disasm)
               {
                    ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
                    ImGui::Begin("Disassembly", &s_show_disasm);
                    draw_panel_disasm(gb);
                    ImGui::End();
               }

               if (s_show_memory)
               {
                    ImGui::SetNextWindowSize(ImVec2(700, 320), ImGuiCond_FirstUseEver);
                    ImGui::Begin("Mem\xc3\xb3ria", &s_show_memory);
                    draw_panel_memory(gb);
                    ImGui::End();
               }

               if (s_show_gpu)
               {
                    ImGui::SetNextWindowSize(ImVec2(420, 380), ImGuiCond_FirstUseEver);
                    ImGui::Begin("GPU / PPU", &s_show_gpu);
                    draw_panel_gpu(gb);
                    ImGui::End();
               }

               if (s_show_oam)
               {
                    ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
                    ImGui::Begin("OAM (Sprites)", &s_show_oam);
                    draw_panel_oam(gb);
                    ImGui::End();
               }

               if (s_show_tiles)
               {
                    ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
                    ImGui::Begin("VRAM \xe2\x80\x93 Tiles", &s_show_tiles);
                    draw_panel_tile_viewer(gb);
                    ImGui::End();
               }

               if (s_show_tilemap)
               {
                    ImGui::SetNextWindowSize(ImVec2(420, 450), ImGuiCond_FirstUseEver);
                    ImGui::Begin("VRAM \xe2\x80\x93 Tilemap", &s_show_tilemap);
                    draw_panel_tilemap_viewer(gb);
                    ImGui::End();
               }

               if (s_show_profiler)
               {
                    ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
                    ImGui::Begin("Profiler", &s_show_profiler);
                    draw_panel_profiler(gb);
                    ImGui::End();
               }

               if (s_show_call_stack)
               {
                    ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
                    ImGui::Begin("Pilha de Chamadas", &s_show_call_stack);
                    draw_panel_call_stack(gb);
                    ImGui::End();
               }

               if (s_show_hw_viz)
               {
                    ImGui::SetNextWindowSize(ImVec2(740, 560), ImGuiCond_FirstUseEver);
                    ImGui::Begin("HW Diagram", &s_show_hw_viz);
                    draw_panel_hw_viz(gb);
                    ImGui::End();
               }

               if (s_show_cpu_viz)
               {
                    ImGui::SetNextWindowSize(ImVec2(680, 570), ImGuiCond_FirstUseEver);
                    ImGui::Begin("CPU Datapath", &s_show_cpu_viz);
                    draw_panel_cpu_viz(gb);
                    ImGui::End();
               }

               if (s_show_transistor_viz)
               {
                    ImGui::SetNextWindowSize(ImVec2(800, 640), ImGuiCond_FirstUseEver);
                    ImGui::Begin("Transistor Die - SM83", &s_show_transistor_viz);
                    draw_panel_transistor_viz(gb);
                    ImGui::End();
               }

               if (s_show_hw_schematic)
               {
                    ImGui::SetNextWindowSize(ImVec2(900, 660), ImGuiCond_FirstUseEver);
                    ImGui::Begin("HW Schematic - DMG", &s_show_hw_schematic);
                    draw_panel_hw_schematic(gb);
                    ImGui::End();
               }
          }
     }

     draw_status_bar(gb);

     int w = (int)vp->Size.x;
     int h = (int)vp->Size.y;
     ImGui::Render();
     glViewport(0, 0, w, h);
     glClearColor(s_bg_color[0], s_bg_color[1], s_bg_color[2], 1.0f);
     glClear(GL_COLOR_BUFFER_BIT);
     ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

     glBindTexture(GL_TEXTURE_2D, s_prev_game_tex);
     glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                     GB_LCD_WIDTH, GB_LCD_HEIGHT,
                     GL_BGRA, GL_UNSIGNED_BYTE, pixels);
     glBindTexture(GL_TEXTURE_2D, 0);
     s_prev_game_valid = gb->cart.rom != nullptr;

     SDL_GL_SwapWindow(s_window);
}

extern "C" float debug_ui_get_speed_multiplier(void)
{
     return s_fast_forward ? s_ff_speed_factor : 1.0f;
}

extern "C" void debug_ui_destroy(struct gb * /*gb*/)
{
     save_ui_config();

     if (!s_gl_context)
          return;

     SDL_GL_MakeCurrent(s_window, s_gl_context);

     GLuint textures[] = {s_game_tex, s_prev_game_tex};
     glDeleteTextures(2, textures);
     s_game_tex = s_prev_game_tex = 0;
     debug_ui_panels_shutdown();
     s_prev_game_valid = false;

     ImGui_ImplOpenGL3_Shutdown();
     ImGui_ImplSDL3_Shutdown();
     ImGui::DestroyContext();

     SDL_GL_DestroyContext(s_gl_context);
     /* s_window pertence ao sdl.c — não destruir aqui */
     s_gl_context = nullptr;
     s_window = nullptr;
}
