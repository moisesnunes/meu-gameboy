#include "gba_debug_ui.h"
#include "gba_debug_ui_config.h"
#include "gba_debug_ui_panels.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_opengl3.h"
#include "RobotoMedium.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <string>
#include <vector>

extern "C"
{
#include "gba/gba.h"
#include "gba/gba_disasm.h"
#include "gba/gba_memory.h"
#include "gba/gba_cart.h"
}

/* Estado interno */

static SDL_Window *s_window = nullptr;
static SDL_GLContext s_gl_context = nullptr;
static GLuint s_game_tex = 0;

/* Visibilidade dos painéis */
static bool s_show_screen = true;
static bool s_show_cpu = true;
static bool s_show_disasm = true;
static bool s_show_memory = true;
static bool s_show_gpu = false;
static bool s_show_oam = false;
static bool s_show_apu = false;
static bool s_show_profiler = false;
static bool s_show_status_bar = true;

/* Vídeo */
static bool s_vsync = true;
static bool s_bilinear = false;
static bool s_fullscreen = false;
static bool s_show_fps = false;
static bool s_scanlines = false;
static float s_scanlines_intensity = 0.5f;
static float s_bg_color[3] = {0.10f, 0.10f, 0.10f};

/* Escala */
static const int VIDEO_SCALE_FIT = 0;
static const int VIDEO_SCALE_INTEGER = -1;
static const int VIDEO_SCALE_FIT_WIDTH = -2;
static const int VIDEO_SCALE_FIT_HEIGHT = -3;
static int s_video_scale = VIDEO_SCALE_FIT;

/* Emulador */
static bool s_fast_forward = false;
static float s_ff_speed_factor = 2.0f;
static bool s_start_paused = false;
static int s_debug_font_size = 1;

/* FPS */
static uint64_t s_fps_last_ns = 0;
static int s_fps_frames = 0;
static float s_fps_current = 0.0f;

/* Popups / status */
static bool s_show_about = false;
static char s_status_msg[160] = "";
static uint64_t s_status_until_ms = 0;

/* File dialog */
static char s_pending_rom[4096] = "";
static bool s_dialog_active = false;

/* ROM recentes */
static std::vector<std::string> s_recent_roms;

static const char *UI_CONFIG_PATH = "meu-gba_ui.ini";

/* Cores (mesmo tema do GB) */
static ImVec4 color_green = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
static ImVec4 color_red = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
static ImVec4 color_yellow = ImVec4(0.9f, 0.9f, 0.1f, 1.0f);
static ImVec4 color_gray = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
static ImVec4 color_cyan = ImVec4(0.2f, 0.9f, 0.9f, 1.0f);
static ImVec4 color_orange = ImVec4(1.0f, 0.55f, 0.1f, 1.0f);

/* Helpers */

static void set_status(const char *fmt, ...)
{
     va_list ap;
     va_start(ap, fmt);
     vsnprintf(s_status_msg, sizeof(s_status_msg), fmt, ap);
     va_end(ap);
     s_status_until_ms = SDL_GetTicks() + 3000;
}

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
     default:
          return 1.00f;
     }
}

static void apply_font_scale(void)
{
     ImGui::GetIO().FontGlobalScale = debug_font_scale();
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

static ImVec4 ui_lerp(const ImVec4 &a, const ImVec4 &b, float t)
{
     return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                   a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

static void set_style(void)
{
     ImGuiStyle &s = ImGui::GetStyle();
     s.WindowRounding = 6.0f;
     s.WindowBorderSize = 1.0f;
     s.ChildRounding = 4.0f;
     s.PopupRounding = 6.0f;
     s.FramePadding = ImVec2(8.0f, 4.0f);
     s.FrameRounding = 4.0f;
     s.ItemSpacing = ImVec2(8.0f, 6.0f);
     s.ScrollbarSize = 12.0f;
     s.ScrollbarRounding = 4.0f;
     s.GrabRounding = 4.0f;
     s.TabRounding = 4.0f;
     s.WindowPadding = ImVec2(12.0f, 10.0f);
     s.WindowMinSize = ImVec2(64.0f, 48.0f);
     s.DisabledAlpha = 0.45f;

     const ImVec4 bg = ImVec4(0.122f, 0.125f, 0.133f, 1.0f);
     const ImVec4 chrome = ImVec4(0.098f, 0.101f, 0.109f, 1.0f);
     const ImVec4 panel = ImVec4(0.145f, 0.149f, 0.157f, 1.0f);
     const ImVec4 panel2 = ImVec4(0.180f, 0.185f, 0.196f, 1.0f);
     const ImVec4 panel3 = ImVec4(0.215f, 0.221f, 0.234f, 1.0f);
     const ImVec4 border = ImVec4(0.255f, 0.260f, 0.278f, 1.0f);
     const ImVec4 text = ImVec4(0.918f, 0.925f, 0.937f, 1.0f);
     const ImVec4 text_d = ImVec4(0.520f, 0.535f, 0.570f, 1.0f);
     const ImVec4 accent = ImVec4(0.239f, 0.525f, 0.878f, 1.0f);
     const ImVec4 accentL = ImVec4(0.380f, 0.635f, 0.940f, 1.0f);
     const ImVec4 accentD = ImVec4(0.239f, 0.525f, 0.878f, 0.35f);

     s.Colors[ImGuiCol_Text] = text;
     s.Colors[ImGuiCol_TextDisabled] = text_d;
     s.Colors[ImGuiCol_WindowBg] = bg;
     s.Colors[ImGuiCol_ChildBg] = ImVec4(0.110f, 0.112f, 0.120f, 1.0f);
     s.Colors[ImGuiCol_PopupBg] = panel;
     s.Colors[ImGuiCol_Border] = border;
     s.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
     s.Colors[ImGuiCol_FrameBg] = panel2;
     s.Colors[ImGuiCol_FrameBgHovered] = panel3;
     s.Colors[ImGuiCol_FrameBgActive] = ui_lerp(panel2, accent, 0.40f);
     s.Colors[ImGuiCol_TitleBg] = chrome;
     s.Colors[ImGuiCol_TitleBgActive] = chrome;
     s.Colors[ImGuiCol_TitleBgCollapsed] = chrome;
     s.Colors[ImGuiCol_MenuBarBg] = chrome;
     s.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
     s.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.31f, 0.32f, 0.34f, 1.0f);
     s.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.42f, 0.43f, 0.46f, 1.0f);
     s.Colors[ImGuiCol_ScrollbarGrabActive] = accent;
     s.Colors[ImGuiCol_CheckMark] = accentL;
     s.Colors[ImGuiCol_SliderGrab] = accent;
     s.Colors[ImGuiCol_SliderGrabActive] = accentL;
     s.Colors[ImGuiCol_Button] = panel2;
     s.Colors[ImGuiCol_ButtonHovered] = ui_lerp(panel2, accent, 0.40f);
     s.Colors[ImGuiCol_ButtonActive] = accent;
     s.Colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
     s.Colors[ImGuiCol_HeaderHovered] = panel3;
     s.Colors[ImGuiCol_HeaderActive] = ui_lerp(panel2, accent, 0.50f);
     s.Colors[ImGuiCol_Separator] = border;
     s.Colors[ImGuiCol_SeparatorHovered] = ui_lerp(border, accent, 0.60f);
     s.Colors[ImGuiCol_SeparatorActive] = accent;
     s.Colors[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
     s.Colors[ImGuiCol_ResizeGripHovered] = accentD;
     s.Colors[ImGuiCol_ResizeGripActive] = accent;
     s.Colors[ImGuiCol_Tab] = chrome;
     s.Colors[ImGuiCol_TabHovered] = ui_lerp(chrome, accent, 0.30f);
     s.Colors[ImGuiCol_TabSelected] = bg;
     s.Colors[ImGuiCol_TabSelectedOverline] = accent;
     s.Colors[ImGuiCol_TabDimmed] = chrome;
     s.Colors[ImGuiCol_TabDimmedSelected] = ui_lerp(chrome, bg, 0.50f);
     s.Colors[ImGuiCol_PlotLines] = accent;
     s.Colors[ImGuiCol_PlotHistogram] = accent;
     s.Colors[ImGuiCol_TableHeaderBg] = panel;
     s.Colors[ImGuiCol_TableBorderStrong] = border;
     s.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
     s.Colors[ImGuiCol_TextSelectedBg] = accentD;
     s.Colors[ImGuiCol_NavCursor] = accent;
     s.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.04f, 0.04f, 0.05f, 0.70f);
}

/* Config I/O */

static void apply_config(const gba_debug_ui_config &cfg)
{
     s_show_screen = cfg.show_screen;
     s_show_cpu = cfg.show_cpu;
     s_show_disasm = cfg.show_disasm;
     s_show_memory = cfg.show_memory;
     s_show_gpu = cfg.show_gpu;
     s_show_oam = cfg.show_oam;
     s_show_apu = cfg.show_apu;
     s_show_profiler = cfg.show_profiler;
     s_show_status_bar = cfg.show_status_bar;
     s_vsync = cfg.vsync;
     s_bilinear = cfg.bilinear;
     s_show_fps = cfg.show_fps;
     s_video_scale = cfg.video_scale;
     s_scanlines = cfg.scanlines;
     s_scanlines_intensity = cfg.scanlines_intensity;
     s_bg_color[0] = cfg.background_color[0];
     s_bg_color[1] = cfg.background_color[1];
     s_bg_color[2] = cfg.background_color[2];
     s_ff_speed_factor = cfg.fast_forward_speed;
     s_debug_font_size = cfg.debug_font_size;
     s_start_paused = cfg.start_paused;
     s_recent_roms = cfg.recent_roms;
}

static void capture_config(gba_debug_ui_config *cfg)
{
     gba_debug_ui_config_defaults(cfg);
     cfg->show_screen = s_show_screen;
     cfg->show_cpu = s_show_cpu;
     cfg->show_disasm = s_show_disasm;
     cfg->show_memory = s_show_memory;
     cfg->show_gpu = s_show_gpu;
     cfg->show_oam = s_show_oam;
     cfg->show_apu = s_show_apu;
     cfg->show_profiler = s_show_profiler;
     cfg->show_status_bar = s_show_status_bar;
     cfg->vsync = s_vsync;
     cfg->bilinear = s_bilinear;
     cfg->show_fps = s_show_fps;
     cfg->video_scale = s_video_scale;
     cfg->scanlines = s_scanlines;
     cfg->scanlines_intensity = s_scanlines_intensity;
     cfg->background_color[0] = s_bg_color[0];
     cfg->background_color[1] = s_bg_color[1];
     cfg->background_color[2] = s_bg_color[2];
     cfg->fast_forward_speed = s_ff_speed_factor;
     cfg->debug_font_size = s_debug_font_size;
     cfg->start_paused = s_start_paused;
     cfg->recent_roms = s_recent_roms;
}

static void load_config(void)
{
     gba_debug_ui_config cfg;
     gba_debug_ui_config_load(&cfg, UI_CONFIG_PATH);
     apply_config(cfg);
}

static void save_config(void)
{
     gba_debug_ui_config cfg;
     capture_config(&cfg);
     gba_debug_ui_config_save(&cfg, UI_CONFIG_PATH);
}

/* File dialog */

static void SDLCALL file_dialog_cb(void *, const char *const *list, int)
{
     s_dialog_active = false;
     if (!list || !list[0])
          return;
     SDL_strlcpy(s_pending_rom, list[0], sizeof(s_pending_rom));
}

static void open_rom_dialog(void)
{
     if (s_dialog_active)
          return;
     s_dialog_active = true;
     SDL_DialogFileFilter filters[] = {{"GBA ROM", "gba;bin"}};
     SDL_ShowOpenFileDialog(file_dialog_cb, nullptr, s_window, filters, 1, nullptr, false);
}

static void push_recent(const char *path)
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

/* Nome do estado do debug */

static const char *debug_state_name(struct gba *gba)
{
     if (!gba->cart.rom)
          return "Idle";
     if (!gba->debug.enabled)
          return "Rodando";
     switch (gba->debug.state)
     {
     case GBA_DEBUG_PAUSED:
          return "Pausado";
     case GBA_DEBUG_STEPPING:
          return "Step";
     case GBA_DEBUG_RUNNING:
          return "Rodando";
     default:
          return "Debug";
     }
}

/* Status bar */

static void draw_status_bar(struct gba *gba)
{
     if (!s_show_status_bar)
          return;

     ImGuiViewport *vp = ImGui::GetMainViewport();
     const float h = 25.0f;
     ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - h), ImGuiCond_Always);
     ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h), ImGuiCond_Always);
     ImGui::SetNextWindowBgAlpha(1.0f);

     ImGuiWindowFlags flags =
         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
         ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoSavedSettings |
         ImGuiWindowFlags_NoBringToFrontOnFocus;

     if (!ImGui::Begin("##gba_status_bar", nullptr, flags))
     {
          ImGui::End();
          return;
     }

     /* Título da ROM */
     if (gba->cart.rom)
     {
          char title[17] = {};
          memcpy(title, gba->cart.rom + 0xA0, 12);
          ImGui::TextColored(color_green, "%s", title[0] ? title : "GBA ROM");
     }
     else
     {
          ImGui::TextColored(color_gray, "meu-GBA");
     }
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "|");
     ImGui::SameLine();
     ImGui::Text("%s", debug_state_name(gba));

     if (gba->cart.rom)
     {
          ImGui::SameLine();
          ImGui::TextColored(color_gray, "|");
          ImGui::SameLine();
          ImGui::Text("LY %u", gba->gpu.vcount);
          ImGui::SameLine();
          ImGui::TextColored(color_gray, "|");
          ImGui::SameLine();
          ImGui::Text("Mode %u", gba->gpu.bg_mode);
          ImGui::SameLine();
          ImGui::TextColored(color_gray, "|");
          ImGui::SameLine();
          ImGui::Text("PC %08x", gba_cpu_current_pc(&gba->cpu));
     }

     if (s_fast_forward)
     {
          ImGui::SameLine();
          ImGui::TextColored(color_yellow, "| FF %.1fx", s_ff_speed_factor);
     }

     if (s_status_msg[0] && SDL_GetTicks() < s_status_until_ms)
     {
          ImGui::SameLine();
          ImGui::TextColored(color_gray, "|");
          ImGui::SameLine();
          ImGui::TextColored(color_cyan, "%s", s_status_msg);
     }

     char right[48];
     snprintf(right, sizeof(right), "%.0f FPS", s_fps_current);
     float rw = ImGui::CalcTextSize(right).x;
     float avail = ImGui::GetContentRegionAvail().x;
     if (avail > rw)
          ImGui::SameLine(ImGui::GetCursorPosX() + avail - rw);
     ImGui::TextColored(s_fps_current >= 55.0f ? color_green : color_yellow, "%s", right);

     ImGui::End();
}

/* Launcher (sem ROM) */

static void draw_launcher(void)
{
     ImGuiIO &io = ImGui::GetIO();
     ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                             ImGuiCond_Always, ImVec2(0.5f, 0.5f));
     ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_Always);
     ImGui::Begin("##gba_launcher", nullptr,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

     float tw = ImGui::CalcTextSize("meu-GBA").x;
     ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - tw) * 0.5f);
     ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f), "meu-GBA");
     ImGui::Spacing();
     ImGui::Separator();
     ImGui::Spacing();

     float bw = ImGui::GetContentRegionAvail().x;
     if (ImGui::Button("Abrir ROM...", ImVec2(bw, 36)))
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
                        "Arraste uma ROM ou use Ctrl+O");
     ImGui::End();
}

/* Janela de saída do jogo */

static void draw_game_output(void)
{
     ImGui::SetNextWindowSize(ImVec2(500, 380), ImGuiCond_FirstUseEver);
     ImGui::Begin("Sa"
                  "\xc3\xad"
                  "da##gba_screen",
                  &s_show_screen,
                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

     float avail_w = ImGui::GetContentRegionAvail().x;
     float avail_h = ImGui::GetContentRegionAvail().y;

     float scale = 1.0f;
     float fit_w = avail_w / (float)GBA_LCD_W;
     float fit_h = avail_h / (float)GBA_LCD_H;
     float fit = fit_w < fit_h ? fit_w : fit_h;

     if (s_video_scale == VIDEO_SCALE_FIT)
          scale = fit < 1.0f ? 1.0f : fit;
     else if (s_video_scale == VIDEO_SCALE_INTEGER)
          scale = floorf(fit) < 1.0f ? 1.0f : floorf(fit);
     else if (s_video_scale == VIDEO_SCALE_FIT_WIDTH)
          scale = fit_w < 1.0f ? 1.0f : fit_w;
     else if (s_video_scale == VIDEO_SCALE_FIT_HEIGHT)
          scale = fit_h < 1.0f ? 1.0f : fit_h;
     else
          scale = (float)s_video_scale;

     ImVec2 img((float)GBA_LCD_W * scale, (float)GBA_LCD_H * scale);
     float off_x = (avail_w - img.x) * 0.5f + ImGui::GetCursorPosX();
     float off_y = (avail_h - img.y) * 0.5f + ImGui::GetCursorPosY();
     if (off_x < ImGui::GetCursorPosX())
          off_x = ImGui::GetCursorPosX();
     if (off_y < ImGui::GetCursorPosY())
          off_y = ImGui::GetCursorPosY();
     ImGui::SetCursorPos(ImVec2(off_x, off_y));

     ImVec2 img_screen = ImGui::GetCursorScreenPos();
     ImGui::Image((ImTextureID)(intptr_t)s_game_tex, img);

     /* Overlay Fast Forward */
     if (s_fast_forward)
     {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          char ff[16];
          snprintf(ff, sizeof(ff), ">> %.0fx", s_ff_speed_factor);
          ImVec2 ts = ImGui::CalcTextSize(ff);
          float tx = img_screen.x + img.x - ts.x - 6.0f;
          float ty = img_screen.y + 4.0f;
          dl->AddRectFilled(ImVec2(tx - 2, ty - 1), ImVec2(tx + ts.x + 2, ty + ts.y + 1),
                            IM_COL32(0, 0, 0, 160), 3.0f);
          dl->AddText(ImVec2(tx, ty), IM_COL32(255, 220, 50, 255), ff);
     }

     /* Scanlines */
     if (s_scanlines)
     {
          ImDrawList *dl = ImGui::GetWindowDrawList();
          ImU32 sc = IM_COL32(0, 0, 0, (int)(s_scanlines_intensity * 210.0f));
          float step = scale * 2.0f < 2.0f ? 2.0f : scale * 2.0f;
          for (float y = img_screen.y; y < img_screen.y + img.y; y += step)
               dl->AddRectFilled(ImVec2(img_screen.x, y),
                                 ImVec2(img_screen.x + img.x, y + scale), sc);
     }

     ImGui::End();
}

/* Menu principal */

static void draw_main_menu(struct gba *gba)
{
     if (!ImGui::BeginMainMenuBar())
          return;

     bool has_rom = gba->cart.rom != nullptr;
     bool paused = gba->debug.enabled && gba->debug.state == GBA_DEBUG_PAUSED;
     bool dbg = gba->debug.enabled;

     /* Arquivo */
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
                    set_status("ROMs recentes limpas");
               }
               ImGui::EndMenu();
          }

          ImGui::Separator();
          if (ImGui::MenuItem("Recarregar ROM", "Ctrl+R", false, has_rom && !s_recent_roms.empty()))
               SDL_strlcpy(s_pending_rom, s_recent_roms[0].c_str(), sizeof(s_pending_rom));

          ImGui::Separator();
          if (ImGui::MenuItem("Salvar Configura\xc3\xa7\xc3\xa3o"))
          {
               save_config();
               set_status("Configura\xc3\xa7\xc3\xa3o salva em %s", UI_CONFIG_PATH);
          }
          if (ImGui::MenuItem("Recarregar Configura\xc3\xa7\xc3\xa3o"))
          {
               load_config();
               apply_font_scale();
               SDL_GL_SetSwapInterval(s_vsync ? 1 : 0);
               apply_texture_filter();
               set_status("Configura\xc3\xa7\xc3\xa3o recarregada");
          }

          ImGui::Separator();
          if (ImGui::MenuItem("Sair", "Alt+F4"))
               gba->quit = true;

          ImGui::EndMenu();
     }

     /* Emulador */
     if (ImGui::BeginMenu("Emulador"))
     {
          if (ImGui::MenuItem("Pausar", "F5", paused, has_rom))
          {
               if (paused)
                    gba->debug.state = GBA_DEBUG_RUNNING;
               else
               {
                    gba->debug.enabled = true;
                    gba->debug.state = GBA_DEBUG_PAUSED;
               }
          }
          if (ImGui::MenuItem("Resetar", nullptr, false, has_rom))
          {
               gba_reset(gba);
               set_status("GBA resetado");
          }

          ImGui::Separator();
          if (ImGui::MenuItem("Avan\xc3\xa7o R\xc3\xa1pido", "Tab", s_fast_forward, has_rom))
               s_fast_forward = !s_fast_forward;
          if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Mantenha Tab pressionado para ativar temporariamente.");

          if (ImGui::BeginMenu("Velocidade do FF", has_rom))
          {
               ImGui::SetNextItemWidth(160.0f);
               ImGui::SliderFloat("##ff", &s_ff_speed_factor, 1.5f, 8.0f, "%.1fx",
                                  ImGuiSliderFlags_AlwaysClamp);
               ImGui::Separator();
               if (ImGui::MenuItem("2x", nullptr, s_ff_speed_factor == 2.0f))
                    s_ff_speed_factor = 2.0f;
               if (ImGui::MenuItem("4x", nullptr, s_ff_speed_factor == 4.0f))
                    s_ff_speed_factor = 4.0f;
               if (ImGui::MenuItem("8x", nullptr, s_ff_speed_factor == 8.0f))
                    s_ff_speed_factor = 8.0f;
               ImGui::EndMenu();
          }

          ImGui::Separator();
          ImGui::MenuItem("Iniciar Pausado", nullptr, &s_start_paused);

          ImGui::EndMenu();
     }

     /* Vídeo */
     if (ImGui::BeginMenu("V"
                          "\xc3\xad"
                          "deo"))
     {
          if (ImGui::MenuItem("Tela Cheia", "F11", s_fullscreen))
          {
               s_fullscreen = !s_fullscreen;
               SDL_SetWindowFullscreen(s_window, s_fullscreen);
          }

          ImGui::Separator();
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
               for (int sc = 1; sc <= 6; ++sc)
               {
                    char lbl[48];
                    snprintf(lbl, sizeof(lbl), "%d\xc3\x97  (%d \xc3\x97 %d)", sc, GBA_LCD_W * sc, GBA_LCD_H * sc);
                    if (ImGui::MenuItem(lbl, nullptr, s_video_scale == sc))
                         s_video_scale = sc;
               }
               ImGui::EndMenu();
          }

          ImGui::Separator();
          if (ImGui::MenuItem("VSync", nullptr, s_vsync))
          {
               s_vsync = !s_vsync;
               SDL_GL_SetSwapInterval(s_vsync ? 1 : 0);
          }
          if (ImGui::MenuItem("Mostrar FPS", nullptr, s_show_fps))
               s_show_fps = !s_show_fps;
          ImGui::MenuItem("Barra de Status", nullptr, &s_show_status_bar);

          ImGui::Separator();
          if (ImGui::MenuItem("Filtro Bilinear", nullptr, s_bilinear))
          {
               s_bilinear = !s_bilinear;
               apply_texture_filter();
          }

          if (ImGui::BeginMenu("Scanlines"))
          {
               ImGui::MenuItem("Ativar Scanlines", nullptr, &s_scanlines);
               ImGui::SetNextItemWidth(200.0f);
               ImGui::SliderFloat("Intensidade##sl", &s_scanlines_intensity,
                                  0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
               ImGui::EndMenu();
          }

          ImGui::Separator();
          if (ImGui::BeginMenu("Cor de Fundo"))
          {
               ImGui::ColorEdit3("##bg", s_bg_color,
                                 ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float);
               ImGui::SameLine();
               ImGui::Text("Cor de fundo");
               ImGui::Separator();
               if (ImGui::MenuItem("Preto"))
               {
                    s_bg_color[0] = 0.0f;
                    s_bg_color[1] = 0.0f;
                    s_bg_color[2] = 0.0f;
               }
               if (ImGui::MenuItem("Cinza (padr\xc3\xa3o)"))
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
               ImGui::EndMenu();
          }

          ImGui::EndMenu();
     }

     /* Debug */
     if (ImGui::BeginMenu("Debug"))
     {
          if (ImGui::MenuItem("Ativar Debug", nullptr, &gba->debug.enabled))
          {
               if (gba->debug.enabled)
                    gba->debug.state = GBA_DEBUG_PAUSED;
               else
                    gba->debug.state = GBA_DEBUG_RUNNING;
          }

          ImGui::Separator();
          ImGui::MenuItem("Sa"
                          "\xc3\xad"
                          "da do Jogo",
                          nullptr, &s_show_screen);

          ImGui::Separator();
          ImGui::MenuItem("CPU / Controle", nullptr, &s_show_cpu, dbg);
          ImGui::MenuItem("Disassembly", nullptr, &s_show_disasm, dbg);
          ImGui::MenuItem("Mem\xc3\xb3ria", nullptr, &s_show_memory, dbg);

          ImGui::Separator();
          if (ImGui::BeginMenu("V"
                               "\xc3\xad"
                               "deo",
                               dbg))
          {
               ImGui::MenuItem("GPU / PPU", nullptr, &s_show_gpu);
               ImGui::MenuItem("OAM (Sprites)", nullptr, &s_show_oam);
               ImGui::EndMenu();
          }
          ImGui::MenuItem("APU", nullptr, &s_show_apu, dbg);
          ImGui::MenuItem("Profiler", nullptr, &s_show_profiler, dbg);

          ImGui::Separator();
          if (ImGui::BeginMenu("Tamanho de Fonte"))
          {
               if (ImGui::MenuItem("Pequena", nullptr, s_debug_font_size == 0))
               {
                    s_debug_font_size = 0;
                    apply_font_scale();
               }
               if (ImGui::MenuItem("Normal", nullptr, s_debug_font_size == 1))
               {
                    s_debug_font_size = 1;
                    apply_font_scale();
               }
               if (ImGui::MenuItem("Grande", nullptr, s_debug_font_size == 2))
               {
                    s_debug_font_size = 2;
                    apply_font_scale();
               }
               if (ImGui::MenuItem("Enorme", nullptr, s_debug_font_size == 3))
               {
                    s_debug_font_size = 3;
                    apply_font_scale();
               }
               ImGui::EndMenu();
          }

          ImGui::EndMenu();
     }

     /* Sobre */
     if (ImGui::BeginMenu("Sobre"))
     {
          if (ImGui::MenuItem("Sobre meu-GBA..."))
               s_show_about = true;
          ImGui::EndMenu();
     }

     /* FPS lado direito */
     if (s_show_fps)
     {
          char fps[32];
          snprintf(fps, sizeof(fps), "%.0f FPS", s_fps_current);
          float w = ImGui::GetContentRegionAvail().x;
          float tw = ImGui::CalcTextSize(fps).x + 8.0f;
          if (w > tw)
               ImGui::SameLine(ImGui::GetCursorPosX() + w - tw);
          ImGui::TextColored(s_fps_current >= 55.0f ? color_green : color_yellow, "%s", fps);
     }

     ImGui::EndMainMenuBar();

     /* Popup Sobre */
     if (s_show_about)
     {
          ImGui::OpenPopup("Sobre##gba_modal");
          s_show_about = false;
     }
     if (ImGui::BeginPopupModal("Sobre##gba_modal", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
     {
          ImGui::Text("meu-GBA");
          ImGui::TextColored(color_gray, "Emulador de Game Boy Advance");
          ImGui::Separator();
          ImGui::Text("Frontend:  SDL3 + Dear ImGui");
          ImGui::Text("Renderer:  OpenGL 3.3 Core");
          ImGui::Separator();
          ImGui::TextDisabled("Atalhos:");
          ImGui::BulletText("Ctrl+O   Abrir ROM");
          ImGui::BulletText("F5       Pausar / Continuar");
          ImGui::BulletText("F10      Step");
          ImGui::BulletText("F11      Tela Cheia");
          ImGui::BulletText("Tab      Avan\xc3\xa7o R\xc3\xa1pido");
          ImGui::Separator();
          if (ImGui::Button("Fechar", ImVec2(120, 0)))
               ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
     }
}

/* API pública */

bool gba_debug_ui_init(struct gba *gba, SDL_Window *window)
{
     s_window = window;
     load_config();

     s_gl_context = SDL_GL_CreateContext(window);
     if (!s_gl_context)
     {
          fprintf(stderr, "gba_debug_ui: SDL_GL_CreateContext: %s\n", SDL_GetError());
          s_window = nullptr;
          return false;
     }

     SDL_GL_MakeCurrent(window, s_gl_context);
     SDL_GL_SetSwapInterval(s_vsync ? 1 : 0);

     IMGUI_CHECKVERSION();
     ImGui::CreateContext();

     ImGuiIO &io = ImGui::GetIO();
     io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
     io.IniFilename = "imgui_gba.ini";

     ImFont *roboto = io.Fonts->AddFontFromMemoryCompressedTTF(
         RobotoMedium_compressed_data, RobotoMedium_compressed_size, 16.0f,
         nullptr, io.Fonts->GetGlyphRangesCyrillic());
     if (roboto)
          io.FontDefault = roboto;
     apply_font_scale();

     ImGui::StyleColorsDark();
     set_style();

     ImGui_ImplSDL3_InitForOpenGL(window, s_gl_context);
     ImGui_ImplOpenGL3_Init("#version 330 core");

     glGenTextures(1, &s_game_tex);
     glBindTexture(GL_TEXTURE_2D, s_game_tex);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
     glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GBA_LCD_W, GBA_LCD_H, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
     glBindTexture(GL_TEXTURE_2D, 0);
     apply_texture_filter();

     gba_debug_ui_panels_init();

     gba->debug.enabled = true;
     if (s_start_paused && gba->debug.state == GBA_DEBUG_RUNNING)
          gba->debug.state = GBA_DEBUG_PAUSED;

     return true;
}

void gba_debug_ui_process_event(struct gba *gba, SDL_Event *event)
{
     if (!s_window)
          return;
     ImGui_ImplSDL3_ProcessEvent(event);

     if (event->type == SDL_EVENT_DROP_FILE)
     {
          SDL_strlcpy(s_pending_rom, event->drop.data, sizeof(s_pending_rom));
          SDL_free((void *)event->drop.data);
     }

     if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat)
     {
          SDL_Keycode key = event->key.key;

          if (key == SDLK_F5)
          {
               if (gba->debug.state == GBA_DEBUG_RUNNING)
                    gba->debug.state = GBA_DEBUG_PAUSED;
               else
                    gba->debug.state = GBA_DEBUG_RUNNING;
          }
          if (key == SDLK_F10 && gba->debug.state == GBA_DEBUG_PAUSED)
               gba->debug.state = GBA_DEBUG_STEPPING;
          if (key == SDLK_F11)
          {
               s_fullscreen = !s_fullscreen;
               SDL_SetWindowFullscreen(s_window, s_fullscreen);
          }
          if (key == SDLK_TAB)
               s_fast_forward = !s_fast_forward;
          if (key == SDLK_O && (SDL_GetModState() & SDL_KMOD_CTRL))
               open_rom_dialog();
          if (key == SDLK_R && (SDL_GetModState() & SDL_KMOD_CTRL) && !s_recent_roms.empty())
               SDL_strlcpy(s_pending_rom, s_recent_roms[0].c_str(), sizeof(s_pending_rom));
     }
     if (event->type == SDL_EVENT_KEY_UP && event->key.key == SDLK_TAB)
          s_fast_forward = false;
}

void gba_debug_ui_render(struct gba *gba, const uint32_t *pixels)
{
     if (!s_window || !s_gl_context)
          return;

     /* FPS */
     {
          uint64_t now = SDL_GetTicksNS();
          s_fps_frames++;
          if (!s_fps_last_ns)
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
     glBindTexture(GL_TEXTURE_2D, s_game_tex);
     glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GBA_LCD_W, GBA_LCD_H,
                     GL_BGRA, GL_UNSIGNED_BYTE, pixels);
     glBindTexture(GL_TEXTURE_2D, 0);

     ImGui_ImplOpenGL3_NewFrame();
     ImGui_ImplSDL3_NewFrame();
     ImGui::NewFrame();

     draw_main_menu(gba);

     if (!gba->cart.rom)
     {
          draw_launcher();
     }
     else
     {
          if (s_show_screen)
               draw_game_output();

          if (gba->debug.enabled)
          {
               if (s_show_cpu)
               {
                    ImGui::SetNextWindowSize(ImVec2(500, 460), ImGuiCond_FirstUseEver);
                    ImGui::Begin("GBA CPU / Controle", &s_show_cpu);
                    gba_draw_panel_cpu_control(gba);
                    ImGui::End();
               }
               if (s_show_disasm)
               {
                    ImGui::SetNextWindowSize(ImVec2(520, 460), ImGuiCond_FirstUseEver);
                    ImGui::Begin("GBA Disassembly", &s_show_disasm);
                    gba_draw_panel_disasm(gba);
                    ImGui::End();
               }
               if (s_show_memory)
               {
                    ImGui::SetNextWindowSize(ImVec2(700, 360), ImGuiCond_FirstUseEver);
                    ImGui::Begin("GBA Mem\xc3\xb3ria", &s_show_memory);
                    gba_draw_panel_memory(gba);
                    ImGui::End();
               }
               if (s_show_gpu)
               {
                    ImGui::SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
                    ImGui::Begin("GBA GPU / PPU", &s_show_gpu);
                    gba_draw_panel_gpu(gba);
                    ImGui::End();
               }
               if (s_show_oam)
               {
                    ImGui::SetNextWindowSize(ImVec2(480, 340), ImGuiCond_FirstUseEver);
                    ImGui::Begin("GBA OAM (Sprites)", &s_show_oam);
                    gba_draw_panel_oam(gba);
                    ImGui::End();
               }
               if (s_show_apu)
               {
                    ImGui::SetNextWindowSize(ImVec2(460, 360), ImGuiCond_FirstUseEver);
                    ImGui::Begin("GBA APU", &s_show_apu);
                    gba_draw_panel_apu(gba);
                    ImGui::End();
               }
               if (s_show_profiler)
               {
                    ImGui::SetNextWindowSize(ImVec2(420, 300), ImGuiCond_FirstUseEver);
                    ImGui::Begin("GBA Profiler", &s_show_profiler);
                    gba_draw_panel_profiler(gba);
                    ImGui::End();
               }
          }
     }

     draw_status_bar(gba);

     ImGuiViewport *vp = ImGui::GetMainViewport();
     ImGui::Render();
     glViewport(0, 0, (int)vp->Size.x, (int)vp->Size.y);
     glClearColor(s_bg_color[0], s_bg_color[1], s_bg_color[2], 1.0f);
     glClear(GL_COLOR_BUFFER_BIT);
     ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
     SDL_GL_SwapWindow(s_window);
}

/* Chamado pelo gba_main quando uma ROM é carregada/trocada via UI */
const char *gba_debug_ui_pending_rom(void)
{
     return s_pending_rom[0] ? s_pending_rom : nullptr;
}

void gba_debug_ui_clear_pending_rom(const char *loaded_path)
{
     push_recent(loaded_path);
     s_pending_rom[0] = '\0';
}

void gba_debug_ui_destroy(void)
{
     if (!s_window)
          return;
     save_config();

     SDL_GL_MakeCurrent(s_window, s_gl_context);
     if (s_game_tex)
     {
          glDeleteTextures(1, &s_game_tex);
          s_game_tex = 0;
     }
     gba_debug_ui_panels_shutdown();

     ImGui_ImplOpenGL3_Shutdown();
     ImGui_ImplSDL3_Shutdown();
     ImGui::DestroyContext();
     SDL_GL_DestroyContext(s_gl_context);
     s_gl_context = nullptr;
     s_window = nullptr;
}

float gba_debug_ui_speed_multiplier(void)
{
     return s_fast_forward ? s_ff_speed_factor : 1.0f;
}
