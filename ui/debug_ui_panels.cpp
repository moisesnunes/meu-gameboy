#include "debug_ui_panels.h"

#include "imgui.h"
#include <SDL3/SDL_opengl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C"
{
#include "cart.h"
#include "debug.h"
#include "disasm.h"
#include "gb.h"
#include "memory.h"
#include "sm83_die_view.h"
#include "sm83_netlist_data.h"
#include "sm83_netlist_sim.h"
#include "sm83_signal_overlay.h"
#include "hw_schematic_data.h"
#include "hw_schematic_view.h"
}

#include "debug_ui_actions.h"

/* ── Estado compartilhado com debug_ui.cpp ── */
int g_mem_addr = 0xC000;
int g_mem_mode = 0;
bool g_show_rom_info = false;

/* ── Estado local dos painéis ── */
static char s_bp_input[8] = "0100";
static char s_wp_input[8] = "0000";
static int s_wp_type = GB_WATCHPOINT_BOTH;
static bool s_show_win_map = false;

/* ── Cores ── */
static ImVec4 color_green = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
static ImVec4 color_red = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
static ImVec4 color_yellow = ImVec4(0.9f, 0.9f, 0.1f, 1.0f);
static ImVec4 color_gray = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
static ImVec4 color_cyan = ImVec4(0.2f, 0.9f, 0.9f, 1.0f);
static ImVec4 color_orange = ImVec4(1.0f, 0.55f, 0.1f, 1.0f);
static ImVec4 color_white = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
static ImVec4 color_skyblue = ImVec4(0.5f, 0.8f, 1.0f, 1.0f);

/* ────────────────────────────────────────────────────────── */
/* VRAM Visualization — texturas OpenGL                        */
/* ────────────────────────────────────────────────────────── */

static const int TV_COLS = 24;
static const int TV_ROWS = 16;
static const int TV_W = TV_COLS * 8;
static const int TV_H = TV_ROWS * 8;

static const int TM_W = 256;
static const int TM_H = 256;

static GLuint s_tile_tex = 0;
static GLuint s_tilemap_tex = 0;
static uint32_t s_tile_buf[TV_W * TV_H];
static uint32_t s_tilemap_buf[TM_W * TM_H];

static const int OAM_ATLAS_W = 320;
static const int OAM_ATLAS_H = 16;
static GLuint s_oam_tex = 0;
static uint32_t s_oam_buf[OAM_ATLAS_W * OAM_ATLAS_H];

static const int EXEC_MAP_W = 256;
static const int EXEC_MAP_H = 256;
static GLuint s_heatmap_tex = 0;
static GLuint s_coverage_tex = 0;
static uint32_t s_heatmap_buf[EXEC_MAP_W * EXEC_MAP_H];
static uint32_t s_coverage_buf[EXEC_MAP_W * EXEC_MAP_H];

static const uint32_t k_dmg_shades[4] = {
    0xFFFFFFFF,
    0xFFAAAAAA,
    0xFF555555,
    0xFF000000,
};

static uint32_t dmg_apply(uint8_t palette, int raw)
{
     int shade = (palette >> (raw * 2)) & 3;
     return k_dmg_shades[shade];
}

static uint32_t gbc_to_rgba(uint16_t c)
{
     uint8_t r = (c >> 0) & 0x1f;
     uint8_t g = (c >> 5) & 0x1f;
     uint8_t b = (c >> 10) & 0x1f;
     r = (r << 3) | (r >> 2);
     g = (g << 3) | (g >> 2);
     b = (b << 3) | (b >> 2);
     return 0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
}

static int vram_tile_raw(const uint8_t *vram, unsigned tile_offset, int x, int y)
{
     int bit = 7 - x;
     int lsb = (vram[tile_offset + y * 2 + 0] >> bit) & 1;
     int msb = (vram[tile_offset + y * 2 + 1] >> bit) & 1;
     return (msb << 1) | lsb;
}

static void make_tex(GLuint *tex, int w, int h)
{
     glGenTextures(1, tex);
     glBindTexture(GL_TEXTURE_2D, *tex);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
     glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
     glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                  GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
     glBindTexture(GL_TEXTURE_2D, 0);
}

void debug_ui_panels_init(void)
{
     make_tex(&s_tile_tex, TV_W, TV_H);
     make_tex(&s_tilemap_tex, TM_W, TM_H);
     make_tex(&s_oam_tex, OAM_ATLAS_W, OAM_ATLAS_H);
     make_tex(&s_heatmap_tex, EXEC_MAP_W, EXEC_MAP_H);
     make_tex(&s_coverage_tex, EXEC_MAP_W, EXEC_MAP_H);
}

void debug_ui_panels_shutdown(void)
{
     glDeleteTextures(1, &s_tile_tex);
     glDeleteTextures(1, &s_tilemap_tex);
     glDeleteTextures(1, &s_oam_tex);
     glDeleteTextures(1, &s_heatmap_tex);
     glDeleteTextures(1, &s_coverage_tex);
     s_tile_tex = s_tilemap_tex = s_oam_tex = s_heatmap_tex = s_coverage_tex = 0;
}

/* ────────────────────────────────────────────────────────── */
/* Painel: VRAM – Tile Viewer                                  */
/* ────────────────────────────────────────────────────────── */

static void update_tile_texture(struct gb *gb)
{
     const uint8_t *vram = gb->vram;
     for (int tile = 0; tile < TV_COLS * TV_ROWS; tile++)
     {
          int tc = tile % TV_COLS;
          int tr = tile / TV_COLS;
          unsigned off = (unsigned)tile * 16u;
          for (int y = 0; y < 8; y++)
               for (int x = 0; x < 8; x++)
               {
                    int raw = vram_tile_raw(vram, off, x, y);
                    s_tile_buf[(tr * 8 + y) * TV_W + (tc * 8 + x)] = k_dmg_shades[raw];
               }
     }
     glBindTexture(GL_TEXTURE_2D, s_tile_tex);
     glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TV_W, TV_H,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_tile_buf);
     glBindTexture(GL_TEXTURE_2D, 0);
}

void draw_panel_tile_viewer(struct gb *gb)
{
     update_tile_texture(gb);

     ImGui::TextColored(color_gray,
                        "VRAM 0x8000-0x97FF  \xc2\xb7  %d tiles  \xc2\xb7  %d colunas \xc3\x97 %d linhas",
                        TV_COLS * TV_ROWS, TV_COLS, TV_ROWS);
     ImGui::Separator();

     const float scale = 2.0f;
     ImVec2 img_size(TV_W * scale, TV_H * scale);
     ImVec2 cursor = ImGui::GetCursorScreenPos();

     ImGui::Image((ImTextureID)(intptr_t)s_tile_tex, img_size);

     ImDrawList *dl = ImGui::GetWindowDrawList();
     ImU32 grid_col = IM_COL32(80, 80, 80, 160);
     for (int i = 0; i <= TV_COLS; i++)
     {
          float x = cursor.x + i * 8 * scale;
          dl->AddLine(ImVec2(x, cursor.y), ImVec2(x, cursor.y + TV_H * scale), grid_col);
     }
     for (int j = 0; j <= TV_ROWS; j++)
     {
          float y = cursor.y + j * 8 * scale;
          dl->AddLine(ImVec2(cursor.x, y), ImVec2(cursor.x + TV_W * scale, y), grid_col);
     }

     if (ImGui::IsItemHovered())
     {
          ImVec2 mouse = ImGui::GetMousePos();
          int tx = (int)((mouse.x - cursor.x) / (8 * scale));
          int ty = (int)((mouse.y - cursor.y) / (8 * scale));
          if (tx >= 0 && tx < TV_COLS && ty >= 0 && ty < TV_ROWS)
          {
               int idx = ty * TV_COLS + tx;
               ImGui::SetTooltip("Tile #%d (0x%02X)  VRAM 0x%04X",
                                 idx, idx, 0x8000 + idx * 16);
          }
     }
}

/* ────────────────────────────────────────────────────────── */
/* Painel: VRAM – Tilemap Viewer                              */
/* ────────────────────────────────────────────────────────── */

static void update_tilemap_texture(struct gb *gb)
{
     const uint8_t *vram = gb->vram;
     struct gb_gpu *gpu = &gb->gpu;
     bool use_high_tm = s_show_win_map ? gpu->window_use_high_tm
                                       : gpu->bg_use_high_tm;
     bool use_spts = gpu->bg_window_use_sprite_ts;
     unsigned tm_base = use_high_tm ? 0x1C00u : 0x1800u;

     for (int my = 0; my < 256; my++)
     {
          for (int mx = 0; mx < 256; mx++)
          {
               int tc = mx / 8, tr = my / 8;
               int px = mx % 8, py = my % 8;
               unsigned tm_addr = tm_base + (unsigned)tr * 32u + (unsigned)tc;
               uint8_t tidx = vram[tm_addr];

               bool flip_x = false, flip_y = false, high_bank = false;
               int pal_idx = 0;
               if (gb->gbc)
               {
                    uint8_t attr = vram[tm_addr + 0x2000u];
                    flip_x = (attr >> 5) & 1;
                    flip_y = (attr >> 6) & 1;
                    high_bank = (attr >> 3) & 1;
                    pal_idx = attr & 7;
               }

               int rx = flip_x ? (7 - px) : px;
               int ry = flip_y ? (7 - py) : py;

               unsigned tile_addr;
               if (use_spts)
                    tile_addr = (unsigned)tidx * 16u;
               else
                    tile_addr = (unsigned)(0x1000u + (int)(int8_t)tidx * 16);
               if (high_bank)
                    tile_addr += 0x2000u;

               int raw = vram_tile_raw(vram, tile_addr, rx, ry);

               uint32_t pixel;
               if (gb->gbc)
                    pixel = gbc_to_rgba(gpu->bg_palettes.colors[pal_idx][raw]);
               else
                    pixel = dmg_apply(gpu->bgp, raw);

               s_tilemap_buf[my * TM_W + mx] = pixel;
          }
     }

     glBindTexture(GL_TEXTURE_2D, s_tilemap_tex);
     glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, TM_W, TM_H,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_tilemap_buf);
     glBindTexture(GL_TEXTURE_2D, 0);
}

void draw_panel_tilemap_viewer(struct gb *gb)
{
     update_tilemap_texture(gb);

     struct gb_gpu *gpu = &gb->gpu;

     if (ImGui::RadioButton("BG", !s_show_win_map))
          s_show_win_map = false;
     ImGui::SameLine();
     if (ImGui::RadioButton("Window", s_show_win_map))
          s_show_win_map = true;

     bool use_high_tm = s_show_win_map ? gpu->window_use_high_tm
                                       : gpu->bg_use_high_tm;
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "  map=%s  ts=%s",
                        use_high_tm ? "0x9C00" : "0x9800",
                        gpu->bg_window_use_sprite_ts ? "0x8000" : "0x8800");
     ImGui::Separator();

     float avail_w = ImGui::GetContentRegionAvail().x;
     float avail_h = ImGui::GetContentRegionAvail().y - 24.0f;
     float scale_w = avail_w / (float)TM_W;
     float scale_h = avail_h / (float)TM_H;
     float scale = scale_w < scale_h ? scale_w : scale_h;
     if (scale < 1.0f)
          scale = 1.0f;
     if (scale > 3.0f)
          scale = 3.0f;

     ImVec2 img_size(TM_W * scale, TM_H * scale);
     ImVec2 cursor = ImGui::GetCursorScreenPos();

     ImGui::Image((ImTextureID)(intptr_t)s_tilemap_tex, img_size);
     bool img_hovered = ImGui::IsItemHovered();
     bool img_clicked = img_hovered && ImGui::IsMouseClicked(0);

     ImDrawList *dl = ImGui::GetWindowDrawList();

     ImU32 grid_col = IM_COL32(60, 60, 60, 100);
     for (int i = 0; i <= 32; i++)
     {
          float x = cursor.x + i * 8 * scale;
          dl->AddLine(ImVec2(x, cursor.y), ImVec2(x, cursor.y + TM_H * scale), grid_col);
     }
     for (int j = 0; j <= 32; j++)
     {
          float y = cursor.y + j * 8 * scale;
          dl->AddLine(ImVec2(cursor.x, y), ImVec2(cursor.x + TM_W * scale, y), grid_col);
     }

     if (img_hovered)
     {
          ImVec2 mouse = ImGui::GetMousePos();
          int tx = (int)((mouse.x - cursor.x) / (8.f * scale));
          int ty = (int)((mouse.y - cursor.y) / (8.f * scale));
          tx = tx < 0 ? 0 : (tx > 31 ? 31 : tx);
          ty = ty < 0 ? 0 : (ty > 31 ? 31 : ty);

          unsigned tm_base = use_high_tm ? 0x1C00u : 0x1800u;
          uint8_t tidx = gb->vram[tm_base + (unsigned)ty * 32u + (unsigned)tx];
          unsigned tile_addr;
          if (gpu->bg_window_use_sprite_ts)
               tile_addr = (unsigned)tidx * 16u;
          else
               tile_addr = (unsigned)(0x1000u + (int)(int8_t)tidx * 16);
          uint16_t gb_tile_addr = (uint16_t)(0x8000u + tile_addr);

          ImGui::SetTooltip("Tile [%d,%d]  idx=0x%02X  VRAM:0x%04X\n"
                            "Clique para focar no Memory Viewer",
                            tx, ty, tidx, gb_tile_addr);

          if (img_clicked)
          {
               g_mem_addr = gb_tile_addr;
               g_mem_mode = 0;
          }
     }

     if (!s_show_win_map)
     {
          float scx = gpu->scx * scale, scy = gpu->scy * scale;
          float vw = 160.0f * scale, vh = 144.0f * scale;
          float mw = TM_W * scale, mh = TM_H * scale;
          ImU32 rc = IM_COL32(255, 80, 80, 220);

          float x0 = cursor.x + scx, y0 = cursor.y + scy;
          float x1 = x0 + vw, y1 = y0 + vh;

          dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), rc, 0, 0, 2.0f);

          if (x1 > cursor.x + mw)
               dl->AddRect(ImVec2(cursor.x, y0),
                           ImVec2(cursor.x + x1 - cursor.x - mw, y1), rc, 0, 0, 2.0f);
          if (y1 > cursor.y + mh)
               dl->AddRect(ImVec2(x0, cursor.y),
                           ImVec2(x1, cursor.y + y1 - cursor.y - mh), rc, 0, 0, 2.0f);
          if (x1 > cursor.x + mw && y1 > cursor.y + mh)
               dl->AddRect(ImVec2(cursor.x, cursor.y),
                           ImVec2(cursor.x + x1 - cursor.x - mw,
                                  cursor.y + y1 - cursor.y - mh),
                           rc, 0, 0, 2.0f);

          if (gpu->window_enable)
          {
               int wx_real = gpu->wx - 7;
               if (wx_real < 0)
                    wx_real = 0;
               if (wx_real < 160 && gpu->wy < 144)
               {
                    int map_x = (gpu->scx + wx_real) & 0xFF;
                    int map_y = (gpu->scy + gpu->wy) & 0xFF;
                    int win_w = 160 - wx_real;
                    int win_h = 144 - gpu->wy;
                    ImU32 wc = IM_COL32(80, 200, 255, 220);

                    float wx0 = cursor.x + map_x * scale;
                    float wy0 = cursor.y + map_y * scale;
                    float wx1 = wx0 + win_w * scale;
                    float wy1 = wy0 + win_h * scale;

                    dl->AddRect(ImVec2(wx0, wy0), ImVec2(wx1, wy1), wc, 0, 0, 2.0f);

                    if (wx1 > cursor.x + mw)
                         dl->AddRect(ImVec2(cursor.x, wy0),
                                     ImVec2(cursor.x + wx1 - cursor.x - mw, wy1), wc, 0, 0, 2.0f);
                    if (wy1 > cursor.y + mh)
                         dl->AddRect(ImVec2(wx0, cursor.y),
                                     ImVec2(wx1, cursor.y + wy1 - cursor.y - mh), wc, 0, 0, 2.0f);
                    if (wx1 > cursor.x + mw && wy1 > cursor.y + mh)
                         dl->AddRect(ImVec2(cursor.x, cursor.y),
                                     ImVec2(cursor.x + wx1 - cursor.x - mw,
                                            cursor.y + wy1 - cursor.y - mh),
                                     wc, 0, 0, 2.0f);
               }
          }

          ImGui::Text("Viewport: SCX=%d  SCY=%d", gpu->scx, gpu->scy);
          if (gpu->window_enable)
          {
               ImGui::SameLine();
               ImGui::TextColored(ImVec4(0.3f, 0.78f, 1.0f, 1.0f),
                                  "  Win: WX=%d WY=%d", gpu->wx, gpu->wy);
          }
     }
     else
     {
          int wx_real = gpu->wx - 7;
          if (wx_real < 0)
               wx_real = 0;
          int vis_w = 160 - wx_real;
          int vis_h = 144 - gpu->wy;
          if (vis_w > 0 && vis_h > 0 && vis_w <= 160 && vis_h <= 144)
          {
               ImU32 wc = IM_COL32(80, 200, 255, 220);
               dl->AddRect(ImVec2(cursor.x, cursor.y),
                           ImVec2(cursor.x + vis_w * scale, cursor.y + vis_h * scale),
                           wc, 0, 0, 2.0f);
          }
          ImGui::Text("Window: WX=%d  WY=%d  (X real=%d)", gpu->wx, gpu->wy, gpu->wx - 7);
     }
}

/* ────────────────────────────────────────────────────────── */
/* Popup: Informações da ROM                                  */
/* ────────────────────────────────────────────────────────── */

static const char *mbc_model_name(enum gb_cart_model m)
{
     switch (m)
     {
     case GB_CART_SIMPLE:
          return "ROM Only";
     case GB_CART_MBC1:
          return "MBC1";
     case GB_CART_MBC2:
          return "MBC2";
     case GB_CART_MBC3:
          return "MBC3";
     case GB_CART_MBC5:
          return "MBC5";
     case GB_CART_MBC7:
          return "MBC7";
     case GB_CART_HUC1:
          return "HuC1";
     case GB_CART_HUC3:
          return "HuC3";
     }
     return "?";
}

void draw_popup_rom_info(struct gb *gb)
{
     if (g_show_rom_info)
     {
          ImGui::OpenPopup("Informa\xc3\xa7\xc3\xb5"
                           "es da ROM##modal");
          g_show_rom_info = false;
     }

     if (!ImGui::BeginPopupModal("Informa\xc3\xa7\xc3\xb5"
                                 "es da ROM##modal",
                                 nullptr, ImGuiWindowFlags_AlwaysAutoResize))
          return;

     struct gb_cart *cart = &gb->cart;
     if (!cart->rom || cart->rom_length < 0x150)
     {
          ImGui::TextColored(color_gray, "(Nenhuma ROM carregada)");
          ImGui::Separator();
          if (ImGui::Button("Fechar", ImVec2(120, 0)))
               ImGui::CloseCurrentPopup();
          ImGui::EndPopup();
          return;
     }

     char title[17] = {0};
     memcpy(title, &cart->rom[0x0134], 16);
     ImGui::TextColored(color_cyan, "T\xc3\xad"
                                    "tulo:   ");
     ImGui::SameLine();
     ImGui::TextColored(color_white, "%s", title[0] ? title : "(sem t\xc3\xad"
                                                              "tulo)");

     char maker[5] = {0};
     memcpy(maker, &cart->rom[0x013F], 4);
     ImGui::TextColored(color_cyan, "Maker:  ");
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "%.4s", maker);

     ImGui::Separator();

     uint8_t cgb_flag = cart->rom[0x0143];
     uint8_t sgb_flag = cart->rom[0x0146];
     ImGui::TextColored(color_cyan, "Modo:   ");
     ImGui::SameLine();
     if (cgb_flag == 0x80)
          ImGui::TextColored(color_green, "GB + GBC");
     else if (cgb_flag == 0xC0)
          ImGui::TextColored(color_green, "GBC apenas");
     else
          ImGui::TextColored(color_gray, "DMG");
     ImGui::SameLine();
     if (sgb_flag == 0x03)
          ImGui::TextColored(color_yellow, "  (SGB)");

     uint8_t cart_type = cart->rom[0x0147];
     ImGui::TextColored(color_cyan, "MBC:    ");
     ImGui::SameLine();
     ImGui::TextColored(color_yellow, "0x%02X  %s", cart_type, mbc_model_name(cart->model));

     static const char *k_rom_sizes[] = {
         "32 KiB", "64 KiB", "128 KiB", "256 KiB", "512 KiB",
         "1 MiB", "2 MiB", "4 MiB", "8 MiB"};
     uint8_t rom_sz_code = cart->rom[0x0148];
     ImGui::TextColored(color_cyan, "ROM:    ");
     ImGui::SameLine();
     ImGui::TextColored(color_white, "%u bancos  (%s)",
                        cart->rom_banks,
                        rom_sz_code < 9 ? k_rom_sizes[rom_sz_code] : "?");

     static const char *k_ram_sizes[] = {
         "Nenhuma", "2 KiB", "8 KiB", "32 KiB", "128 KiB", "64 KiB"};
     uint8_t ram_sz_code = cart->rom[0x0149];
     ImGui::TextColored(color_cyan, "SRAM:   ");
     ImGui::SameLine();
     ImGui::TextColored(color_white, "%u bancos  (%s)",
                        cart->ram_banks,
                        ram_sz_code < 6 ? k_ram_sizes[ram_sz_code] : "?");

     ImGui::Separator();

     uint8_t hdr_cs = cart->rom[0x014D];
     uint8_t calc_cs = 0;
     for (int i = 0x0134; i <= 0x014C; i++)
          calc_cs = calc_cs - cart->rom[i] - 1;
     bool cs_ok = (calc_cs == hdr_cs);
     ImGui::TextColored(color_cyan, "Checksum header: ");
     ImGui::SameLine();
     ImGui::TextColored(cs_ok ? color_green : color_red,
                        "0x%02X %s", hdr_cs, cs_ok ? "(OK)" : "(ERRO!)");

     ImGui::TextColored(color_cyan, "Tamanho do arquivo: ");
     ImGui::SameLine();
     if (cart->rom_length >= 1024 * 1024)
          ImGui::TextColored(color_gray, "%.2f MiB", cart->rom_length / (1024.0f * 1024.0f));
     else
          ImGui::TextColored(color_gray, "%u KiB", cart->rom_length / 1024);

     ImGui::Separator();
     if (ImGui::Button("Fechar", ImVec2(120, 0)))
          ImGui::CloseCurrentPopup();

     ImGui::EndPopup();
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Call Stack                                         */
/* ────────────────────────────────────────────────────────── */

void draw_panel_call_stack(struct gb *gb)
{
     uint16_t sp = gb->cpu.sp;
     ImGui::TextColored(color_orange, "SP = 0x%04X", sp);
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "  (l\xc3\xaa at\xc3\xa9 16 entradas a partir do SP)");
     ImGui::Separator();

     ImGuiTableFlags tf =
         ImGuiTableFlags_BordersOuter |
         ImGuiTableFlags_BordersInnerV |
         ImGuiTableFlags_RowBg |
         ImGuiTableFlags_ScrollY;

     if (!ImGui::BeginTable("##callstack", 3, tf))
          return;

     ImGui::TableSetupScrollFreeze(0, 1);
     ImGui::TableSetupColumn("Endere\xc3\xa7o", ImGuiTableColumnFlags_WidthFixed, 68);
     ImGui::TableSetupColumn("Retorno", ImGuiTableColumnFlags_WidthFixed, 68);
     ImGui::TableSetupColumn("Instr. chamadora", ImGuiTableColumnFlags_WidthStretch);
     ImGui::TableHeadersRow();

     static const uint8_t k_call_ops[] = {0xCD, 0xC4, 0xCC, 0xD4, 0xDC};

     for (int i = 0; i < 16; i++)
     {
          uint16_t stack_addr = (uint16_t)(sp + (unsigned)(i * 2));
          if (stack_addr > 0xFFFE)
               break;

          uint8_t lo = gb_memory_peekb(gb, stack_addr);
          uint8_t hi = gb_memory_peekb(gb, (uint16_t)(stack_addr + 1));
          uint16_t ret_addr = (uint16_t)((hi << 8) | lo);

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextColored(color_orange, "0x%04X", stack_addr);
          ImGui::TableSetColumnIndex(1);
          ImGui::TextColored(color_cyan, "0x%04X", ret_addr);
          ImGui::TableSetColumnIndex(2);

          bool found_call = false;
          if (ret_addr >= 3)
          {
               uint8_t prev = gb_memory_peekb(gb, (uint16_t)(ret_addr - 3));
               for (uint8_t op : k_call_ops)
               {
                    if (prev == op)
                    {
                         char mn[48];
                         gb_disasm(gb, (uint16_t)(ret_addr - 3), mn, sizeof(mn));
                         ImGui::TextColored(color_gray, "%s", mn);
                         found_call = true;
                         break;
                    }
               }
          }
          if (!found_call)
               ImGui::TextColored(color_gray, "--");
     }

     ImGui::EndTable();
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Controle de Execução                               */
/* ────────────────────────────────────────────────────────── */

static bool cpu_parse_hex_u32(const char *text, uint32_t *out)
{
     if (!text || !text[0])
          return false;
     char *end = NULL;
     unsigned long value = strtoul(text, &end, 16);
     if (end == text)
          return false;
     *out = (uint32_t)value;
     return true;
}

static uint8_t cpu_flags_value(const gb_cpu *cpu)
{
     return (uint8_t)((cpu->f_z ? 0x80 : 0) |
                      (cpu->f_n ? 0x40 : 0) |
                      (cpu->f_h ? 0x20 : 0) |
                      (cpu->f_c ? 0x10 : 0));
}

static void cpu_set_flags_value(gb_cpu *cpu, uint8_t f)
{
     cpu->f_z = (f & 0x80) != 0;
     cpu->f_n = (f & 0x40) != 0;
     cpu->f_h = (f & 0x20) != 0;
     cpu->f_c = (f & 0x10) != 0;
}

static const char *cpu_bin8(uint8_t v)
{
     static char bufs[4][12];
     static int idx = 0;
     char *out = bufs[idx++ & 3];
     for (int i = 7, j = 0; i >= 0; i--)
     {
          out[j++] = (v & (1u << i)) ? '1' : '0';
          if (i == 4)
               out[j++] = ' ';
     }
     out[9] = '\0';
     return out;
}

static bool cpu_edit_u8(const char *name, uint16_t id, uint8_t value,
                        ImVec4 label_color, uint8_t *out)
{
     static ImGuiID editing_id = 0;
     static int editing_frames = 0;
     static char edit_buf[8] = "";

     bool changed = false;
     ImGui::PushID((int)id);
     ImGuiID widget_id = ImGui::GetID("##cpu_u8");

     ImGui::TextColored(label_color, "%2s", name);
     ImGui::SameLine();

     if (editing_id == widget_id)
     {
          ImGui::SetNextItemWidth(32);
          if (editing_frames == 0)
               ImGui::SetKeyboardFocusHere();
          bool enter = ImGui::InputText("##edit", edit_buf, sizeof(edit_buf),
                                        ImGuiInputTextFlags_CharsHexadecimal |
                                            ImGuiInputTextFlags_CharsUppercase |
                                            ImGuiInputTextFlags_AutoSelectAll |
                                            ImGuiInputTextFlags_EnterReturnsTrue);
          editing_frames++;
          if (enter)
          {
               uint32_t parsed;
               if (cpu_parse_hex_u32(edit_buf, &parsed))
               {
                    *out = (uint8_t)parsed;
                    changed = true;
               }
               editing_id = 0;
          }
          if ((editing_frames > 1 && !ImGui::IsItemActive()) || ImGui::IsKeyPressed(ImGuiKey_Escape))
               editing_id = 0;
     }
     else
     {
          char text[8];
          snprintf(text, sizeof(text), "$%02X", value);
          if (ImGui::Selectable(text, false, 0, ImVec2(40, 0)))
          {
               editing_id = widget_id;
               editing_frames = 0;
               snprintf(edit_buf, sizeof(edit_buf), "%02X", value);
          }
          if (ImGui::IsItemHovered())
          {
               char ascii = (value >= 32 && value < 127) ? (char)value : '.';
               ImGui::SetTooltip("Hex $%02X\nDec %u / %d\nBin %s\nASCII %c",
                                 value, value, (int8_t)value, cpu_bin8(value), ascii);
          }
     }

     ImGui::TextColored(color_gray, "%s", cpu_bin8(value));
     ImGui::PopID();
     return changed;
}

static bool cpu_edit_u16(const char *name, uint16_t id, uint16_t value,
                         ImVec4 label_color, uint16_t *out)
{
     static ImGuiID editing_id = 0;
     static int editing_frames = 0;
     static char edit_buf[8] = "";

     bool changed = false;
     ImGui::PushID((int)id + 0x1000);
     ImGuiID widget_id = ImGui::GetID("##cpu_u16");

     ImGui::TextColored(label_color, "%2s", name);
     ImGui::SameLine();

     if (editing_id == widget_id)
     {
          ImGui::SetNextItemWidth(48);
          if (editing_frames == 0)
               ImGui::SetKeyboardFocusHere();
          bool enter = ImGui::InputText("##edit", edit_buf, sizeof(edit_buf),
                                        ImGuiInputTextFlags_CharsHexadecimal |
                                            ImGuiInputTextFlags_CharsUppercase |
                                            ImGuiInputTextFlags_AutoSelectAll |
                                            ImGuiInputTextFlags_EnterReturnsTrue);
          editing_frames++;
          if (enter)
          {
               uint32_t parsed;
               if (cpu_parse_hex_u32(edit_buf, &parsed))
               {
                    *out = (uint16_t)parsed;
                    changed = true;
               }
               editing_id = 0;
          }
          if ((editing_frames > 1 && !ImGui::IsItemActive()) || ImGui::IsKeyPressed(ImGuiKey_Escape))
               editing_id = 0;
     }
     else
     {
          char text[8];
          snprintf(text, sizeof(text), "$%04X", value);
          if (ImGui::Selectable(text, false, 0, ImVec2(56, 0)))
          {
               editing_id = widget_id;
               editing_frames = 0;
               snprintf(edit_buf, sizeof(edit_buf), "%04X", value);
          }
          if (ImGui::IsItemHovered())
          {
               ImGui::SetTooltip("Hex $%04X\nDec %u / %d\nHigh %s  Low %s",
                                 value, value, (int16_t)value,
                                 cpu_bin8((uint8_t)(value >> 8)),
                                 cpu_bin8((uint8_t)value));
          }
     }

     ImGui::TextColored(color_gray, "%s %s",
                        cpu_bin8((uint8_t)(value >> 8)), cpu_bin8((uint8_t)value));
     ImGui::PopID();
     return changed;
}

static void draw_cpu_flag_bit(const char *name, bool *flag)
{
     ImGui::TextColored(color_orange, "%s", name);
     ImGui::SameLine();
     ImGui::PushID(name);
     ImGui::PushStyleColor(ImGuiCol_Text, *flag ? color_green : color_gray);
     if (ImGui::Selectable(*flag ? "1" : "0", false, 0, ImVec2(18, 0)))
          *flag = !*flag;
     ImGui::PopStyleColor();
     ImGui::PopID();
}

static void draw_cpu_toolbar(struct gb *gb)
{
     struct gb_debug *dbg = &gb->debug;

     if (gb->cart.rom && gb->cart.rom_length > 0x0143)
     {
          char title[17] = {0};
          memcpy(title, &gb->cart.rom[0x0134], 16);
          for (int i = 15; i >= 0; i--)
               if (title[i] == '\0' || title[i] == ' ')
                    title[i] = '\0';
               else
                    break;
          if (title[0])
               ImGui::TextColored(color_cyan, "ROM: %s", title);
     }

     bool paused = dbg->state == GB_DEBUG_PAUSED;
     bool running = dbg->state == GB_DEBUG_RUNNING;

     if (ImGui::Button(running ? "Break" : "Continue"))
     {
          dbg->enabled = true;
          if (running)
               dbg->state = GB_DEBUG_PAUSED;
          else
               dbg->state = GB_DEBUG_RUNNING;
     }
     if (ImGui::IsItemHovered())
          ImGui::SetTooltip(running ? "Pause emulation" : "Start / Continue");

     ImGui::SameLine();
     ImGui::BeginDisabled(!paused);
     if (ImGui::Button("Step In"))
          dbg->state = GB_DEBUG_STEPPING;
     if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Step Into [F10]");
     ImGui::EndDisabled();

     ImGui::SameLine();
     ImGui::BeginDisabled(!paused);
     if (ImGui::Button("Step Over"))
          gb_debug_step_over(gb);
     if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Step over CALL");
     ImGui::EndDisabled();

     ImGui::SameLine();
     ImGui::BeginDisabled(!paused);
     if (ImGui::Button("Step Out"))
          gb_debug_step_out(gb);
     if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Continue until return");
     ImGui::EndDisabled();

     ImGui::SameLine();
     if (ImGui::Button("Reset"))
          debug_ui_action_reset(gb, true);

     ImGui::SameLine();
     ImGui::TextColored(paused ? color_red : color_green, paused ? "PAUSED" : "RUNNING");

     ImGui::TextColored(color_gray, "instructions %llu", (unsigned long long)dbg->instruction_count);
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "cycles %llu", (unsigned long long)dbg->cycle_count);
     ImGui::SameLine();
     ImGui::TextColored(gb->cpu.halted ? color_yellow : color_gray, "HALT");
     ImGui::SameLine();
     ImGui::TextColored(gb->cpu.stopped ? color_yellow : color_gray, "STOP");
     ImGui::SameLine();
     ImGui::TextColored(gb->cpu.irq_enable ? color_green : color_gray, "IME");
}

static void draw_cpu_register_editor(struct gb *gb)
{
     struct gb_cpu *cpu = &gb->cpu;

     ImGui::TextColored(color_orange, "Flags");
     ImGui::SameLine();
     draw_cpu_flag_bit("Z", &cpu->f_z);
     ImGui::SameLine();
     draw_cpu_flag_bit("N", &cpu->f_n);
     ImGui::SameLine();
     draw_cpu_flag_bit("H", &cpu->f_h);
     ImGui::SameLine();
     draw_cpu_flag_bit("C", &cpu->f_c);
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "F=$%02X", cpu_flags_value(cpu));

     ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(4.0f, 3.0f));
     if (ImGui::BeginTable("##cpu_regs", 2,
                           ImGuiTableFlags_BordersInnerH |
                               ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_NoPadOuterX))
     {
          ImGui::TableSetupColumn("Left", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("Right", ImGuiTableColumnFlags_WidthStretch);

          uint16_t pc = cpu->pc, sp = cpu->sp;
          uint16_t bc = (uint16_t)((cpu->b << 8) | cpu->c);
          uint16_t de = (uint16_t)((cpu->d << 8) | cpu->e);
          uint16_t hl = (uint16_t)((cpu->h << 8) | cpu->l);
          uint8_t f = cpu_flags_value(cpu);
          uint8_t a = cpu->a, b = cpu->b, c = cpu->c, d = cpu->d;
          uint8_t e = cpu->e, h = cpu->h, l = cpu->l;

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          if (cpu_edit_u16("PC", 1, pc, color_yellow, &pc))
               cpu->pc = pc;
          ImGui::TableSetColumnIndex(1);
          if (cpu_edit_u16("SP", 2, sp, color_yellow, &sp))
               cpu->sp = sp;

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          if (cpu_edit_u8("A", 3, a, color_cyan, &a))
               cpu->a = a;
          ImGui::TableSetColumnIndex(1);
          if (cpu_edit_u8("F", 4, f, color_cyan, &f))
               cpu_set_flags_value(cpu, f);

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          if (cpu_edit_u8("B", 5, b, color_cyan, &b))
               cpu->b = b;
          ImGui::TableSetColumnIndex(1);
          if (cpu_edit_u8("C", 6, c, color_cyan, &c))
               cpu->c = c;

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          if (cpu_edit_u8("D", 7, d, color_cyan, &d))
               cpu->d = d;
          ImGui::TableSetColumnIndex(1);
          if (cpu_edit_u8("E", 8, e, color_cyan, &e))
               cpu->e = e;

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          if (cpu_edit_u8("H", 9, h, color_cyan, &h))
               cpu->h = h;
          ImGui::TableSetColumnIndex(1);
          if (cpu_edit_u8("L", 10, l, color_cyan, &l))
               cpu->l = l;

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          if (cpu_edit_u16("BC", 11, bc, color_skyblue, &bc))
          {
               cpu->b = (uint8_t)(bc >> 8);
               cpu->c = (uint8_t)bc;
          }
          ImGui::TableSetColumnIndex(1);
          if (cpu_edit_u16("DE", 12, de, color_green, &de))
          {
               cpu->d = (uint8_t)(de >> 8);
               cpu->e = (uint8_t)de;
          }

          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          if (cpu_edit_u16("HL", 13, hl, color_orange, &hl))
          {
               cpu->h = (uint8_t)(hl >> 8);
               cpu->l = (uint8_t)hl;
          }
          ImGui::TableSetColumnIndex(1);
          ImGui::TextColored(cpu->irq_enable ? color_green : color_gray, "IME");
          ImGui::SameLine();
          if (ImGui::Selectable(cpu->irq_enable ? "enabled" : "disabled", false, 0, ImVec2(74, 0)))
               cpu->irq_enable = !cpu->irq_enable;

          ImGui::EndTable();
     }
     ImGui::PopStyleVar();
}

void draw_panel_control(struct gb *gb)
{
     draw_cpu_toolbar(gb);
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Registradores CPU                                  */
/* ────────────────────────────────────────────────────────── */

void draw_panel_registers(struct gb *gb)
{
     draw_cpu_register_editor(gb);
}

void draw_panel_cpu_control(struct gb *gb)
{
     draw_cpu_toolbar(gb);
     ImGui::Separator();
     draw_cpu_register_editor(gb);

     ImGui::Separator();
     if (ImGui::BeginTabBar("##cpu_debug_tabs"))
     {
          if (ImGui::BeginTabItem("Breakpoints"))
          {
               draw_panel_breakpoints(gb);
               ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem("Watchpoints"))
          {
               draw_panel_watchpoints(gb);
               ImGui::EndTabItem();
          }
          if (ImGui::BeginTabItem("Stack"))
          {
               draw_panel_call_stack(gb);
               ImGui::EndTabItem();
          }
          ImGui::EndTabBar();
     }
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Disassembly                                        */
/* ────────────────────────────────────────────────────────── */

static int s_disasm_selected_addr = -1;
static int s_disasm_scroll_addr = -1;
static int s_disasm_back_scroll = 0;
static bool s_disasm_follow_pc = true;
static char s_disasm_goto[8] = "";
static char s_disasm_runto[8] = "";

static bool parse_hex_u16(const char *text, uint16_t *out)
{
     if (!text || !text[0])
          return false;
     char *end = NULL;
     unsigned long value = strtoul(text, &end, 16);
     if (end == text || value > 0xFFFFul)
          return false;
     *out = (uint16_t)value;
     return true;
}

static const char *disasm_segment_name(uint16_t addr)
{
     if (addr < 0x4000)
          return "ROM0";
     if (addr < 0x8000)
          return "ROMX";
     if (addr < 0xA000)
          return "VRAM";
     if (addr < 0xC000)
          return "ERAM";
     if (addr < 0xE000)
          return "WRAM";
     if (addr < 0xFE00)
          return "ECHO";
     if (addr < 0xFEA0)
          return "OAM ";
     if (addr < 0xFF00)
          return "----";
     if (addr < 0xFF80)
          return "IO  ";
     if (addr < 0xFFFF)
          return "HRAM";
     return "IE  ";
}

static unsigned disasm_bank_for_addr(struct gb *gb, uint16_t addr)
{
     if (addr < 0x4000)
          return 0;
     if (addr < 0x8000)
          return gb->cart.cur_rom_bank;
     if (addr >= 0xA000 && addr < 0xC000)
          return gb->cart.cur_ram_bank;
     if (addr >= 0x8000 && addr < 0xA000)
          return gb->vram_high_bank ? 1u : 0u;
     if (addr >= 0xD000 && addr < 0xE000)
          return gb->iram_high_bank;
     return 0;
}

static bool disasm_is_return(const gb_disasm_instr *ins)
{
     return strcmp(ins->mnemonic, "RET") == 0 ||
            strcmp(ins->mnemonic, "RETI") == 0;
}

static void disasm_toggle_breakpoint(struct gb *gb, uint16_t addr)
{
     for (unsigned i = 0; i < gb->debug.n_breakpoints; i++)
     {
          if (gb->debug.breakpoints[i] == addr)
          {
               gb_debug_remove_breakpoint(gb, i);
               return;
          }
     }
     gb_debug_add_breakpoint(gb, addr);
}

static void draw_disasm_controls(struct gb *gb)
{
     struct gb_debug *dbg = &gb->debug;
     bool paused = dbg->state == GB_DEBUG_PAUSED;

     if (ImGui::Button("Continue"))
     {
          dbg->enabled = true;
          dbg->state = GB_DEBUG_RUNNING;
     }
     if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Start / Continue");

     ImGui::SameLine();
     if (ImGui::Button("Break"))
     {
          dbg->enabled = true;
          dbg->state = GB_DEBUG_PAUSED;
     }
     if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Stop at current instruction");

     ImGui::SameLine();
     ImGui::BeginDisabled(!paused);
     if (ImGui::Button("Step In"))
          dbg->state = GB_DEBUG_STEPPING;
     if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Step Into [F10]");

     ImGui::SameLine();
     if (ImGui::Button("Step Over"))
          gb_debug_step_over(gb);
     if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Step Over current CALL");

     ImGui::SameLine();
     if (ImGui::Button("Step Out"))
          gb_debug_step_out(gb);
     if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Continue until return");
     ImGui::EndDisabled();

     ImGui::SameLine();
     if (ImGui::Button("Run Cursor") && s_disasm_selected_addr >= 0)
     {
          gb_debug_add_breakpoint(gb, (uint16_t)s_disasm_selected_addr);
          dbg->enabled = true;
          dbg->state = GB_DEBUG_RUNNING;
     }
     if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
          ImGui::SetTooltip("Add a breakpoint at selected address and continue");

     ImGui::SameLine();
     ImGui::TextColored(paused ? color_red : color_green, paused ? "PAUSED" : "RUNNING");

     ImGui::SameLine();
     ImGui::Checkbox("Follow PC", &s_disasm_follow_pc);

     ImGui::SetNextItemWidth(64);
     if (ImGui::InputTextWithHint("##dis_goto", "0000", s_disasm_goto, sizeof(s_disasm_goto),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase |
                                      ImGuiInputTextFlags_EnterReturnsTrue))
     {
          uint16_t addr;
          if (parse_hex_u16(s_disasm_goto, &addr))
               s_disasm_scroll_addr = addr;
          s_disasm_goto[0] = 0;
     }
     ImGui::SameLine();
     if (ImGui::Button("GoTo"))
     {
          uint16_t addr;
          if (parse_hex_u16(s_disasm_goto, &addr))
               s_disasm_scroll_addr = addr;
          s_disasm_goto[0] = 0;
     }
     ImGui::SameLine();
     if (ImGui::Button("PC"))
          s_disasm_scroll_addr = gb->cpu.pc;
     ImGui::SameLine();
     if (ImGui::Button("Back"))
          s_disasm_scroll_addr = s_disasm_back_scroll;

     ImGui::SameLine();
     ImGui::SetNextItemWidth(64);
     if (ImGui::InputTextWithHint("##dis_runto", "0000", s_disasm_runto, sizeof(s_disasm_runto),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase |
                                      ImGuiInputTextFlags_EnterReturnsTrue))
     {
          uint16_t addr;
          if (parse_hex_u16(s_disasm_runto, &addr))
          {
               gb_debug_add_breakpoint(gb, addr);
               dbg->enabled = true;
               dbg->state = GB_DEBUG_RUNNING;
          }
          s_disasm_runto[0] = 0;
     }
     ImGui::SameLine();
     if (ImGui::Button("Run To"))
     {
          uint16_t addr;
          if (parse_hex_u16(s_disasm_runto, &addr))
          {
               gb_debug_add_breakpoint(gb, addr);
               dbg->enabled = true;
               dbg->state = GB_DEBUG_RUNNING;
          }
          s_disasm_runto[0] = 0;
     }
}

void draw_panel_disasm(struct gb *gb)
{
     static uint16_t s_prev_pc = 0xFFFF;
     uint16_t pc = gb->cpu.pc;
     bool pc_changed = (pc != s_prev_pc);
     s_prev_pc = pc;

     draw_disasm_controls(gb);
     ImGui::Separator();

     float line_h = ImGui::GetTextLineHeightWithSpacing();
     ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit |
                             ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_ScrollX |
                             ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_NoPadOuterX;

     ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.0f, 1.0f));
     if (ImGui::BeginTable("##gear_disasm", 7, flags, ImVec2(0, 0)))
     {
          ImGui::TableSetupColumn("BP", ImGuiTableColumnFlags_WidthFixed, 22);
          ImGui::TableSetupColumn("SEG", ImGuiTableColumnFlags_WidthFixed, 42);
          ImGui::TableSetupColumn("BANK", ImGuiTableColumnFlags_WidthFixed, 36);
          ImGui::TableSetupColumn("ADDR", ImGuiTableColumnFlags_WidthFixed, 54);
          ImGui::TableSetupColumn("PC", ImGuiTableColumnFlags_WidthFixed, 24);
          ImGui::TableSetupColumn("INSTRUCTION", ImGuiTableColumnFlags_WidthStretch, 180);
          ImGui::TableSetupColumn("BYTES", ImGuiTableColumnFlags_WidthFixed, 78);
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableHeadersRow();

          if (pc_changed && s_disasm_follow_pc)
               s_disasm_scroll_addr = pc;
          if (s_disasm_scroll_addr >= 0)
          {
               s_disasm_back_scroll = (int)(ImGui::GetScrollY() / line_h);
               ImGui::SetScrollY((float)s_disasm_scroll_addr * line_h - ImGui::GetWindowHeight() * 0.45f);
               s_disasm_scroll_addr = -1;
          }

          ImGuiListClipper clipper;
          clipper.Begin(0x10000, line_h);
          while (clipper.Step())
          {
               for (int item = clipper.DisplayStart; item < clipper.DisplayEnd; item++)
               {
                    uint16_t addr = (uint16_t)item;
                    gb_disasm_instr ins;
                    int len = gb_disasm_ex(gb, addr, &ins);
                    if (len <= 0)
                         len = 1;

                    bool is_pc = addr == pc;
                    bool is_bp = gb_debug_has_breakpoint(gb, addr);
                    bool is_selected = s_disasm_selected_addr == (int)addr;

                    ImGui::PushID(item);
                    ImGui::TableNextRow();
                    if (is_selected)
                         ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                                ImGui::ColorConvertFloat4ToU32(ImVec4(0.05f, 0.17f, 0.22f, 1.0f)));
                    else if (is_bp)
                         ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                                ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f, 0.05f, 0.05f, 1.0f)));
                    else if (is_pc)
                         ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                                ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f, 0.17f, 0.04f, 1.0f)));

                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(is_bp ? color_red : color_gray, is_bp ? "B" : ".");
                    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0))
                         disasm_toggle_breakpoint(gb, addr);

                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(color_orange, "%s", disasm_segment_name(addr));

                    ImGui::TableSetColumnIndex(2);
                    ImGui::TextColored(color_skyblue, "%02X", disasm_bank_for_addr(gb, addr));

                    ImGui::TableSetColumnIndex(3);
                    char addr_text[8];
                    snprintf(addr_text, sizeof(addr_text), "%04X", addr);
                    if (ImGui::Selectable(addr_text, is_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
                    {
                         if (ImGui::IsMouseDoubleClicked(0) && ins.has_target)
                              s_disasm_scroll_addr = ins.target;
                         else
                              s_disasm_selected_addr = is_selected ? -1 : (int)addr;
                    }

                    if (ImGui::BeginPopupContextItem("##dis_ctx"))
                    {
                         s_disasm_selected_addr = addr;
                         if (ImGui::Selectable("Run To Cursor"))
                         {
                              gb_debug_add_breakpoint(gb, addr);
                              gb->debug.state = GB_DEBUG_RUNNING;
                         }
                         if (ImGui::Selectable(is_bp ? "Remove Breakpoint" : "Add Breakpoint"))
                              disasm_toggle_breakpoint(gb, addr);
                         if (ins.has_target && ImGui::Selectable("Go To Target"))
                              s_disasm_scroll_addr = ins.target;
                         if (ImGui::Selectable("View In Memory"))
                         {
                              g_mem_mode = 0;
                              g_mem_addr = addr;
                         }
                         ImGui::EndPopup();
                    }

                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextColored(is_pc ? color_yellow : color_gray, is_pc ? "->" : "  ");

                    ImGui::TableSetColumnIndex(5);
                    ImGui::TextColored(is_pc ? color_yellow : color_white, "%s", ins.mnemonic);
                    if (ins.operands[0])
                    {
                         ImGui::SameLine(0, 8);
                         ImGui::TextColored(ins.has_target ? color_cyan : color_gray, "%s", ins.operands);
                         if (ins.has_target && ImGui::IsItemHovered())
                         {
                              ImGui::SetTooltip("Go to %04X", ins.target);
                              if (ImGui::IsMouseClicked(0))
                                   s_disasm_scroll_addr = ins.target;
                         }
                    }

                    ImGui::TableSetColumnIndex(6);
                    char bytes[16] = "";
                    for (int b = 0; b < len && b < 3; b++)
                    {
                         char tmp[5];
                         snprintf(tmp, sizeof(tmp), "%02X ", ins.bytes[b]);
                         strncat(bytes, tmp, sizeof(bytes) - strlen(bytes) - 1);
                    }
                    ImGui::TextColored(color_gray, "%s", bytes);

                    if (disasm_is_return(&ins))
                    {
                         ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
                                                ImGui::ColorConvertFloat4ToU32(ImVec4(0.04f, 0.13f, 0.08f, 1.0f)));
                    }

                    ImGui::PopID();
               }
          }
          ImGui::EndTable();
     }
     ImGui::PopStyleVar();
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Estado da GPU                                      */
/* ────────────────────────────────────────────────────────── */

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

void draw_panel_gpu(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     int mode = gb_gpu_get_mode(gb);

     ImGui::TextColored(color_cyan, "Modo: %s", gpu_mode_name(mode));
     ImGui::Text("LY:  %3d    LYC: %3d  %s",
                 gpu->ly, gpu->lyc,
                 (gpu->ly == gpu->lyc) ? "(coincide!)" : "");
     ImGui::Text("SCX: %3d    SCY: %3d", gpu->scx, gpu->scy);
     ImGui::Text("WX:  %3d    WY:  %3d  (X real = WX-7 = %d)",
                 gpu->wx, gpu->wy, gpu->wx - 7);
     ImGui::Separator();

     ImGui::Text("LCDC:");
     ImGui::BulletText("LCD enable:      %s", gpu->master_enable ? "ON" : "OFF");
     ImGui::BulletText("Window tile map: %s", gpu->window_use_high_tm ? "0x9C00" : "0x9800");
     ImGui::BulletText("Window enable:   %s", gpu->window_enable ? "ON" : "OFF");
     ImGui::BulletText("BG/Win tile set: %s", gpu->bg_window_use_sprite_ts ? "0x8000" : "0x8800");
     ImGui::BulletText("BG tile map:     %s", gpu->bg_use_high_tm ? "0x9C00" : "0x9800");
     ImGui::BulletText("Sprite size:     %s", gpu->tall_sprites ? "8x16" : "8x8");
     ImGui::BulletText("Sprite enable:   %s", gpu->sprite_enable ? "ON" : "OFF");
     ImGui::BulletText("BG enable:       %s", gpu->bg_enable ? "ON" : "OFF");
     ImGui::Separator();

     ImGui::Text("STAT interrupts:");
     ImGui::BulletText("LYC=LY: %s", gpu->iten_lyc ? "ON" : "OFF");
     ImGui::BulletText("Modo 2: %s", gpu->iten_mode2 ? "ON" : "OFF");
     ImGui::BulletText("Modo 1: %s", gpu->iten_mode1 ? "ON" : "OFF");
     ImGui::BulletText("Modo 0: %s", gpu->iten_mode0 ? "ON" : "OFF");
     ImGui::Separator();

     ImGui::Text("Paletas DMG:");
     ImGui::Text("  BGP : 0x%02X", gpu->bgp);
     ImGui::Text("  OBP0: 0x%02X", gpu->obp0);
     ImGui::Text("  OBP1: 0x%02X", gpu->obp1);
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Memory Viewer                                      */
/* ────────────────────────────────────────────────────────── */

enum mem_region_id
{
     MEM_REGION_CPU,
     MEM_REGION_ROM_BANK,
     MEM_REGION_FULL_ROM,
     MEM_REGION_CART_RAM,
     MEM_REGION_WRAM,
     MEM_REGION_VRAM,
     MEM_REGION_OAM,
     MEM_REGION_HRAM,
     MEM_REGION_COUNT
};

static int s_mem_region = MEM_REGION_CPU;
static int s_mem_rom_bank = 1;
static int s_mem_bytes_per_row = 16;
static bool s_mem_show_ascii = true;
static bool s_mem_show_preview = true;
static bool s_mem_gray_zeros = true;
static bool s_mem_highlight_changes = true;
static uint32_t s_mem_selection_start = 0;
static uint32_t s_mem_selection_end = 0;
static int s_mem_edit_offset = -1;
static char s_mem_edit_buf[4] = "00";
static char s_mem_goto_buf[12] = "";
static char s_mem_find_buf[8] = "";
static int s_mem_scroll_to = -1;
static int s_mem_last_region_key = -1;
static int s_mem_last_cpu_focus = -1;
static uint8_t s_mem_prev[0x10000];
static uint8_t s_mem_prev_valid[0x10000];

static const char *mem_region_name(int region)
{
     switch (region)
     {
     case MEM_REGION_CPU:
          return "CPU Map";
     case MEM_REGION_ROM_BANK:
          return "ROM Bank";
     case MEM_REGION_FULL_ROM:
          return "Full ROM";
     case MEM_REGION_CART_RAM:
          return "Cart RAM";
     case MEM_REGION_WRAM:
          return "WRAM";
     case MEM_REGION_VRAM:
          return "VRAM";
     case MEM_REGION_OAM:
          return "OAM";
     case MEM_REGION_HRAM:
          return "HRAM";
     }
     return "?";
}

static uint32_t mem_region_base(struct gb *gb, int region)
{
     (void)gb;
     switch (region)
     {
     case MEM_REGION_ROM_BANK:
          return s_mem_rom_bank == 0 ? 0x0000u : 0x4000u;
     case MEM_REGION_CART_RAM:
          return 0xA000u;
     case MEM_REGION_WRAM:
          return 0xC000u;
     case MEM_REGION_VRAM:
          return 0x8000u;
     case MEM_REGION_OAM:
          return 0xFE00u;
     case MEM_REGION_HRAM:
          return 0xFF80u;
     default:
          return 0;
     }
}

static uint32_t mem_region_size(struct gb *gb, int region)
{
     switch (region)
     {
     case MEM_REGION_CPU:
          return 0x10000u;
     case MEM_REGION_ROM_BANK:
          return 0x4000u;
     case MEM_REGION_FULL_ROM:
          return gb->cart.rom_length;
     case MEM_REGION_CART_RAM:
          return gb->cart.ram_length;
     case MEM_REGION_WRAM:
          return 0x2000u;
     case MEM_REGION_VRAM:
          return gb->gbc ? 0x4000u : 0x2000u;
     case MEM_REGION_OAM:
          return 0xA0u;
     case MEM_REGION_HRAM:
          return 0x80u;
     }
     return 0;
}

static bool mem_region_writable(struct gb *gb, int region)
{
     switch (region)
     {
     case MEM_REGION_FULL_ROM:
     case MEM_REGION_ROM_BANK:
          return false;
     case MEM_REGION_CART_RAM:
          return gb->cart.ram != NULL && !gb->cart.ram_write_protected;
     default:
          return true;
     }
}

static uint8_t mem_region_read(struct gb *gb, int region, uint32_t offset)
{
     switch (region)
     {
     case MEM_REGION_CPU:
          return gb_memory_peekb(gb, (uint16_t)offset);
     case MEM_REGION_ROM_BANK:
     {
          uint32_t phys = (uint32_t)s_mem_rom_bank * 0x4000u + offset;
          return (gb->cart.rom && phys < gb->cart.rom_length) ? gb->cart.rom[phys] : 0xFF;
     }
     case MEM_REGION_FULL_ROM:
          return (gb->cart.rom && offset < gb->cart.rom_length) ? gb->cart.rom[offset] : 0xFF;
     case MEM_REGION_CART_RAM:
          return (gb->cart.ram && offset < gb->cart.ram_length) ? gb->cart.ram[offset] : 0xFF;
     case MEM_REGION_WRAM:
          return offset < 0x2000u ? gb_memory_peekb(gb, (uint16_t)(0xC000u + offset)) : 0xFF;
     case MEM_REGION_VRAM:
          return offset < sizeof(gb->vram) ? gb->vram[offset] : 0xFF;
     case MEM_REGION_OAM:
          return offset < 0xA0u ? gb_memory_peekb(gb, (uint16_t)(0xFE00u + offset)) : 0xFF;
     case MEM_REGION_HRAM:
          return offset < 0x80u ? gb_memory_peekb(gb, (uint16_t)(0xFF80u + offset)) : 0xFF;
     }
     return 0xFF;
}

static void mem_region_write(struct gb *gb, int region, uint32_t offset, uint8_t value)
{
     if (!mem_region_writable(gb, region))
          return;

     switch (region)
     {
     case MEM_REGION_CPU:
          gb_memory_writeb(gb, (uint16_t)offset, value);
          break;
     case MEM_REGION_CART_RAM:
          if (gb->cart.ram && offset < gb->cart.ram_length)
          {
               gb->cart.ram[offset] = value;
               gb->cart.dirty_ram = true;
          }
          break;
     case MEM_REGION_WRAM:
          if (offset < 0x2000u)
               gb_memory_writeb(gb, (uint16_t)(0xC000u + offset), value);
          break;
     case MEM_REGION_VRAM:
          if (offset < sizeof(gb->vram))
               gb->vram[offset] = value;
          break;
     case MEM_REGION_OAM:
          if (offset < 0xA0u)
               gb_memory_writeb(gb, (uint16_t)(0xFE00u + offset), value);
          break;
     case MEM_REGION_HRAM:
          if (offset < 0x80u)
               gb_memory_writeb(gb, (uint16_t)(0xFF80u + offset), value);
          break;
     default:
          break;
     }
}

static bool parse_hex_u32(const char *text, uint32_t *out)
{
     if (!text || !text[0])
          return false;
     char *end = NULL;
     unsigned long value = strtoul(text, &end, 16);
     if (end == text)
          return false;
     *out = (uint32_t)value;
     return true;
}

static void mem_format_display_addr(struct gb *gb, int region, uint32_t offset, char *out, size_t out_len)
{
     uint32_t display = mem_region_base(gb, region) + offset;
     uint32_t size = mem_region_size(gb, region);
     if (region == MEM_REGION_FULL_ROM || display > 0xFFFFu || size > 0x10000u)
          snprintf(out, out_len, "%05X", display);
     else
          snprintf(out, out_len, "%04X", display & 0xFFFFu);
}

static void mem_jump_to_display_addr(struct gb *gb, uint32_t display_addr)
{
     uint32_t base = mem_region_base(gb, s_mem_region);
     uint32_t size = mem_region_size(gb, s_mem_region);
     uint32_t offset = display_addr >= base ? display_addr - base : display_addr;
     if (size > 0 && offset >= size)
          offset = size - 1;
     s_mem_scroll_to = (int)offset;
     s_mem_selection_start = s_mem_selection_end = offset;
     if (s_mem_region == MEM_REGION_CPU)
          g_mem_addr = (int)display_addr;
}

static void mem_find_next(struct gb *gb)
{
     uint32_t wanted;
     uint32_t size = mem_region_size(gb, s_mem_region);
     if (!parse_hex_u32(s_mem_find_buf, &wanted) || size == 0)
          return;

     uint8_t byte = (uint8_t)wanted;
     uint32_t start = s_mem_selection_end + 1;
     for (uint32_t pass = 0; pass < 2; pass++)
     {
          uint32_t begin = pass == 0 ? start : 0;
          uint32_t end = pass == 0 ? size : start;
          for (uint32_t i = begin; i < end; i++)
          {
               if (mem_region_read(gb, s_mem_region, i) == byte)
               {
                    s_mem_selection_start = s_mem_selection_end = i;
                    s_mem_scroll_to = (int)i;
                    return;
               }
          }
     }
}

static void draw_memory_preview(struct gb *gb, uint32_t size)
{
     if (!s_mem_show_preview || size == 0)
          return;

     uint32_t pos = s_mem_selection_start < size ? s_mem_selection_start : size - 1;
     uint8_t b0 = mem_region_read(gb, s_mem_region, pos);
     uint8_t b1 = (pos + 1 < size) ? mem_region_read(gb, s_mem_region, pos + 1) : 0;
     uint8_t b2 = (pos + 2 < size) ? mem_region_read(gb, s_mem_region, pos + 2) : 0;
     uint8_t b3 = (pos + 3 < size) ? mem_region_read(gb, s_mem_region, pos + 3) : 0;
     uint16_t le16 = (uint16_t)(b0 | (b1 << 8));
     uint16_t be16 = (uint16_t)((b0 << 8) | b1);
     uint32_t le32 = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);

     ImGui::Separator();
     ImGui::TextColored(color_cyan, "Preview:");
     ImGui::SameLine();
     ImGui::Text("u8 $%02X  u16le $%04X  u16be $%04X  u32le $%08X", b0, le16, be16, le32);
}

static void draw_memory_editor(struct gb *gb)
{
     uint32_t size = mem_region_size(gb, s_mem_region);
     if (size == 0)
     {
          ImGui::TextColored(color_gray, "(regiao indisponivel)");
          return;
     }

     if (s_mem_region == MEM_REGION_CPU)
     {
          g_mem_addr &= 0xFFFF;
          if (g_mem_addr != s_mem_last_cpu_focus)
          {
               s_mem_selection_start = s_mem_selection_end = (uint32_t)g_mem_addr;
               s_mem_scroll_to = g_mem_addr;
               s_mem_last_cpu_focus = g_mem_addr;
          }
     }

     int region_key = s_mem_region * 100000 + s_mem_rom_bank;
     if (region_key != s_mem_last_region_key)
     {
          memset(s_mem_prev_valid, 0, sizeof(s_mem_prev_valid));
          s_mem_last_region_key = region_key;
          s_mem_edit_offset = -1;
          s_mem_selection_start = s_mem_selection_end = 0;
     }

     s_mem_bytes_per_row = s_mem_bytes_per_row == 8 || s_mem_bytes_per_row == 32 ? s_mem_bytes_per_row : 16;
     int total_rows = (int)((size + (uint32_t)s_mem_bytes_per_row - 1) / (uint32_t)s_mem_bytes_per_row);
     float line_h = ImGui::GetTextLineHeightWithSpacing();
     float footer_h = ImGui::GetFrameHeightWithSpacing() * 2.0f + (s_mem_show_preview ? line_h + 8.0f : 0.0f);

     ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(2.0f, 1.0f));
     ImGuiTableFlags flags = ImGuiTableFlags_SizingFixedFit |
                             ImGuiTableFlags_ScrollY |
                             ImGuiTableFlags_ScrollX |
                             ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_NoPadOuterX;
     if (ImGui::BeginTable("##gear_mem", s_mem_bytes_per_row + (s_mem_show_ascii ? 3 : 1),
                           flags, ImVec2(0, -footer_h)))
     {
          ImGui::TableSetupColumn("ADDR", ImGuiTableColumnFlags_WidthFixed, 62);
          for (int i = 0; i < s_mem_bytes_per_row; i++)
          {
               char label[16];
               snprintf(label, sizeof(label), "%02X", i);
               ImGui::TableSetupColumn(label, ImGuiTableColumnFlags_WidthFixed, 24);
          }
          if (s_mem_show_ascii)
          {
               ImGui::TableSetupColumn("|", ImGuiTableColumnFlags_WidthFixed, 12);
               ImGui::TableSetupColumn("ASCII", ImGuiTableColumnFlags_WidthFixed, (float)s_mem_bytes_per_row * 8.0f);
          }
          ImGui::TableSetupScrollFreeze(1, 1);
          ImGui::TableHeadersRow();

          if (s_mem_scroll_to >= 0)
          {
               ImGui::SetScrollY((float)(s_mem_scroll_to / s_mem_bytes_per_row) * line_h - ImGui::GetWindowHeight() * 0.45f);
               s_mem_scroll_to = -1;
          }

          ImGuiListClipper clipper;
          clipper.Begin(total_rows, line_h);
          while (clipper.Step())
          {
               for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
               {
                    uint32_t row_offset = (uint32_t)row * (uint32_t)s_mem_bytes_per_row;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    char row_addr[16];
                    mem_format_display_addr(gb, s_mem_region, row_offset, row_addr, sizeof(row_addr));
                    ImGui::TextColored(color_cyan, "%s", row_addr);

                    for (int col = 0; col < s_mem_bytes_per_row; col++)
                    {
                         uint32_t offset = row_offset + (uint32_t)col;
                         ImGui::TableSetColumnIndex(col + 1);
                         if (offset >= size)
                         {
                              ImGui::TextUnformatted("  ");
                              continue;
                         }

                         uint8_t value = mem_region_read(gb, s_mem_region, offset);
                         bool selected = offset >= s_mem_selection_start && offset <= s_mem_selection_end;
                         bool changed = s_mem_highlight_changes &&
                                        offset < sizeof(s_mem_prev) &&
                                        s_mem_prev_valid[offset] &&
                                        s_mem_prev[offset] != value;
                         bool pc_here = s_mem_region == MEM_REGION_CPU && offset == gb->cpu.pc;

                         ImGui::PushID((int)offset);
                         if (s_mem_edit_offset == (int)offset)
                         {
                              ImGui::SetNextItemWidth(24);
                              bool enter = ImGui::InputText("##edit", s_mem_edit_buf, sizeof(s_mem_edit_buf),
                                                            ImGuiInputTextFlags_CharsHexadecimal |
                                                                ImGuiInputTextFlags_CharsUppercase |
                                                                ImGuiInputTextFlags_AutoSelectAll |
                                                                ImGuiInputTextFlags_EnterReturnsTrue);
                              if (enter)
                              {
                                   uint32_t parsed;
                                   if (parse_hex_u32(s_mem_edit_buf, &parsed))
                                        mem_region_write(gb, s_mem_region, offset, (uint8_t)parsed);
                                   s_mem_edit_offset = -1;
                              }
                              if (ImGui::IsKeyPressed(ImGuiKey_Escape))
                                   s_mem_edit_offset = -1;
                         }
                         else
                         {
                              char byte_text[4];
                              snprintf(byte_text, sizeof(byte_text), "%02X", value);
                              ImVec4 text_col = color_white;
                              if (selected)
                                   text_col = color_cyan;
                              else if (changed)
                                   text_col = color_orange;
                              else if (pc_here)
                                   text_col = color_yellow;
                              else if (s_mem_gray_zeros && value == 0)
                                   text_col = color_gray;

                              ImGui::PushStyleColor(ImGuiCol_Text, text_col);
                              if (ImGui::Selectable(byte_text, selected, ImGuiSelectableFlags_AllowDoubleClick))
                              {
                                   s_mem_selection_start = s_mem_selection_end = offset;
                                   if (s_mem_region == MEM_REGION_CPU)
                                   {
                                        g_mem_addr = (int)offset;
                                        s_mem_last_cpu_focus = (int)offset;
                                   }
                                   if (ImGui::IsMouseDoubleClicked(0) && mem_region_writable(gb, s_mem_region))
                                   {
                                        s_mem_edit_offset = (int)offset;
                                        snprintf(s_mem_edit_buf, sizeof(s_mem_edit_buf), "%02X", value);
                                   }
                              }
                              ImGui::PopStyleColor();
                              if (ImGui::IsItemHovered())
                              {
                                   char tip_addr[16];
                                   mem_format_display_addr(gb, s_mem_region, offset, tip_addr, sizeof(tip_addr));
                                   ImGui::SetTooltip("%s $%s = $%02X", mem_region_name(s_mem_region),
                                                     tip_addr, value);
                              }
                         }
                         ImGui::PopID();

                         if (offset < sizeof(s_mem_prev))
                         {
                              s_mem_prev[offset] = value;
                              s_mem_prev_valid[offset] = 1;
                         }
                    }

                    if (s_mem_show_ascii)
                    {
                         ImGui::TableSetColumnIndex(s_mem_bytes_per_row + 1);
                         ImGui::TextColored(color_gray, "|");
                         ImGui::TableSetColumnIndex(s_mem_bytes_per_row + 2);
                         char ascii[40];
                         int n = 0;
                         for (int col = 0; col < s_mem_bytes_per_row && n < (int)sizeof(ascii) - 1; col++)
                         {
                              uint32_t offset = row_offset + (uint32_t)col;
                              uint8_t value = offset < size ? mem_region_read(gb, s_mem_region, offset) : 0;
                              ascii[n++] = (value >= 0x20 && value < 0x7F) ? (char)value : '.';
                         }
                         ascii[n] = '\0';
                         ImGui::TextColored(color_skyblue, "%s", ascii);
                    }
               }
          }
          ImGui::EndTable();
     }
     ImGui::PopStyleVar();

     ImGui::SetNextItemWidth(70);
     if (ImGui::InputTextWithHint("##mem_goto", "0000", s_mem_goto_buf, sizeof(s_mem_goto_buf),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase |
                                      ImGuiInputTextFlags_EnterReturnsTrue))
     {
          uint32_t addr;
          if (parse_hex_u32(s_mem_goto_buf, &addr))
               mem_jump_to_display_addr(gb, addr);
          s_mem_goto_buf[0] = 0;
     }
     ImGui::SameLine();
     if (ImGui::Button("GoTo"))
     {
          uint32_t addr;
          if (parse_hex_u32(s_mem_goto_buf, &addr))
               mem_jump_to_display_addr(gb, addr);
          s_mem_goto_buf[0] = 0;
     }
     ImGui::SameLine();
     ImGui::SetNextItemWidth(48);
     if (ImGui::InputTextWithHint("##mem_find", "00", s_mem_find_buf, sizeof(s_mem_find_buf),
                                  ImGuiInputTextFlags_CharsHexadecimal |
                                      ImGuiInputTextFlags_CharsUppercase |
                                      ImGuiInputTextFlags_EnterReturnsTrue))
          mem_find_next(gb);
     ImGui::SameLine();
     if (ImGui::Button("Find Next"))
          mem_find_next(gb);

     uint32_t sel0 = s_mem_selection_start < s_mem_selection_end ? s_mem_selection_start : s_mem_selection_end;
     uint32_t sel1 = s_mem_selection_start < s_mem_selection_end ? s_mem_selection_end : s_mem_selection_start;
     ImGui::SameLine();
     ImGui::TextColored(color_cyan, "REGION:");
     ImGui::SameLine();
     char region_start[16], region_end[16], sel_start[16], sel_end[16];
     mem_format_display_addr(gb, s_mem_region, 0, region_start, sizeof(region_start));
     mem_format_display_addr(gb, s_mem_region, size - 1, region_end, sizeof(region_end));
     mem_format_display_addr(gb, s_mem_region, sel0, sel_start, sizeof(sel_start));
     mem_format_display_addr(gb, s_mem_region, sel1, sel_end, sizeof(sel_end));
     ImGui::Text("$%s-$%s", region_start, region_end);
     ImGui::SameLine();
     ImGui::TextColored(color_cyan, "SELECTION:");
     ImGui::SameLine();
     if (sel0 == sel1)
          ImGui::Text("$%s", sel_start);
     else
          ImGui::Text("$%s-$%s", sel_start, sel_end);

     draw_memory_preview(gb, size);
}

void draw_panel_memory(struct gb *gb)
{
     struct gb_cart *cart = &gb->cart;

     ImGui::TextColored(color_cyan, "BANKS:");
     ImGui::SameLine();
     ImGui::TextColored(color_skyblue, "ROM");
     ImGui::SameLine(0, 2);
     ImGui::Text("$%02X/%u", cart->cur_rom_bank, cart->rom_banks ? cart->rom_banks - 1 : 0);
     ImGui::SameLine();
     ImGui::TextColored(color_skyblue, "RAM");
     ImGui::SameLine(0, 2);
     ImGui::Text("$%02X/%u", cart->cur_ram_bank, cart->ram_banks ? cart->ram_banks - 1 : 0);
     ImGui::SameLine();
     ImGui::TextColored(cart->ram_write_protected ? color_red : color_green,
                        cart->ram_write_protected ? "RAM OFF" : "RAM ON");
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "MBC %s", mbc_model_name(cart->model));

     ImGui::SameLine();
     int bytes_index = s_mem_bytes_per_row == 8 ? 0 : (s_mem_bytes_per_row == 32 ? 2 : 1);
     ImGui::SetNextItemWidth(82);
     if (ImGui::Combo("Bytes", &bytes_index, "8\0"
                                             "16\0"
                                             "32\0\0"))
          s_mem_bytes_per_row = bytes_index == 0 ? 8 : (bytes_index == 2 ? 32 : 16);

     ImGui::SameLine();
     ImGui::Checkbox("ASCII", &s_mem_show_ascii);
     ImGui::SameLine();
     ImGui::Checkbox("Preview", &s_mem_show_preview);
     ImGui::SameLine();
     ImGui::Checkbox("Gray 00", &s_mem_gray_zeros);
     ImGui::SameLine();
     ImGui::Checkbox("Changes", &s_mem_highlight_changes);

     if (ImGui::BeginTabBar("##mem_tabs"))
     {
          for (int region = 0; region < MEM_REGION_COUNT; region++)
          {
               if (region == MEM_REGION_CART_RAM && cart->ram_length == 0)
                    continue;
               if ((region == MEM_REGION_FULL_ROM || region == MEM_REGION_ROM_BANK) && !cart->rom)
                    continue;

               if (ImGui::BeginTabItem(mem_region_name(region)))
               {
                    s_mem_region = region;
                    g_mem_mode = (region == MEM_REGION_ROM_BANK) ? 1 : 0;
                    if (region == MEM_REGION_ROM_BANK)
                    {
                         int max_bank = cart->rom_banks > 0 ? (int)cart->rom_banks - 1 : 0;
                         ImGui::SetNextItemWidth(80);
                         ImGui::InputInt("Bank", &s_mem_rom_bank, 1, 16);
                         if (s_mem_rom_bank < 0)
                              s_mem_rom_bank = 0;
                         if (s_mem_rom_bank > max_bank)
                              s_mem_rom_bank = max_bank;
                         ImGui::SameLine();
                         ImGui::TextColored(color_gray, "physical $%05X-$%05X",
                                            s_mem_rom_bank * 0x4000,
                                            s_mem_rom_bank * 0x4000 + 0x3FFF);
                    }
                    draw_memory_editor(gb);
                    ImGui::EndTabItem();
               }
          }
          ImGui::EndTabBar();
     }
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Breakpoints                                        */
/* ────────────────────────────────────────────────────────── */

void draw_panel_breakpoints(struct gb *gb)
{
     struct gb_debug *dbg = &gb->debug;

     ImGui::SetNextItemWidth(84);
     ImGui::InputText("##bp_addr", s_bp_input, sizeof(s_bp_input),
                      ImGuiInputTextFlags_CharsHexadecimal |
                          ImGuiInputTextFlags_CharsUppercase |
                          ImGuiInputTextFlags_EnterReturnsTrue);
     ImGui::SameLine();
     if (ImGui::Button("Add"))
     {
          uint32_t addr;
          if (cpu_parse_hex_u32(s_bp_input, &addr))
               gb_debug_add_breakpoint(gb, (uint16_t)addr);
     }
     ImGui::SameLine();
     if (ImGui::Button("Remove All"))
          dbg->n_breakpoints = 0;
     ImGui::SameLine();
     if (ImGui::Button("Disable All"))
     {
          for (unsigned i = 0; i < dbg->n_breakpoints; i++)
               dbg->bp_enabled[i] = false;
     }

     ImGui::Separator();

     if (dbg->n_breakpoints == 0)
     {
          ImGui::TextColored(color_gray, "(nenhum breakpoint)");
          return;
     }

     int to_remove = -1;
     ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_SizingFixedFit |
                             ImGuiTableFlags_ScrollY;
     if (ImGui::BeginTable("##cpu_breakpoints", 5, flags, ImVec2(0, 150)))
     {
          ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28);
          ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 38);
          ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 58);
          ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24);
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableHeadersRow();

          for (unsigned i = 0; i < dbg->n_breakpoints; i++)
          {
               ImGui::PushID((int)i);
               bool hit = (gb->cpu.pc == dbg->breakpoints[i]);

               ImGui::TableNextRow();
               if (hit)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                           ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f, 0.05f, 0.05f, 1.0f)));

               ImGui::TableSetColumnIndex(0);
               bool en = dbg->bp_enabled[i];
               if (ImGui::Checkbox("##en", &en))
                    dbg->bp_enabled[i] = en;

               ImGui::TableSetColumnIndex(1);
               ImGui::TextColored(en ? color_red : color_gray, "EXEC");

               ImGui::TableSetColumnIndex(2);
               if (ImGui::Selectable("##addr", false, ImGuiSelectableFlags_SpanAllColumns))
               {
                    g_mem_mode = 0;
                    g_mem_addr = dbg->breakpoints[i];
               }
               ImGui::SameLine(0, 0);
               ImGui::TextColored(en ? color_cyan : color_gray, "%04X", dbg->breakpoints[i]);

               ImGui::TableSetColumnIndex(3);
               ImGui::TextColored(hit ? color_yellow : color_gray, hit ? "PC here" : "armed");

               ImGui::TableSetColumnIndex(4);
               if (ImGui::SmallButton("X"))
                    to_remove = (int)i;
               ImGui::PopID();
          }
          ImGui::EndTable();
     }
     if (to_remove >= 0)
          gb_debug_remove_breakpoint(gb, to_remove);
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Watchpoints                                        */
/* ────────────────────────────────────────────────────────── */

void draw_panel_watchpoints(struct gb *gb)
{
     struct gb_debug *dbg = &gb->debug;

     ImGui::SetNextItemWidth(84);
     ImGui::InputText("##wp_addr", s_wp_input, sizeof(s_wp_input),
                      ImGuiInputTextFlags_CharsHexadecimal |
                          ImGuiInputTextFlags_CharsUppercase |
                          ImGuiInputTextFlags_EnterReturnsTrue);
     ImGui::SameLine();
     ImGui::SetNextItemWidth(80);
     ImGui::Combo("##wp_type", &s_wp_type, "Read\0Write\0Both\0\0");
     ImGui::SameLine();
     if (ImGui::Button("Add"))
     {
          uint32_t addr;
          if (cpu_parse_hex_u32(s_wp_input, &addr))
          {
               gb_watchpoint_type type = (gb_watchpoint_type)(s_wp_type + 1);
               gb_debug_add_watchpoint(gb, (uint16_t)addr, type);
          }
     }
     ImGui::SameLine();
     if (ImGui::Button("Remove All"))
          dbg->n_watchpoints = 0;
     ImGui::SameLine();
     if (ImGui::Button("Disable All"))
     {
          for (unsigned i = 0; i < dbg->n_watchpoints; i++)
               dbg->watchpoints[i].enabled = false;
     }

     ImGui::Separator();

     if (dbg->n_watchpoints == 0)
     {
          ImGui::TextColored(color_gray, "(nenhum watchpoint)");
          return;
     }

     int to_remove = -1;
     ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_BordersInnerV |
                             ImGuiTableFlags_SizingFixedFit |
                             ImGuiTableFlags_ScrollY;
     if (ImGui::BeginTable("##cpu_watchpoints", 5, flags, ImVec2(0, 150)))
     {
          ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28);
          ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 38);
          ImGui::TableSetupColumn("Addr", ImGuiTableColumnFlags_WidthFixed, 58);
          ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24);
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableHeadersRow();

          for (unsigned i = 0; i < dbg->n_watchpoints; i++)
          {
               ImGui::PushID((int)(1000 + i));
               const char *type_str = "?";
               if (dbg->watchpoints[i].type == GB_WATCHPOINT_READ)
                    type_str = "R";
               else if (dbg->watchpoints[i].type == GB_WATCHPOINT_WRITE)
                    type_str = "W";
               else if (dbg->watchpoints[i].type == GB_WATCHPOINT_BOTH)
                    type_str = "R/W";

               ImGui::TableNextRow();
               ImGui::TableSetColumnIndex(0);
               bool en = dbg->watchpoints[i].enabled;
               if (ImGui::Checkbox("##en_wp", &en))
                    dbg->watchpoints[i].enabled = en;

               ImGui::TableSetColumnIndex(1);
               ImGui::TextColored(en ? color_orange : color_gray, "%s", type_str);

               ImGui::TableSetColumnIndex(2);
               if (ImGui::Selectable("##addr", false, ImGuiSelectableFlags_SpanAllColumns))
               {
                    g_mem_mode = 0;
                    g_mem_addr = dbg->watchpoints[i].addr;
               }
               ImGui::SameLine(0, 0);
               ImGui::TextColored(en ? color_cyan : color_gray, "%04X", dbg->watchpoints[i].addr);

               ImGui::TableSetColumnIndex(3);
               ImGui::TextColored(color_gray, "$%02X", gb_memory_peekb(gb, dbg->watchpoints[i].addr));

               ImGui::TableSetColumnIndex(4);
               if (ImGui::SmallButton("X##wp"))
                    to_remove = (int)i;
               ImGui::PopID();
          }
          ImGui::EndTable();
     }
     if (to_remove >= 0)
          gb_debug_remove_watchpoint(gb, to_remove);
}

/* ────────────────────────────────────────────────────────── */
/* Painel: OAM (sprites)                                      */
/* ────────────────────────────────────────────────────────── */

static void update_oam_texture(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;
     const uint8_t *vram = gb->vram;
     int sprite_h = gpu->tall_sprites ? 16 : 8;

     memset(s_oam_buf, 0x22, sizeof(s_oam_buf));

     for (int s = 0; s < 40; s++)
     {
          int off = s * 4;
          int tile = gpu->oam[off + 2];
          int flags_b = gpu->oam[off + 3];
          bool flip_x = (flags_b >> 5) & 1;
          bool flip_y = (flags_b >> 6) & 1;
          uint8_t pal = (flags_b & 0x10) ? gpu->obp1 : gpu->obp0;
          int tile_top = gpu->tall_sprites ? (tile & ~1) : tile;

          for (int y = 0; y < sprite_h; y++)
          {
               int src_y = flip_y ? (sprite_h - 1 - y) : y;
               int tile_idx = tile_top + (src_y / 8);
               int row_in_tile = src_y % 8;
               unsigned tile_off = (unsigned)tile_idx * 16u + (unsigned)row_in_tile * 2u;

               for (int x = 0; x < 8; x++)
               {
                    int src_x = flip_x ? (7 - x) : x;
                    int bit = 7 - src_x;
                    int lsb = (vram[tile_off + 0] >> bit) & 1;
                    int msb = (vram[tile_off + 1] >> bit) & 1;
                    int raw = (msb << 1) | lsb;

                    uint32_t pixel = (raw == 0) ? 0xFF333333u : dmg_apply(pal, raw);
                    s_oam_buf[y * OAM_ATLAS_W + s * 8 + x] = pixel;
               }
          }
     }

     glBindTexture(GL_TEXTURE_2D, s_oam_tex);
     glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, OAM_ATLAS_W, OAM_ATLAS_H,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_oam_buf);
     glBindTexture(GL_TEXTURE_2D, 0);
}

void draw_panel_oam(struct gb *gb)
{
     struct gb_gpu *gpu = &gb->gpu;

     ImGuiTableFlags flags =
         ImGuiTableFlags_BordersOuter |
         ImGuiTableFlags_BordersInnerV |
         ImGuiTableFlags_RowBg |
         ImGuiTableFlags_ScrollY;

     update_oam_texture(gb);

     int sprite_h = gpu->tall_sprites ? 16 : 8;
     float prev_w = 16.f;
     float prev_h = sprite_h * 2.f;

     if (!ImGui::BeginTable("oam", 8, flags, ImVec2(0, 0)))
          return;

     ImGui::TableSetupScrollFreeze(0, 1);
     ImGui::TableSetupColumn("Spr", ImGuiTableColumnFlags_WidthFixed, prev_w + 4);
     ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 28);
     ImGui::TableSetupColumn("Y", ImGuiTableColumnFlags_WidthFixed, 40);
     ImGui::TableSetupColumn("X", ImGuiTableColumnFlags_WidthFixed, 40);
     ImGui::TableSetupColumn("Tile", ImGuiTableColumnFlags_WidthFixed, 48);
     ImGui::TableSetupColumn("Flags", ImGuiTableColumnFlags_WidthFixed, 50);
     ImGui::TableSetupColumn("Paleta", ImGuiTableColumnFlags_WidthFixed, 50);
     ImGui::TableSetupColumn("Vis.", ImGuiTableColumnFlags_WidthFixed, 36);
     ImGui::TableHeadersRow();

     for (int i = 0; i < 40; i++)
     {
          int off = i * 4;
          int y_raw = gpu->oam[off + 0];
          int x_raw = gpu->oam[off + 1];
          int tile = gpu->oam[off + 2];
          int flags_b = gpu->oam[off + 3];
          int y = y_raw - 16;
          int x = x_raw - 8;
          bool visible = (y_raw > 0 && y_raw < (144 + sprite_h) && x_raw > 0 && x_raw < 168);

          ImGui::TableNextRow(0, prev_h);

          ImGui::TableSetColumnIndex(0);
          float u0 = (float)(i * 8) / (float)OAM_ATLAS_W;
          float u1 = (float)(i * 8 + 8) / (float)OAM_ATLAS_W;
          float v1 = (float)sprite_h / (float)OAM_ATLAS_H;
          ImGui::Image((ImTextureID)(intptr_t)s_oam_tex,
                       ImVec2(prev_w, prev_h), ImVec2(u0, 0.f), ImVec2(u1, v1));

          ImGui::TableSetColumnIndex(1);
          ImGui::Text("%d", i);
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%d", y);
          ImGui::TableSetColumnIndex(3);
          ImGui::Text("%d", x);
          ImGui::TableSetColumnIndex(4);
          ImGui::Text("0x%02X", tile);
          ImGui::TableSetColumnIndex(5);
          ImGui::Text("%s%s%s",
                      (flags_b & 0x40) ? "Y" : "-",
                      (flags_b & 0x20) ? "X" : "-",
                      (flags_b & 0x80) ? "B" : "-");
          ImGui::TableSetColumnIndex(6);
          ImGui::Text("OBP%d", (flags_b & 0x10) ? 1 : 0);
          ImGui::TableSetColumnIndex(7);
          if (visible)
               ImGui::TextColored(color_green, "sim");
          else
               ImGui::TextColored(color_gray, "nao");
     }

     ImGui::EndTable();
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Profiler                                           */
/* ────────────────────────────────────────────────────────── */

static void update_heatmap_texture(struct gb *gb)
{
     const uint32_t *hm = gb->debug.exec_heatmap;
     uint32_t max_val = 1;
     for (int i = 0; i < 65536; i++)
          if (hm[i] > max_val)
               max_val = hm[i];

     float log_max = logf(1.0f + (float)max_val);

     for (int addr = 0; addr < 65536; addr++)
     {
          int x = addr & 0xFF;
          int y = addr >> 8;

          if (hm[addr] == 0)
          {
               s_heatmap_buf[y * EXEC_MAP_W + x] = 0xFF111111u;
          }
          else
          {
               float t = logf(1.0f + (float)hm[addr]) / log_max;
               uint8_t r, g, b;
               if (t < 0.25f)
               {
                    float tt = t / 0.25f;
                    r = 0;
                    g = (uint8_t)(tt * 255);
                    b = 255;
               }
               else if (t < 0.5f)
               {
                    float tt = (t - 0.25f) / 0.25f;
                    r = 0;
                    g = 255;
                    b = (uint8_t)((1 - tt) * 255);
               }
               else if (t < 0.75f)
               {
                    float tt = (t - 0.5f) / 0.25f;
                    r = (uint8_t)(tt * 255);
                    g = 255;
                    b = 0;
               }
               else
               {
                    float tt = (t - 0.75f) / 0.25f;
                    r = 255;
                    g = (uint8_t)((1 - tt) * 255);
                    b = 0;
               }
               s_heatmap_buf[y * EXEC_MAP_W + x] =
                   0xFF000000u | ((uint32_t)b << 16) | ((uint32_t)g << 8) | r;
          }
     }

     glBindTexture(GL_TEXTURE_2D, s_heatmap_tex);
     glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, EXEC_MAP_W, EXEC_MAP_H,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_heatmap_buf);
     glBindTexture(GL_TEXTURE_2D, 0);
}

static void update_coverage_texture(struct gb *gb)
{
     const uint8_t *cov = gb->debug.exec_coverage;
     for (int addr = 0; addr < 65536; addr++)
     {
          int x = addr & 0xFF;
          int y = addr >> 8;
          bool covered = (cov[addr >> 3] >> (addr & 7)) & 1;
          s_coverage_buf[y * EXEC_MAP_W + x] = covered ? 0xFF00CC00u : 0xFF222222u;
     }
     glBindTexture(GL_TEXTURE_2D, s_coverage_tex);
     glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, EXEC_MAP_W, EXEC_MAP_H,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_coverage_buf);
     glBindTexture(GL_TEXTURE_2D, 0);
}

struct OpcodeEntry
{
     int idx;
     uint32_t count;
};

static int cmp_opcode_desc(const void *a, const void *b)
{
     uint32_t ca = ((const OpcodeEntry *)a)->count;
     uint32_t cb = ((const OpcodeEntry *)b)->count;
     return (cb > ca) - (cb < ca);
}

void draw_panel_profiler(struct gb *gb)
{
     struct gb_debug *dbg = &gb->debug;

     if (ImGui::Button("Reset"))
          gb_debug_reset_profiler(gb);
     ImGui::SameLine();
     ImGui::TextColored(color_gray, "%llu instru\xc3\xa7\xc3\xb5"
                                    "es totais",
                        (unsigned long long)dbg->instruction_count);
     ImGui::Separator();

     if (!ImGui::BeginTabBar("##prof_tabs"))
          return;

     if (ImGui::BeginTabItem("Opcodes"))
     {
          uint64_t total_hits = 0;
          for (int i = 0; i < 512; i++)
               total_hits += dbg->opcode_hits[i];

          static OpcodeEntry sorted[512];
          for (int i = 0; i < 512; i++)
          {
               sorted[i].idx = i;
               sorted[i].count = dbg->opcode_hits[i];
          }
          qsort(sorted, 512, sizeof(OpcodeEntry), cmp_opcode_desc);

          ImGuiTableFlags tf = ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY;

          if (ImGui::BeginTable("##op_tbl", 4, tf))
          {
               ImGui::TableSetupScrollFreeze(0, 1);
               ImGui::TableSetupColumn("Opcode", ImGuiTableColumnFlags_WidthFixed, 76);
               ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 70);
               ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 52);
               ImGui::TableSetupColumn("Bar", ImGuiTableColumnFlags_WidthStretch);
               ImGui::TableHeadersRow();

               for (int i = 0; i < 512 && sorted[i].count > 0; i++)
               {
                    int idx = sorted[i].idx;
                    uint32_t cnt = sorted[i].count;
                    float pct = (total_hits > 0) ? (100.0f * cnt / (float)total_hits) : 0.0f;

                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (idx >= 256)
                         ImGui::TextColored(color_yellow, "CB %02X", idx - 256);
                    else
                         ImGui::TextColored(color_cyan, "   %02X", idx);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%u", cnt);
                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%.1f%%", pct);
                    ImGui::TableSetColumnIndex(3);
                    ImGui::ProgressBar(pct / 100.0f, ImVec2(-1.f, 0.f), "");
               }
               ImGui::EndTable();
          }
          ImGui::EndTabItem();
     }

     if (ImGui::BeginTabItem("Heatmap"))
     {
          update_heatmap_texture(gb);
          ImGui::TextColored(color_gray, "Execu\xc3\xa7\xc3\xb5"
                                         "es por endere\xc3\xa7o  (X=byte baixo, Y=byte alto)");
          ImGui::Separator();

          float avail_w = ImGui::GetContentRegionAvail().x;
          float scale = avail_w / (float)EXEC_MAP_W;
          if (scale > 2.0f)
               scale = 2.0f;
          if (scale < 1.0f)
               scale = 1.0f;

          ImVec2 img_size(EXEC_MAP_W * scale, EXEC_MAP_H * scale);
          ImVec2 cursor = ImGui::GetCursorScreenPos();
          ImGui::Image((ImTextureID)(intptr_t)s_heatmap_tex, img_size);

          if (ImGui::IsItemHovered())
          {
               ImVec2 mouse = ImGui::GetMousePos();
               int px = (int)((mouse.x - cursor.x) / scale);
               int py = (int)((mouse.y - cursor.y) / scale);
               if (px >= 0 && px < 256 && py >= 0 && py < 256)
               {
                    uint16_t addr = (uint16_t)((py << 8) | px);
                    ImGui::SetTooltip("0x%04X  hits: %u\nClique \xe2\x86\x92 Memory Viewer",
                                      addr, dbg->exec_heatmap[addr]);
                    if (ImGui::IsMouseClicked(0))
                    {
                         g_mem_addr = addr;
                         g_mem_mode = 0;
                    }
               }
          }

          ImGui::Spacing();
          ImGui::TextColored(color_gray, "Frio");
          ImGui::SameLine();
          ImVec2 bar_pos = ImGui::GetCursorScreenPos();
          const float BAR_W = 120.f, BAR_H = 10.f;
          ImDrawList *dl = ImGui::GetWindowDrawList();
          for (int i = 0; i < (int)BAR_W; i++)
          {
               float t = (float)i / BAR_W;
               uint8_t r2, g2, b2;
               if (t < 0.25f)
               {
                    float tt = t / 0.25f;
                    r2 = 0;
                    g2 = (uint8_t)(tt * 255);
                    b2 = 255;
               }
               else if (t < 0.5f)
               {
                    float tt = (t - 0.25f) / 0.25f;
                    r2 = 0;
                    g2 = 255;
                    b2 = (uint8_t)((1 - tt) * 255);
               }
               else if (t < 0.75f)
               {
                    float tt = (t - 0.5f) / 0.25f;
                    r2 = (uint8_t)(tt * 255);
                    g2 = 255;
                    b2 = 0;
               }
               else
               {
                    float tt = (t - 0.75f) / 0.25f;
                    r2 = 255;
                    g2 = (uint8_t)((1 - tt) * 255);
                    b2 = 0;
               }
               dl->AddRectFilled(ImVec2(bar_pos.x + i, bar_pos.y),
                                 ImVec2(bar_pos.x + i + 1, bar_pos.y + BAR_H),
                                 IM_COL32(r2, g2, b2, 255));
          }
          ImGui::Dummy(ImVec2(BAR_W, BAR_H));
          ImGui::SameLine();
          ImGui::TextColored(color_red, "Quente");
          ImGui::EndTabItem();
     }

     if (ImGui::BeginTabItem("Coverage"))
     {
          update_coverage_texture(gb);

          int covered = 0;
          for (int i = 0; i < 8192; i++)
          {
               uint8_t b = gb->debug.exec_coverage[i];
               while (b)
               {
                    covered += b & 1;
                    b >>= 1;
               }
          }
          float pct = covered * 100.0f / 65536.0f;

          ImGui::TextColored(color_green, "%d / 65536", covered);
          ImGui::SameLine();
          ImGui::TextColored(color_gray, "endere\xc3\xa7os executados (%.1f%%)", pct);
          ImGui::ProgressBar(pct / 100.0f, ImVec2(-1.f, 0.f));
          ImGui::Separator();

          float avail_w = ImGui::GetContentRegionAvail().x;
          float scale = avail_w / (float)EXEC_MAP_W;
          if (scale > 2.0f)
               scale = 2.0f;
          if (scale < 1.0f)
               scale = 1.0f;

          ImVec2 img_size(EXEC_MAP_W * scale, EXEC_MAP_H * scale);
          ImVec2 cursor = ImGui::GetCursorScreenPos();
          ImGui::Image((ImTextureID)(intptr_t)s_coverage_tex, img_size);

          if (ImGui::IsItemHovered())
          {
               ImVec2 mouse = ImGui::GetMousePos();
               int px = (int)((mouse.x - cursor.x) / scale);
               int py = (int)((mouse.y - cursor.y) / scale);
               if (px >= 0 && px < 256 && py >= 0 && py < 256)
               {
                    uint16_t addr = (uint16_t)((py << 8) | px);
                    bool cov = (gb->debug.exec_coverage[addr >> 3] >> (addr & 7)) & 1;
                    ImGui::SetTooltip("0x%04X  %s\nClique \xe2\x86\x92 Memory Viewer",
                                      addr, cov ? "EXECUTADO" : "n\xc3\xa3o executado");
                    if (ImGui::IsMouseClicked(0))
                    {
                         g_mem_addr = addr;
                         g_mem_mode = 0;
                    }
               }
          }
          ImGui::EndTabItem();
     }

     ImGui::EndTabBar();
}

/* ────────────────────────────────────────────────────────── */
/* Helpers internos para draw_panel_hw_viz / cpu_viz           */
/* ────────────────────────────────────────────────────────── */

static ImU32 viz_col(ImU32 active, float fade)
{
     if (fade <= 0.0f)
          return IM_COL32(80, 80, 80, 180);
     float t = fade > 1.0f ? 1.0f : fade;
     uint8_t ar = (active >> IM_COL32_R_SHIFT) & 0xFF;
     uint8_t ag = (active >> IM_COL32_G_SHIFT) & 0xFF;
     uint8_t ab = (active >> IM_COL32_B_SHIFT) & 0xFF;
     uint8_t r = (uint8_t)(80 + (ar - 80) * t);
     uint8_t g = (uint8_t)(80 + (ag - 80) * t);
     uint8_t b = (uint8_t)(80 + (ab - 80) * t);
     uint8_t a = (uint8_t)(180 + (255 - 180) * t);
     return IM_COL32(r, g, b, a);
}

static void viz_block(ImDrawList *dl, ImVec2 tl, ImVec2 br,
                      const char *title, const char *body,
                      ImU32 border_col, float rounding = 6.0f)
{
     dl->AddRectFilled(tl, br, IM_COL32(28, 30, 36, 230), rounding);
     dl->AddRect(tl, br, border_col, rounding, 0, 2.0f);

     float tx = tl.x + 8.0f;
     float ty = tl.y + 6.0f;
     dl->AddText(ImVec2(tx, ty), IM_COL32(220, 220, 255, 255), title);
     if (body && body[0])
          dl->AddText(ImVec2(tx, ty + 16.0f), IM_COL32(180, 200, 180, 220), body);
}

static void viz_arrow(ImDrawList *dl, ImVec2 from, ImVec2 to, ImU32 col, float thickness = 2.0f)
{
     dl->AddLine(from, to, col, thickness);
     /* arrow head */
     float dx = to.x - from.x;
     float dy = to.y - from.y;
     float len = sqrtf(dx * dx + dy * dy);
     if (len < 1.0f)
          return;
     dx /= len;
     dy /= len;
     float sz = 7.0f;
     dl->AddTriangleFilled(
         to,
         ImVec2(to.x - sz * dx + sz * 0.5f * dy,
                to.y - sz * dy - sz * 0.5f * dx),
         ImVec2(to.x - sz * dx - sz * 0.5f * dy,
                to.y - sz * dy + sz * 0.5f * dx),
         col);
}

/* ────────────────────────────────────────────────────────── */
/* Painel: Hardware System Diagram                             */
/* ────────────────────────────────────────────────────────── */

static const char *cart_model_str(enum gb_cart_model m)
{
     switch (m)
     {
     case GB_CART_SIMPLE:
          return "ROM only";
     case GB_CART_MBC1:
          return "MBC1";
     case GB_CART_MBC2:
          return "MBC2";
     case GB_CART_MBC3:
          return "MBC3";
     case GB_CART_MBC5:
          return "MBC5";
     case GB_CART_MBC7:
          return "MBC7";
     case GB_CART_HUC1:
          return "HuC1";
     case GB_CART_HUC3:
          return "HuC3";
     default:
          return "???";
     }
}

static const char *gpu_mode_str(uint8_t mode)
{
     switch (mode)
     {
     case 0:
          return "HBlank(0)";
     case 1:
          return "VBlank(1)";
     case 2:
          return "OAM(2)  ";
     case 3:
          return "Draw(3) ";
     default:
          return "???";
     }
}

void draw_panel_hw_viz(struct gb *gb)
{
     float dt = ImGui::GetIO().DeltaTime;

     /* ── Decay dos fade timers ── */
     struct gb_sys_viz *sv = &gb->debug.sys_viz;
     float decay = dt * 8.0f;
     sv->fade_cpu_rom = (sv->fade_cpu_rom > decay) ? sv->fade_cpu_rom - decay : 0.0f;
     sv->fade_cpu_wram = (sv->fade_cpu_wram > decay) ? sv->fade_cpu_wram - decay : 0.0f;
     sv->fade_cpu_vram = (sv->fade_cpu_vram > decay) ? sv->fade_cpu_vram - decay : 0.0f;
     sv->fade_cpu_oam = (sv->fade_cpu_oam > decay) ? sv->fade_cpu_oam - decay : 0.0f;
     sv->fade_cpu_io = (sv->fade_cpu_io > decay) ? sv->fade_cpu_io - decay : 0.0f;
     sv->fade_dma_oam = (sv->fade_dma_oam > decay) ? sv->fade_dma_oam - decay : 0.0f;
     sv->fade_ppu_vram = (sv->fade_ppu_vram > decay) ? sv->fade_ppu_vram - decay : 0.0f;
     sv->fade_irq_cpu = (sv->fade_irq_cpu > decay) ? sv->fade_irq_cpu - decay : 0.0f;
     sv->fade_apu = (sv->fade_apu > decay) ? sv->fade_apu - decay : 0.0f;

     ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
     ImVec2 canvas_size = ImGui::GetContentRegionAvail();
     if (canvas_size.x < 600.0f)
          canvas_size.x = 600.0f;
     if (canvas_size.y < 500.0f)
          canvas_size.y = 500.0f;

     /* Invisible widget para reservar o espaço */
     ImGui::InvisibleButton("##hw_canvas", canvas_size);
     ImDrawList *dl = ImGui::GetWindowDrawList();

     float ox = canvas_pos.x;
     float oy = canvas_pos.y;

     /* ── Posições dos blocos (layout fixo, escalado) ── */
     float sx = canvas_size.x / 720.0f;
     float sy = canvas_size.y / 520.0f;
     float sc = sx < sy ? sx : sy;

     auto P = [&](float x, float y) -> ImVec2
     { return ImVec2(ox + x * sc, oy + y * sc); };
     auto R = [&](float x, float y, float w, float h, const char *title, const char *body,
                  ImU32 border)
     {
          viz_block(dl, P(x, y), P(x + w, y + h), title, body, border);
     };

     /* Cartridge */
     char cart_title[32], cart_body[48];
     snprintf(cart_title, sizeof(cart_title), "Cartridge");
     if (gb->cart.rom)
          snprintf(cart_body, sizeof(cart_body), "%s  ROM:%u  RAM:%u",
                   cart_model_str(gb->cart.model),
                   gb->cart.cur_rom_bank,
                   gb->cart.cur_ram_bank);
     else
          snprintf(cart_body, sizeof(cart_body), "(sem ROM)");
     ImU32 cart_border = viz_col(IM_COL32(255, 220, 60, 255), sv->fade_cpu_rom);
     R(10, 20, 140, 52, cart_title, cart_body, cart_border);

     /* CPU */
     char cpu_body[96];
     snprintf(cpu_body, sizeof(cpu_body),
              "PC:%04X  SP:%04X  IME:%s\n"
              "A:%02X  BC:%02X%02X  DE:%02X%02X  HL:%02X%02X",
              gb->cpu.pc, gb->cpu.sp,
              gb->cpu.irq_enable ? "ON" : "OFF",
              gb->cpu.a,
              gb->cpu.b, gb->cpu.c,
              gb->cpu.d, gb->cpu.e,
              gb->cpu.h, gb->cpu.l);
     ImU32 cpu_border = IM_COL32(80, 180, 255, 220);
     viz_block(dl, P(190, 10), P(540, 82), "CPU  LR35902", cpu_body, cpu_border);

     /* IRQ */
     char irq_body[32];
     snprintf(irq_body, sizeof(irq_body), "IF:%02X  IE:%02X",
              gb->irq.irq_flags, gb->irq.irq_enable);
     ImU32 irq_border = viz_col(IM_COL32(180, 100, 255, 255), sv->fade_irq_cpu);
     R(560, 10, 150, 52, "IRQ Controller", irq_body, irq_border);

     /* WRAM */
     char wram_body[24];
     snprintf(wram_body, sizeof(wram_body), "%s  bk:%u",
              gb->gbc ? "32 KiB" : "8 KiB", gb->iram_high_bank);
     ImU32 wram_border = viz_col(IM_COL32(60, 200, 120, 255), sv->fade_cpu_wram);
     R(10, 120, 130, 52, "WRAM", wram_body, wram_border);

     /* VRAM */
     char vram_body[24];
     snprintf(vram_body, sizeof(vram_body), "%s  bk:%u",
              gb->gbc ? "16 KiB" : "8 KiB", gb->vram_high_bank ? 1 : 0);
     ImU32 vram_border = viz_col(IM_COL32(60, 160, 255, 255),
                                 sv->fade_cpu_vram > sv->fade_ppu_vram
                                     ? sv->fade_cpu_vram
                                     : sv->fade_ppu_vram);
     R(200, 120, 130, 52, "VRAM", vram_body, vram_border);

     /* IO Regs */
     ImU32 io_border = viz_col(IM_COL32(255, 160, 60, 255), sv->fade_cpu_io);
     R(380, 120, 130, 52, "IO Regs", "$FF00-$FF7F", io_border);

     /* PPU/GPU */
     char ppu_body[48];
     uint8_t ppu_mode = gb_gpu_get_mode(gb);
     snprintf(ppu_body, sizeof(ppu_body), "Mode:%s  LY:%3d\nLCDC:%02X  STAT:%02X",
              gpu_mode_str(ppu_mode), gb->gpu.ly,
              gb_gpu_get_lcdc(gb), gb_gpu_get_lcd_stat(gb));
     ImU32 ppu_mode_colors[4] = {
         IM_COL32(100, 200, 100, 255), /* 0 HBlank */
         IM_COL32(255, 200, 60, 255),  /* 1 VBlank */
         IM_COL32(60, 180, 255, 255),  /* 2 OAM Scan */
         IM_COL32(255, 100, 100, 255), /* 3 Drawing */
     };
     ImU32 ppu_border = gb->gpu.master_enable
                            ? ppu_mode_colors[ppu_mode & 3]
                            : IM_COL32(80, 80, 80, 200);
     R(200, 240, 160, 68, "PPU / GPU", ppu_body, ppu_border);

     /* OAM */
     char oam_body[24];
     snprintf(oam_body, sizeof(oam_body), "40 sprites  OBJ:%s",
              gb->gpu.sprite_enable ? "ON" : "OFF");
     ImU32 oam_border = viz_col(IM_COL32(255, 140, 200, 255),
                                sv->fade_cpu_oam > sv->fade_dma_oam
                                    ? sv->fade_cpu_oam
                                    : sv->fade_dma_oam);
     R(10, 240, 140, 52, "OAM", oam_body, oam_border);

     /* DMA Engine */
     char dma_body[32];
     if (gb->dma.running)
          snprintf(dma_body, sizeof(dma_body), "pos:%u  src:%04X",
                   gb->dma.position, gb->dma.source);
     else
          snprintf(dma_body, sizeof(dma_body), "idle");
     ImU32 dma_border = viz_col(IM_COL32(255, 140, 200, 255), sv->fade_dma_oam);
     R(10, 350, 130, 52, "DMA Engine", dma_body, dma_border);

     /* APU */
     char apu_body[40];
     snprintf(apu_body, sizeof(apu_body), "CH1:%s CH2:%s CH3:%s CH4:%s",
              gb->spu.nr1.running ? "\xe2\x96\xa0" : "\xe2\x96\xa1",
              gb->spu.nr2.running ? "\xe2\x96\xa0" : "\xe2\x96\xa1",
              gb->spu.nr3.running ? "\xe2\x96\xa0" : "\xe2\x96\xa1",
              gb->spu.nr4.running ? "\xe2\x96\xa0" : "\xe2\x96\xa1");
     ImU32 apu_border = viz_col(IM_COL32(255, 200, 100, 255), sv->fade_apu);
     R(420, 240, 150, 52, "APU", apu_body, apu_border);

     /* LCD */
     char lcd_body[24];
     snprintf(lcd_body, sizeof(lcd_body), "160x144  %s",
              gb->gpu.master_enable ? "ON" : "OFF");
     R(200, 370, 160, 52, "LCD", lcd_body, IM_COL32(120, 200, 120, 200));

     /* ── Linhas de barramento ── */

     /* Cartridge ↔ CPU (address+data bus) */
     {
          ImU32 c = viz_col(IM_COL32(255, 220, 60, 255), sv->fade_cpu_rom);
          ImVec2 cart_r = P(150, 46);
          ImVec2 cpu_l = P(190, 46);
          dl->AddLine(cart_r, cpu_l, c, 2.5f);
     }

     /* CPU ↓ WRAM */
     {
          ImU32 c = viz_col(IM_COL32(60, 200, 120, 255), sv->fade_cpu_wram);
          viz_arrow(dl, P(250, 82), P(75, 120), c);
     }

     /* CPU ↓ VRAM */
     {
          ImU32 c = viz_col(IM_COL32(60, 160, 255, 255), sv->fade_cpu_vram);
          viz_arrow(dl, P(310, 82), P(265, 120), c);
     }

     /* CPU ↓ IO */
     {
          ImU32 c = viz_col(IM_COL32(255, 160, 60, 255), sv->fade_cpu_io);
          viz_arrow(dl, P(400, 82), P(445, 120), c);
     }

     /* CPU → IRQ */
     {
          ImU32 c = viz_col(IM_COL32(180, 100, 255, 255), sv->fade_irq_cpu);
          dl->AddLine(P(540, 46), P(560, 46), c, 2.5f);
     }

     /* VRAM ↓ PPU */
     {
          ImU32 c = viz_col(IM_COL32(60, 160, 255, 255), sv->fade_ppu_vram);
          viz_arrow(dl, P(265, 172), P(265, 240), c);
     }

     /* PPU ↓ LCD */
     {
          ImU32 c = gb->gpu.master_enable
                        ? IM_COL32(120, 240, 120, 220)
                        : IM_COL32(80, 80, 80, 150);
          viz_arrow(dl, P(280, 308), P(280, 370), c);
     }

     /* OAM ↔ PPU */
     {
          ImU32 c = viz_col(IM_COL32(255, 140, 200, 255),
                            sv->fade_cpu_oam > sv->fade_dma_oam
                                ? sv->fade_cpu_oam
                                : sv->fade_dma_oam);
          dl->AddLine(P(150, 266), P(200, 266), c, 2.5f);
     }

     /* DMA → OAM */
     {
          ImU32 c = viz_col(IM_COL32(255, 140, 200, 255), sv->fade_dma_oam);
          viz_arrow(dl, P(75, 350), P(75, 292), c);
     }

     /* IO Regs → APU (APU é controlado via registradores de IO $FF10-$FF3F) */
     {
          ImU32 c = viz_col(IM_COL32(255, 200, 100, 255), sv->fade_cpu_io);
          /* IO Regs bottom → APU top */
          ImVec2 io_b = P(445, 172);
          ImVec2 apu_t = P(495, 240);
          dl->AddLine(io_b, ImVec2(io_b.x, apu_t.y - 10.0f * sc), c, 2.0f);
          viz_arrow(dl, ImVec2(io_b.x, apu_t.y - 10.0f * sc), apu_t, c);
     }

     /* ── PPU scanline progress bar ── */
     {
          float bar_x = ox + 200.0f * sc;
          float bar_y = oy + 460.0f * sc;
          float bar_w = 160.0f * sc;
          float bar_h = 14.0f * sc;
          dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h),
                            IM_COL32(30, 30, 40, 220), 3.0f);
          /* 154 total scanlines: 0-143 visible, 144-153 VBlank */
          float pct = gb->gpu.ly / 154.0f;
          ImU32 bar_col = gb->gpu.ly < 144
                              ? ppu_mode_colors[ppu_mode & 3]
                              : IM_COL32(255, 200, 60, 200);
          dl->AddRectFilled(ImVec2(bar_x, bar_y),
                            ImVec2(bar_x + bar_w * pct, bar_y + bar_h),
                            bar_col, 3.0f);
          char bar_label[32];
          snprintf(bar_label, sizeof(bar_label), "LY %3d / 154", gb->gpu.ly);
          dl->AddText(ImVec2(bar_x + 4.0f, bar_y + 1.0f),
                      IM_COL32(220, 220, 220, 255), bar_label);
     }

     /* ── Legend ── */
     {
          float lx = ox + 560.0f * sc;
          float ly = oy + 120.0f * sc;
          float lh = 18.0f * sc;
          struct
          {
               ImU32 col;
               const char *label;
          } entries[] = {
              {IM_COL32(255, 220, 60, 255), "ROM/bus"},
              {IM_COL32(60, 200, 120, 255), "WRAM"},
              {IM_COL32(60, 160, 255, 255), "VRAM"},
              {IM_COL32(255, 160, 60, 255), "IO"},
              {IM_COL32(255, 140, 200, 255), "OAM/DMA"},
              {IM_COL32(180, 100, 255, 255), "IRQ"},
          };
          dl->AddText(ImVec2(lx, ly - lh), IM_COL32(160, 160, 160, 200), "Bus activity:");
          for (int i = 0; i < 6; ++i)
          {
               dl->AddRectFilled(ImVec2(lx, ly + i * lh + 2.0f),
                                 ImVec2(lx + 12.0f, ly + i * lh + 12.0f),
                                 entries[i].col, 2.0f);
               dl->AddText(ImVec2(lx + 16.0f, ly + i * lh),
                           IM_COL32(200, 200, 200, 220), entries[i].label);
          }
     }
}

/* ────────────────────────────────────────────────────────── */
/* Painel: CPU Datapath (Ângulo 1)                            */
/* ────────────────────────────────────────────────────────── */

void draw_panel_cpu_viz(struct gb *gb)
{
     float dt = ImGui::GetIO().DeltaTime;
     struct gb_cpu_viz *cv = &gb->debug.cpu_viz;

     /* Decay activity fade */
     float decay = dt * 5.0f;
     cv->activity_fade = (cv->activity_fade > decay) ? cv->activity_fade - decay : 0.0f;

     const float design_w = 760.0f;
     const float design_h = 430.0f;
     ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
     ImVec2 canvas_size = ImGui::GetContentRegionAvail();
     if (canvas_size.x < design_w)
          canvas_size.x = design_w;
     if (canvas_size.y < design_h)
          canvas_size.y = design_h;

     ImGui::InvisibleButton("##cpu_canvas", canvas_size);
     ImDrawList *dl = ImGui::GetWindowDrawList();

     float ox = canvas_pos.x + (canvas_size.x - design_w) * 0.5f;
     float oy = canvas_pos.y + 6.0f;

     auto P = [&](float x, float y) -> ImVec2
     { return ImVec2(ox + x, oy + y); };

     float fade = cv->activity_fade;

     /* ALU name table */
     static const char *alu_names[] = {
         "idle", "ADD", "SUB", "AND", "OR", "XOR", "CP", "INC", "DEC", "SHIFT/ROT", "BIT"};
     uint8_t alu_idx = cv->alu_op < 11 ? cv->alu_op : 0;
     bool alu_active = (alu_idx > 0) && fade > 0.0f;

     /* Register name table (GB_VIZ_REG_* indices) */
     static const char *reg_names[] = {
         "---", "A", "B", "C", "D", "E", "H", "L", "HL", "BC", "DE", "SP", "imm8", "imm16", "(HL)"};

     dl->AddRectFilled(P(0, 0), P(design_w, design_h),
                       IM_COL32(15, 17, 23, 215), 8.0f);
     dl->AddRect(P(0, 0), P(design_w, design_h),
                 IM_COL32(70, 76, 92, 160), 8.0f, 0, 1.0f);

     auto centered_text = [&](ImVec2 tl, ImVec2 br, const char *text, ImU32 col, float dy)
     {
          ImVec2 ts = ImGui::CalcTextSize(text);
          dl->AddText(ImVec2(tl.x + (br.x - tl.x - ts.x) * 0.5f, tl.y + dy), col, text);
     };

     auto label_text = [&](float x, float y, const char *text)
     {
          dl->AddText(P(x, y), IM_COL32(120, 124, 145, 190), text);
     };

     /* Helper: reg cell color — yellow src, green dst, orange src==dst */
     auto reg_col = [&](uint8_t reg_id, bool is_src, bool is_dst) -> ImU32
     {
          if (!fade || reg_id == GB_VIZ_REG_NONE || reg_id == GB_VIZ_REG_IMM8 ||
              reg_id == GB_VIZ_REG_IMM16 || reg_id == GB_VIZ_REG_MEM)
               return IM_COL32(55, 60, 75, 220);
          if (is_src && is_dst)
               return viz_col(IM_COL32(255, 160, 40, 255), fade);
          if (is_src)
               return viz_col(IM_COL32(255, 220, 60, 255), fade);
          if (is_dst)
               return viz_col(IM_COL32(60, 220, 100, 255), fade);
          return IM_COL32(55, 60, 75, 220);
     };

     /* Helper: draw a single register cell */
     auto reg_cell = [&](float x, float y, float w, float h,
                         const char *name, uint8_t val,
                         uint8_t reg_id)
     {
          bool is_src = (cv->src == reg_id) && fade > 0.0f;
          bool is_dst = (cv->dst == reg_id) && fade > 0.0f;
          ImU32 border = reg_col(reg_id, is_src, is_dst);
          ImVec2 tl = P(x, y), br = P(x + w, y + h);
          dl->AddRectFilled(tl, br, IM_COL32(28, 32, 42, 230), 4.0f);
          dl->AddRect(tl, br, border, 4.0f, 0, (is_src || is_dst) ? 2.0f : 1.0f);
          /* name label */
          dl->AddText(ImVec2(tl.x + 4.0f, tl.y + 3.0f),
                      IM_COL32(140, 140, 160, 200), name);
          /* value */
          char vbuf[8];
          snprintf(vbuf, sizeof(vbuf), "%02X", val);
          float vw = ImGui::CalcTextSize(vbuf).x;
          dl->AddText(ImVec2(br.x - vw - 4.0f, tl.y + 28.0f),
                      (is_src || is_dst) ? IM_COL32(255, 255, 200, 255) : IM_COL32(180, 200, 180, 220),
                      vbuf);
     };

     /* ════════════════════════════════════════════════════
        ROW 1: IR | IDU | pipeline stages
        ════════════════════════════════════════════════════ */

     /* IR — Instruction Register */
     {
          /* Classify opcode into a category string */
          const char *instr_type =
              (cv->opcode >= 0x40 && cv->opcode <= 0x7F && cv->opcode != 0x76) ? "LD" : (alu_idx > 0)                                                                                                                                                        ? "ALU"
                                                                                    : (cv->opcode == 0xCB)                                                                                                                                                   ? "BIT"
                                                                                    : (cv->opcode == 0x76 || cv->opcode == 0x10)                                                                                                                             ? "CTRL"
                                                                                    : (cv->opcode == 0xC3 || cv->opcode == 0xC9 || (cv->opcode & 0xC7) == 0xC0 || (cv->opcode & 0xC7) == 0xC2 || (cv->opcode & 0xC7) == 0xC4 || (cv->opcode & 0xC7) == 0xC7) ? "JMP"
                                                                                                                                                                                                                                                             : "MISC";
          char ir_body[24];
          snprintf(ir_body, sizeof(ir_body), "$%02X  [%s]", cv->opcode, instr_type);
          ImU32 ir_col = fade > 0.0f ? viz_col(IM_COL32(200, 160, 255, 255), fade)
                                     : IM_COL32(70, 70, 90, 200);
          viz_block(dl, P(14, 14), P(160, 58), "IR", ir_body, ir_col);
     }

     /* IDU — Increment/Decrement Unit */
     {
          /* IDU is active when PC/SP changes (any instruction really, but visually
             highlight when a branch or SP operation just happened) */
          bool idu_active = fade > 0.0f;
          ImU32 idu_col = idu_active ? viz_col(IM_COL32(100, 200, 255, 255), fade)
                                     : IM_COL32(60, 70, 85, 200);
          char idu_body[16];
          snprintf(idu_body, sizeof(idu_body), "PC"
                                               "\xc2\xb1"
                                               "1  SP"
                                               "\xc2\xb1"
                                               "1");
          viz_block(dl, P(172, 14), P(300, 58), "IDU", idu_body, idu_col);
     }

     /* Pipeline stage pills */
     {
          static const char *stages[] = {"FETCH", "DECODE", "EXECUTE", "IRQ"};
          static const ImU32 stage_cols[] = {
              IM_COL32(255, 220, 60, 255),
              IM_COL32(60, 200, 255, 255),
              IM_COL32(60, 220, 100, 255),
              IM_COL32(255, 100, 100, 255),
          };
          float sx2 = 318.0f;
          for (int i = 0; i < 4; ++i)
          {
               bool active = (cv->stage == (uint8_t)i) && fade > 0.0f;
               ImU32 col = active ? stage_cols[i] : IM_COL32(65, 65, 75, 180);
               float bw = (i == 2) ? 98.0f : 88.0f;
               ImVec2 tl = P(sx2, 18), br = P(sx2 + bw, 48);
               dl->AddRectFilled(tl, br, active ? IM_COL32(30, 36, 48, 240) : IM_COL32(20, 22, 28, 180), 4.0f);
               dl->AddRect(tl, br, col, 4.0f, 0, active ? 2.0f : 1.0f);
               centered_text(tl, br, stages[i], col, 7.0f);
               if (i < 3)
                    dl->AddLine(P(sx2 + bw, 33), P(sx2 + bw + 14, 33), IM_COL32(100, 100, 110, 160), 1.5f);
               sx2 += bw + 14.0f;
          }
     }

     /* ════════════════════════════════════════════════════
        ROW 2: ALU with live inputs | FLAGS | IME
        ════════════════════════════════════════════════════ */

     /* ALU block — shows A op B → result */
     {
          ImU32 alu_border = alu_active
                                 ? viz_col(IM_COL32(60, 220, 100, 255), fade)
                                 : IM_COL32(65, 72, 65, 200);
          viz_block(dl, P(14, 78), P(364, 166), "ALU", nullptr, alu_border);

          /* Internal ALU lanes */
          float ax = ox + 28.0f;
          float ay = oy + 106.0f;
          float lh = 20.0f;

          /* Lane: A input */
          dl->AddText(ImVec2(ax, ay), IM_COL32(160, 160, 180, 200), "A:");
          char abuf[8];
          snprintf(abuf, sizeof(abuf), "%02X", cv->alu_a);
          dl->AddText(ImVec2(ax + 18.0f, ay),
                      alu_active ? IM_COL32(255, 240, 120, 255) : IM_COL32(160, 180, 160, 200), abuf);

          /* Op */
          dl->AddText(ImVec2(ax + 62.0f, ay),
                      alu_active ? viz_col(IM_COL32(120, 255, 160, 255), fade) : IM_COL32(80, 90, 80, 180),
                      alu_names[alu_idx]);

          /* B input */
          dl->AddText(ImVec2(ax + 168.0f, ay), IM_COL32(160, 160, 180, 200), "B:");
          char bbuf[8];
          snprintf(bbuf, sizeof(bbuf), "%02X", cv->alu_b);
          dl->AddText(ImVec2(ax + 186.0f, ay),
                      alu_active ? IM_COL32(255, 220, 80, 255) : IM_COL32(160, 180, 160, 200), bbuf);

          /* Arrow + result */
          dl->AddText(ImVec2(ax + 230.0f, ay), IM_COL32(120, 120, 130, 200), "->");
          char rbuf[8];
          snprintf(rbuf, sizeof(rbuf), "%02X", cv->alu_result);
          dl->AddText(ImVec2(ax + 252.0f, ay),
                      alu_active ? IM_COL32(100, 255, 160, 255) : IM_COL32(140, 160, 140, 180), rbuf);

          /* src/dst register names below */
          if (cv->src < 15 || cv->dst < 15)
          {
               char opstr[40];
               const char *sn = (cv->src < 15) ? reg_names[cv->src] : "?";
               const char *dn = (cv->dst < 15) ? reg_names[cv->dst] : "?";
               snprintf(opstr, sizeof(opstr), "%s -> %s", sn, dn);
               dl->AddText(ImVec2(ax, ay + lh),
                           fade > 0.0f ? IM_COL32(180, 180, 200, 200) : IM_COL32(80, 80, 90, 160),
                           opstr);
          }
     }

     /* FLAGS — individual cells Z N H C */
     {
          struct
          {
               const char *name;
               bool val;
               ImU32 active_col;
               uint8_t bit;
          } flags[] = {
              {"Z", gb->cpu.f_z, IM_COL32(80, 220, 255, 255), 3},
              {"N", gb->cpu.f_n, IM_COL32(255, 160, 60, 255), 2},
              {"H", gb->cpu.f_h, IM_COL32(255, 230, 60, 255), 1},
              {"C", gb->cpu.f_c, IM_COL32(220, 220, 220, 255), 0},
          };
          float fx = 386.0f;
          dl->AddRectFilled(P(fx, 78), P(fx + 176, 166), IM_COL32(22, 24, 32, 220), 5.0f);
          dl->AddRect(P(fx, 78), P(fx + 176, 166), IM_COL32(70, 70, 86, 180), 5.0f, 0, 1.0f);
          dl->AddText(P(fx + 10, 86), IM_COL32(140, 140, 160, 180), "FLAGS");
          for (int i = 0; i < 4; ++i)
          {
               bool changed = (cv->flags_changed >> flags[i].bit) & 1;
               ImU32 border = changed && fade > 0.0f
                                  ? viz_col(flags[i].active_col, fade)
                                  : (flags[i].val ? IM_COL32(120, 120, 130, 200) : IM_COL32(55, 55, 65, 180));
               ImVec2 tl = P(fx + 10 + i * 40.0f, 112), br = P(fx + 44 + i * 40.0f, 150);
               ImU32 bg = flags[i].val
                              ? IM_COL32(40, 45, 55, 230)
                              : IM_COL32(22, 24, 30, 200);
               dl->AddRectFilled(tl, br, bg, 4.0f);
               dl->AddRect(tl, br, border, 4.0f, 0, changed && fade > 0.0f ? 2.0f : 1.0f);
               /* flag letter */
               float tw = ImGui::CalcTextSize(flags[i].name).x;
               dl->AddText(ImVec2(tl.x + (br.x - tl.x - tw) * 0.5f, tl.y + 2.0f),
                           flags[i].val ? flags[i].active_col : IM_COL32(80, 80, 95, 200),
                           flags[i].name);
               /* 0/1 value */
               const char *vstr = flags[i].val ? "1" : "0";
               float vw = ImGui::CalcTextSize(vstr).x;
               dl->AddText(ImVec2(tl.x + (br.x - tl.x - vw) * 0.5f, tl.y + 20.0f),
                           flags[i].val ? IM_COL32(220, 220, 180, 220) : IM_COL32(70, 70, 80, 180),
                           vstr);
          }
     }

     /* IME latch */
     {
          bool ime = gb->cpu.irq_enable;
          ImU32 ime_col = ime ? IM_COL32(100, 220, 100, 255) : IM_COL32(80, 80, 90, 200);
          ImVec2 tl = P(580, 78), br = P(746, 166);
          dl->AddRectFilled(tl, br, IM_COL32(22, 28, 22, 220), 4.0f);
          dl->AddRect(tl, br, ime_col, 4.0f, 0, ime ? 2.0f : 1.0f);
          dl->AddText(ImVec2(tl.x + 10.0f, tl.y + 8.0f), IM_COL32(130, 130, 150, 200), "IME latch");
          const char *ime_str = ime ? "ON" : "OFF";
          centered_text(tl, br, ime_str, ime_col, 45.0f);
     }

     /* ════════════════════════════════════════════════════
        ROW 3: Acc A | TMP W | TMP Z | Register File cells
        ════════════════════════════════════════════════════ */

     label_text(14, 188, "Internal registers");

     /* Accumulator A */
     reg_cell(14, 210, 76, 58, "Acc A", gb->cpu.a, GB_VIZ_REG_A);

     /* TMP W and Z — internal latches (not directly addressable; show addr_bus hi/lo) */
     {
          uint8_t w_val = (uint8_t)(cv->addr_bus >> 8);
          uint8_t z_val = (uint8_t)(cv->addr_bus & 0xFF);
          ImU32 tmp_col = fade > 0.0f
                              ? viz_col(IM_COL32(180, 140, 255, 255), fade * 0.6f)
                              : IM_COL32(50, 50, 68, 200);
          auto tmp_cell = [&](float x, float y, const char *name, uint8_t val)
          {
               ImVec2 tl = P(x, y), br = P(x + 66, y + 58);
               dl->AddRectFilled(tl, br, IM_COL32(26, 26, 38, 220), 4.0f);
               dl->AddRect(tl, br, tmp_col, 4.0f, 0, 1.0f);
               dl->AddText(ImVec2(tl.x + 4.0f, tl.y + 3.0f), IM_COL32(120, 100, 160, 180), name);
               char vb[8];
               snprintf(vb, sizeof(vb), "%02X", val);
               float vw = ImGui::CalcTextSize(vb).x;
               dl->AddText(ImVec2(br.x - vw - 6.0f, tl.y + 28.0f),
                           IM_COL32(160, 140, 200, 200), vb);
          };
          tmp_cell(102, 210, "W", w_val);
          tmp_cell(176, 210, "Z", z_val);
     }

     /* Register file: B C D E H L — each its own cell */
     {
          struct
          {
               uint8_t id;
               const char *name;
               uint8_t val;
          } regs[] = {
              {GB_VIZ_REG_B, "B", gb->cpu.b},
              {GB_VIZ_REG_C, "C", gb->cpu.c},
              {GB_VIZ_REG_D, "D", gb->cpu.d},
              {GB_VIZ_REG_E, "E", gb->cpu.e},
              {GB_VIZ_REG_H, "H", gb->cpu.h},
              {GB_VIZ_REG_L, "L", gb->cpu.l},
          };
          float rx = 262.0f;
          for (int i = 0; i < 6; ++i)
          {
               reg_cell(rx, 210, 76, 58, regs[i].name, regs[i].val, regs[i].id);
               rx += 82.0f;
          }
     }

     /* ════════════════════════════════════════════════════
        ROW 4: PC | SP | M-cycles
        ════════════════════════════════════════════════════ */

     label_text(14, 288, "Address registers");

     /* PC */
     {
          ImU32 pc_col = viz_col(IM_COL32(255, 220, 60, 255), fade);
          char pc_body[12];
          snprintf(pc_body, sizeof(pc_body), "$%04X", gb->cpu.pc);
          viz_block(dl, P(14, 308), P(250, 356), "PC  (Program Counter)", pc_body, pc_col);
     }

     /* SP */
     {
          bool sp_active = (cv->src == GB_VIZ_REG_SP || cv->dst == GB_VIZ_REG_SP) && fade > 0.0f;
          ImU32 sp_col = sp_active
                             ? viz_col(IM_COL32(180, 140, 255, 255), fade)
                             : IM_COL32(80, 80, 100, 200);
          char sp_body[12];
          snprintf(sp_body, sizeof(sp_body), "$%04X", gb->cpu.sp);
          viz_block(dl, P(266, 308), P(502, 356), "SP  (Stack Pointer)", sp_body, sp_col);
     }

     /* M-cycles badge */
     {
          char mc_buf[20];
          snprintf(mc_buf, sizeof(mc_buf), "M-cycles: %u", (unsigned)cv->m_cycles);
          ImU32 mc_col = cv->m_cycles > 0 ? IM_COL32(160, 200, 160, 220) : IM_COL32(80, 80, 90, 180);
          ImVec2 tl = P(520, 308), br = P(746, 356);
          dl->AddRectFilled(tl, br, IM_COL32(22, 24, 30, 220), 4.0f);
          dl->AddRect(tl, br, IM_COL32(70, 80, 90, 180), 4.0f, 0, 1.0f);
          centered_text(tl, br, mc_buf, mc_col, 15.0f);
     }

     /* ════════════════════════════════════════════════════
        ROW 5: Address/Data Bus visual
        ════════════════════════════════════════════════════ */

     {
          ImU32 bus_col = viz_col(IM_COL32(255, 220, 60, 255), fade);
          float bx1 = ox + 14.0f, bx2 = ox + 746.0f;
          float by = oy + 374.0f, bh = 20.0f;
          /* track */
          dl->AddRectFilled(ImVec2(bx1, by), ImVec2(bx2, by + bh), IM_COL32(22, 24, 30, 210), 3.0f);
          /* fill proportional to addr value (16-bit → width) */
          float fill = (float)cv->addr_bus / 65535.0f;
          dl->AddRectFilled(ImVec2(bx1, by), ImVec2(bx1 + (bx2 - bx1) * fill, by + bh),
                            IM_COL32(60, 50, 0, 80), 3.0f);
          /* border */
          dl->AddRect(ImVec2(bx1, by), ImVec2(bx2, by + bh), bus_col, 3.0f, 0, 1.5f);
          /* label */
          char alabel[32];
          snprintf(alabel, sizeof(alabel), "Address Bus  $%04X", cv->addr_bus);
          dl->AddText(ImVec2(bx1 + 6.0f, by + 2.0f),
                      fade > 0.0f ? IM_COL32(255, 230, 120, 255) : IM_COL32(120, 110, 60, 200),
                      alabel);
     }

     {
          ImU32 bus_col = cv->bus_write
                              ? viz_col(IM_COL32(255, 120, 60, 255), fade)
                              : viz_col(IM_COL32(60, 200, 120, 255), fade);
          float bx1 = ox + 14.0f, bx2 = ox + 746.0f;
          float by = oy + 400.0f, bh = 20.0f;
          dl->AddRectFilled(ImVec2(bx1, by), ImVec2(bx2, by + bh), IM_COL32(22, 24, 30, 210), 3.0f);
          float fill = (float)cv->data_bus / 255.0f;
          dl->AddRectFilled(ImVec2(bx1, by), ImVec2(bx1 + (bx2 - bx1) * fill, by + bh),
                            cv->bus_write ? IM_COL32(60, 25, 0, 80) : IM_COL32(0, 50, 25, 80), 3.0f);
          dl->AddRect(ImVec2(bx1, by), ImVec2(bx2, by + bh), bus_col, 3.0f, 0, 1.5f);
          char dlabel[40];
          snprintf(dlabel, sizeof(dlabel), "Data Bus  $%02X  %s",
                   cv->data_bus, cv->bus_write ? "<- WR" : "RD ->");
          dl->AddText(ImVec2(bx1 + 6.0f, by + 2.0f),
                      fade > 0.0f ? IM_COL32(200, 255, 200, 255) : IM_COL32(60, 120, 80, 200),
                      dlabel);
     }

     /* Mnemonic strip + CPU state badges */
     {
          char strip[64];
          snprintf(strip, sizeof(strip), "$%02X  %s",
                   cv->opcode, cv->mnemonic[0] ? cv->mnemonic : "---");
          ImVec2 tl = P(14, 62);
          dl->AddText(tl,
                      fade > 0.0f ? IM_COL32(255, 240, 160, 255) : IM_COL32(130, 130, 140, 200),
                      strip);

          float fx = ox + 580.0f;
          float fy = oy + 54.0f;
          auto badge = [&](const char *label, bool active, ImU32 active_col)
          {
               ImU32 bg = active ? active_col : IM_COL32(38, 40, 50, 200);
               float tw = ImGui::CalcTextSize(label).x + 10.0f;
               dl->AddRectFilled(ImVec2(fx, fy), ImVec2(fx + tw, fy + 18.0f), bg, 3.0f);
               dl->AddText(ImVec2(fx + 5.0f, fy + 1.5f),
                           active ? IM_COL32(20, 20, 20, 255) : IM_COL32(110, 110, 120, 200),
                           label);
               fx += tw + 6.0f;
          };
          badge("HALT", gb->cpu.halted, IM_COL32(255, 200, 60, 255));
          badge("STOP", gb->cpu.stopped, IM_COL32(255, 100, 60, 255));
          badge("IME", gb->cpu.irq_enable, IM_COL32(100, 220, 100, 255));
          badge("2x spd", gb->double_speed, IM_COL32(60, 180, 255, 255));
     }
}

/* ======================================================================
 * Transistor Die Viewer
 * ====================================================================== */

/* Layer colors (ABGR for ImGui IM_COL32) */
static const ImU32 s_layer_colors[] = {
    IM_COL32(120, 180, 255, 180), /* metal1  - blue  */
    IM_COL32(255, 120, 120, 180), /* poly    - red   */
    IM_COL32(100, 220, 100, 180), /* nactive - green */
    IM_COL32(220, 160, 60, 180),  /* pactive - amber */
    IM_COL32(80, 160, 255, 220),  /* ntrans  - bright blue */
    IM_COL32(255, 80, 80, 220),   /* ptrans  - bright red  */
};

static const char *s_layer_names[] = {
    "Metal",
    "Poly",
    "N-Active",
    "P-Active",
    "N-Trans",
    "P-Trans",
};

/* Persistent panel state */
static Sm83ViewTransform s_die_view;
static Sm83ViewCache     s_die_cache;   /* updated once per frame before draw loops */
static Sm83Selection s_die_sel;
static Sm83SignalOverlay s_die_overlay;
static unsigned int s_layer_mask = SM83_VIEW_LAYER_ALL;
static bool s_die_view_init = false;

/* Performance counters shown in the info bar */
static int s_drawn_arcs = 0;
static int s_drawn_trans = 0;

/* ── Netlist Sim state (Fase 6) ── */
static Sm83NetlistSim s_netlist_sim;
static bool s_sim_view_init = false;

/* Connected-arc highlight: list of arc indices to draw highlighted */
/* Flat boolean lookup arrays for net highlight (arc and node).
 * Allocated once, never freed; zeroed on each rebuild. */
static uint8_t *s_arc_highlight_flags  = nullptr;
static uint8_t *s_node_highlight_flags = nullptr;
static int      s_highlight_flags_cap  = 0; /* tracks SM83_ARC_COUNT at alloc time */

static void rebuild_highlight(void)
{
    if (s_highlight_flags_cap < SM83_ARC_COUNT)
    {
        delete[] s_arc_highlight_flags;
        delete[] s_node_highlight_flags;
        s_arc_highlight_flags  = new uint8_t[SM83_ARC_COUNT]();
        s_node_highlight_flags = new uint8_t[SM83_NODE_COUNT]();
        s_highlight_flags_cap  = SM83_ARC_COUNT;
    }
    else
    {
        memset(s_arc_highlight_flags,  0, SM83_ARC_COUNT);
        memset(s_node_highlight_flags, 0, SM83_NODE_COUNT);
    }

    if (s_die_sel.type != SM83_SEL_NODE || s_die_sel.index < 0)
        return;

    sm83_net_flood(s_die_sel.index, s_arc_highlight_flags, s_node_highlight_flags);
}

void draw_panel_transistor_viz(struct gb *gb)
{
     /* Initialize transform and sim once */
     if (!s_die_view_init)
     {
          sm83_view_fit(&s_die_view);
          s_die_sel.type = SM83_SEL_NONE;
          s_die_sel.index = -1;
          sm83_overlay_init(&s_die_overlay);
          s_die_view_init = true;
     }
     if (!s_sim_view_init)
     {
          sm83_sim_init(&s_netlist_sim);
          s_sim_view_init = true;
     }

     float dt = ImGui::GetIO().DeltaTime;
     sm83_overlay_update(&s_die_overlay, gb);
     sm83_overlay_tick(&s_die_overlay, dt);

     /* Run sim step when enabled (only if paused or stepping) */
     bool gb_paused = gb && gb->debug.state == GB_DEBUG_PAUSED;
     if (s_netlist_sim.enabled && s_netlist_sim.initialized && gb_paused)
     {
          sm83_sim_seed_from_gb(&s_netlist_sim, gb);
          sm83_sim_step(&s_netlist_sim, 16);
     }

     /* ── Layer toggles toolbar ── */
     ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 4));
     for (int i = 0; i < 6; i++)
     {
          unsigned int bit = 1u << i;
          bool active = (s_layer_mask & bit) != 0;
          if (active)
               ImGui::PushStyleColor(ImGuiCol_Button, s_layer_colors[i]);
          else
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 60, 200));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                active ? (ImU32)(s_layer_colors[i] | IM_COL32(0, 0, 0, 55)) : IM_COL32(70, 70, 80, 200));
          if (ImGui::Button(s_layer_names[i]))
               s_layer_mask ^= bit;
          ImGui::PopStyleColor(2);
          ImGui::SameLine();
     }
     /* Emulator Overlay toggle */
     {
          bool ov_active = (s_layer_mask & SM83_VIEW_LAYER_OVERLAY) != 0;
          if (ov_active)
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(255, 220, 60, 200));
          else
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 60, 200));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(200, 180, 50, 200));
          if (ImGui::Button("Overlay"))
               s_layer_mask ^= SM83_VIEW_LAYER_OVERLAY;
          ImGui::PopStyleColor(2);
     }
     ImGui::SameLine();
     /* Netlist Sim toggle — clearly separate from Emulator Overlay */
     {
          bool sim_on = s_netlist_sim.enabled;
          if (sim_on)
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(100, 255, 180, 200));
          else
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 60, 200));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(80, 200, 140, 200));
          if (ImGui::Button("Netlist Sim [EXP]"))
          {
               s_netlist_sim.enabled = !s_netlist_sim.enabled;
               if (!s_netlist_sim.enabled)
                    sm83_sim_reset(&s_netlist_sim);
          }
          ImGui::PopStyleColor(2);
          if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Experimental transistor-level simulation.\n"
                                 "Runs only while paused. Not authoritative.");
     }
     ImGui::SameLine();
     ImGui::Text("  zoom:%.2fx", s_die_view.zoom);
     ImGui::PopStyleVar();

     /* ── Canvas ── */
     ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
     ImVec2 canvas_size = ImGui::GetContentRegionAvail();
     if (canvas_size.x < 400.0f)
          canvas_size.x = 400.0f;
     if (canvas_size.y < 300.0f)
          canvas_size.y = 300.0f;

     /* Update transform with current canvas */
     s_die_view.canvas_x = canvas_pos.x;
     s_die_view.canvas_y = canvas_pos.y;
     s_die_view.canvas_w = canvas_size.x;
     s_die_view.canvas_h = canvas_size.y;

     ImGui::InvisibleButton("##die_canvas", canvas_size,
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
     bool hovered = ImGui::IsItemHovered();
     bool active_drag = ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left);

     /* Pan with left-drag */
     if (active_drag)
     {
          ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
          ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
          float scale_x, scale_y;
          sm83_view_screen_scale(&s_die_view, &scale_x, &scale_y);
          s_die_view.pan_x -= delta.x / scale_x;
          s_die_view.pan_y -= delta.y / scale_y;
     }

     /* Zoom with scroll */
     if (hovered)
     {
          float wheel = ImGui::GetIO().MouseWheel;
          if (wheel != 0.0f)
          {
               ImVec2 mouse = ImGui::GetMousePos();
               float mx = mouse.x, my = mouse.y;
               float nx_before, ny_before;
               sm83_screen_to_die(&s_die_view, mx, my, &nx_before, &ny_before);

               float factor = (wheel > 0.0f) ? 1.15f : (1.0f / 1.15f);
               s_die_view.zoom *= factor;
               if (s_die_view.zoom < 0.5f)
                    s_die_view.zoom = 0.5f;
               if (s_die_view.zoom > 500.0f)
                    s_die_view.zoom = 500.0f;

               float nx_after, ny_after;
               sm83_screen_to_die(&s_die_view, mx, my, &nx_after, &ny_after);
               s_die_view.pan_x += nx_before - nx_after;
               s_die_view.pan_y += ny_before - ny_after;
          }
     }

     /* Hit test on click */
     if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !active_drag)
     {
          ImVec2 mp = ImGui::GetMousePos();
          sm83_hit_test(&s_die_view, mp.x, mp.y, 8.0f, s_layer_mask, &s_die_sel);
          rebuild_highlight();
     }

     ImDrawList *dl = ImGui::GetWindowDrawList();

     /* Background */
     dl->AddRectFilled(canvas_pos,
                       ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                       IM_COL32(18, 18, 24, 255));
     dl->AddRect(canvas_pos,
                 ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                 IM_COL32(60, 60, 80, 200));

     dl->PushClipRect(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), true);

     /* Precompute projection once — eliminates O(N) recomputes inside loops */
     sm83_view_cache_update(&s_die_view, &s_die_cache);
     const Sm83ViewCache *vc = &s_die_cache;

     float die_scale_x = vc->scale_x;
     float arc_width_norm = 1.0f / (SM83_BBOX_X_MAX - SM83_BBOX_X_MIN);

     s_drawn_arcs  = 0;
     s_drawn_trans = 0;

     /* ── Draw arcs (wire segments) ── */
     for (int i = 0; i < SM83_ARC_COUNT; i++)
     {
          const Sm83Arc *a = &sm83_arcs[i];
          unsigned int lbit = 1u << a->layer;
          if (!(s_layer_mask & lbit))
               continue;
          if (s_arc_highlight_flags && s_arc_highlight_flags[i])
               continue; /* drawn in highlight pass below */
          if (!sm83_arc_in_viewport_fast(vc, a->ntx, a->nty, a->nhx, a->nhy))
               continue;

          float sx0, sy0, sx1, sy1;
          sm83_die_to_screen_fast(vc, a->ntx, a->nty, &sx0, &sy0);
          sm83_die_to_screen_fast(vc, a->nhx, a->nhy, &sx1, &sy1);

          float thickness = a->width > 0.0f ? a->width * arc_width_norm * die_scale_x : 1.0f;
          if (thickness < 0.5f) thickness = 0.5f;
          if (thickness > 8.0f) thickness = 8.0f;

          dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), s_layer_colors[a->layer], thickness);
          s_drawn_arcs++;
     }

     /* ── Draw highlighted arcs (full net BFS from selected node) ── */
     if (s_arc_highlight_flags)
     {
          for (int i = 0; i < SM83_ARC_COUNT; i++)
          {
               if (!s_arc_highlight_flags[i])
                    continue;
               const Sm83Arc *a = &sm83_arcs[i];
               if (!sm83_arc_in_viewport_fast(vc, a->ntx, a->nty, a->nhx, a->nhy))
                    continue;

               float sx0, sy0, sx1, sy1;
               sm83_die_to_screen_fast(vc, a->ntx, a->nty, &sx0, &sy0);
               sm83_die_to_screen_fast(vc, a->nhx, a->nhy, &sx1, &sy1);

               float thickness = a->width > 0.0f ? a->width * arc_width_norm * die_scale_x : 1.0f;
               if (thickness < 1.0f) thickness = 1.0f;
               thickness += 1.5f;

               dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), IM_COL32(255, 255, 80, 220), thickness);
               s_drawn_arcs++;
          }
     }

     /* ── Draw transistors ── */
     float trans_r = 3.5f * s_die_view.zoom;
     if (trans_r < 1.5f) trans_r = 1.5f;
     if (trans_r > 10.0f) trans_r = 10.0f;

     if (s_die_view.zoom > 0.8f)
     {
          for (int i = 0; i < SM83_TRANSISTOR_COUNT; i++)
          {
               const Sm83Transistor *tr = &sm83_transistors[i];
               unsigned int lbit = 1u << tr->layer;
               if (!(s_layer_mask & lbit))
                    continue;
               if (!sm83_in_viewport_fast(vc, tr->nx, tr->ny, 0.0f))
                    continue;

               float sx, sy;
               sm83_die_to_screen_fast(vc, tr->nx, tr->ny, &sx, &sy);

               bool is_sel = (s_die_sel.type == SM83_SEL_TRANSISTOR && s_die_sel.index == i);
               ImU32 col = is_sel ? IM_COL32(255, 255, 80, 255) : s_layer_colors[tr->layer];
               dl->AddCircleFilled(ImVec2(sx, sy), trans_r, col);
               if (is_sel)
                    dl->AddCircle(ImVec2(sx, sy), trans_r + 2.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
               s_drawn_trans++;
          }
     }

     /* ── Draw selected node highlight ── */
     if (s_die_sel.type == SM83_SEL_NODE && s_die_sel.index >= 0)
     {
          const Sm83Node *n = &sm83_nodes[s_die_sel.index];
          float sx, sy;
          sm83_die_to_screen_fast(vc, n->nx, n->ny, &sx, &sy);
          dl->AddCircle(ImVec2(sx, sy), 6.0f, IM_COL32(255, 255, 80, 255), 0, 2.0f);
     }

     /* ── Draw net nodes (flood-fill from selected node) ── */
     if (s_node_highlight_flags && s_die_sel.type == SM83_SEL_NODE)
     {
          for (int i = 0; i < SM83_NODE_COUNT; i++)
          {
               if (!s_node_highlight_flags[i])
                    continue;
               const Sm83Node *n = &sm83_nodes[i];
               if (!sm83_in_viewport_fast(vc, n->nx, n->ny, 0.005f))
                    continue;
               float sx, sy;
               sm83_die_to_screen_fast(vc, n->nx, n->ny, &sx, &sy);
               dl->AddCircleFilled(ImVec2(sx, sy), 2.5f, IM_COL32(255, 255, 80, 120));
          }
     }

     /* ── Overlay: named signals from emulator state ── */
     if (s_layer_mask & SM83_VIEW_LAYER_OVERLAY)
     {
          /* Colors mirror CPU Datapath panel: same hue per component so
           * activity in one panel is immediately recognizable in the other. */
          static const ImU32 s_group_colors[] = {
              IM_COL32(255, 220, 60,  255), /* PC    - yellow      (= Datapath PC)   */
              IM_COL32(60,  220, 255, 255), /* A     - cyan        (= Datapath A src) */
              IM_COL32(100, 255, 100, 255), /* B     - green       (= Datapath dst)   */
              IM_COL32(200, 160, 255, 255), /* IR    - violet      (= Datapath IR)    */
              IM_COL32(80,  220, 255, 255), /* flags - cyan-blue   (= Datapath Z)     */
              IM_COL32(100, 200, 255, 255), /* IDU   - light blue  (= Datapath IDU)   */
              IM_COL32(180, 255, 180, 255), /* C     - pale green                     */
              IM_COL32(255, 180, 180, 255), /* D     - pale red                       */
              IM_COL32(255, 255, 140, 255), /* E     - pale yellow                    */
              IM_COL32(140, 220, 255, 255), /* H     - sky blue                       */
              IM_COL32(255, 160, 255, 255), /* L     - pink                           */
              IM_COL32(180, 140, 255, 255), /* SP    - violet      (= Datapath SP)    */
              IM_COL32(255, 120, 60,  255), /* DBUS  - coral       (= Datapath bus WR)*/
          };
          float ov_r = 4.0f * s_die_view.zoom + 1.0f;
          float ov_ring = 6.0f * s_die_view.zoom + 2.0f;
          for (int i = 0; i < s_die_overlay.count; i++)
          {
               const Sm83OverlaySignal *sig = &s_die_overlay.signals[i];
               if (!sig->valid)
                    continue;
               if (!sm83_in_viewport_fast(vc, sig->nx, sig->ny, 0.002f))
                    continue;

               float sx, sy;
               sm83_die_to_screen_fast(vc, sig->nx, sig->ny, &sx, &sy);

               int group_idx = sig->group < (int)(sizeof(s_group_colors) / sizeof(s_group_colors[0])) ? sig->group : 0;
               ImU32 base_col = s_group_colors[group_idx];
               uint8_t alpha = sig->value ? (uint8_t)(180 + (uint8_t)(sig->fade * 75.0f))
                                          : (uint8_t)(60 + (uint8_t)(sig->fade * 60.0f));
               ImU32 col = (base_col & 0x00FFFFFFu) | ((ImU32)alpha << 24);
               dl->AddCircleFilled(ImVec2(sx, sy), ov_r, col);
               if (sig->fade > 0.1f)
                    dl->AddCircle(ImVec2(sx, sy), ov_ring,
                                  IM_COL32(255, 255, 255, (uint8_t)(sig->fade * 180)), 0, 1.0f);
          }
     }

     /* ── Netlist Sim overlay: color arcs by net state (EXP, separate from Emulator Overlay) ── */
     if (s_netlist_sim.enabled && s_netlist_sim.initialized)
     {
          /* Colors: green=HIGH, red=LOW, grey=FLOAT, dark=UNKNOWN */
          static const ImU32 sim_colors[] = {
              IM_COL32(60,  60,  70,  140), /* UNKNOWN */
              IM_COL32(80,  220, 80,  200), /* LOW     - green (active transistor) */
              IM_COL32(220, 80,  80,  200), /* HIGH    - red   */
              IM_COL32(140, 140, 160, 120), /* FLOAT   - grey  */
          };
          for (int i = 0; i < SM83_ARC_COUNT; i++)
          {
               const Sm83Arc *a = &sm83_arcs[i];
               if (a->net_id < 0) continue;
               unsigned int lbit = 1u << a->layer;
               if (!(s_layer_mask & lbit)) continue;
               if (!sm83_arc_in_viewport_fast(vc, a->ntx, a->nty, a->nhx, a->nhy)) continue;

               Sm83SimState st = sm83_sim_net_state(&s_netlist_sim, a->net_id);
               if (st == SM83_SIM_UNKNOWN) continue; /* don't clutter with unknown */

               float sx0, sy0, sx1, sy1;
               sm83_die_to_screen_fast(vc, a->ntx, a->nty, &sx0, &sy0);
               sm83_die_to_screen_fast(vc, a->nhx, a->nhy, &sx1, &sy1);

               ImU32 col = sim_colors[st & 3];
               dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), col, 2.0f);
          }
     }

     dl->PopClipRect();

     /* ── Tooltip on hover ── */
     if (hovered)
     {
          ImVec2 mp = ImGui::GetMousePos();
          Sm83Selection hover_sel;
          if (sm83_hit_test(&s_die_view, mp.x, mp.y, 6.0f, s_layer_mask, &hover_sel))
          {
               ImGui::BeginTooltip();
               static const char *sim_state_names[] = {"UNKNOWN","LOW","HIGH","FLOAT"};
               if (hover_sel.type == SM83_SEL_TRANSISTOR)
               {
                    const Sm83Transistor *tr = &sm83_transistors[hover_sel.index];
                    ImGui::Text("%s transistor #%d",
                                tr->layer == SM83_LAYER_NTRANS ? "N" : "P",
                                hover_sel.index);
                    ImGui::Text("pos: (%.1f, %.1f)", tr->x, tr->y);
                    ImGui::Text("norm: (%.4f, %.4f)", tr->nx, tr->ny);
                    /* Show net names and sim state for gate/s1/s2 */
                    if (tr->gate_net >= 0 && tr->gate_net < SM83_NET_COUNT)
                         ImGui::Text("gate: %s  [%s]",
                                     sm83_nets[tr->gate_net],
                                     sim_state_names[sm83_sim_net_state(&s_netlist_sim, tr->gate_net) & 3]);
                    if (tr->s1_net >= 0 && tr->s1_net < SM83_NET_COUNT)
                         ImGui::Text("s1:   %s  [%s]",
                                     sm83_nets[tr->s1_net],
                                     sim_state_names[sm83_sim_net_state(&s_netlist_sim, tr->s1_net) & 3]);
                    if (tr->s2_net >= 0 && tr->s2_net < SM83_NET_COUNT)
                         ImGui::Text("s2:   %s  [%s]",
                                     sm83_nets[tr->s2_net],
                                     sim_state_names[sm83_sim_net_state(&s_netlist_sim, tr->s2_net) & 3]);
               }
               else if (hover_sel.type == SM83_SEL_NODE)
               {
                    const Sm83Node *n = &sm83_nodes[hover_sel.index];
                    ImGui::Text("Node #%d  layer: %s",
                                hover_sel.index,
                                n->layer < 6 ? s_layer_names[n->layer] : "?");
                    ImGui::Text("pos: (%.1f, %.1f)", n->x, n->y);
               }
               ImGui::EndTooltip();
          }
     }

     /* ── Info bar ── */
     ImGui::Separator();
     float fps = ImGui::GetIO().Framerate;
     if (s_netlist_sim.enabled)
          ImGui::TextDisabled("arcs:%d  trans:%d  %.0f fps  |  sim iters:%d  nets:%d",
                              s_drawn_arcs, s_drawn_trans, fps,
                              s_netlist_sim.iterations, SM83_NET_COUNT);
     else
          ImGui::TextDisabled("arcs:%d  trans:%d  %.0f fps", s_drawn_arcs, s_drawn_trans, fps);
     ImGui::SameLine(0, 16);
     if (s_die_sel.type == SM83_SEL_TRANSISTOR)
     {
          const Sm83Transistor *tr = &sm83_transistors[s_die_sel.index];
          ImGui::Text("Selected: %s transistor #%d  @ (%.1f, %.1f)",
                      tr->layer == SM83_LAYER_NTRANS ? "N" : "P",
                      s_die_sel.index, tr->x, tr->y);
     }
     else if (s_die_sel.type == SM83_SEL_NODE)
     {
          const Sm83Node *n = &sm83_nodes[s_die_sel.index];
          ImGui::Text("Selected: node #%d  layer:%s  @ (%.1f, %.1f)",
                      s_die_sel.index,
                      n->layer < 6 ? s_layer_names[n->layer] : "?",
                      n->x, n->y);
     }
     else
     {
          ImGui::TextDisabled("Click a transistor or node to select. Scroll to zoom. Drag to pan.");
     }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * HW Schematic Viewer
 * Renders the actual DMG-CPU-06 KiCad schematic geometry (wires, buses,
 * labels, junctions, component boxes) with live animation driven by
 * emulator bus activity — each net is coloured by its signal group.
 * Source: https://github.com/Gekkio/gb-schematics (CC-BY 4.0, Gekkio)
 * ══════════════════════════════════════════════════════════════════════════*/

static HwSchematicView s_sch_view      = {};
static bool            s_sch_view_init = false;

/* Per-animation-group fade value (driven by sys_viz each frame) */
static float s_sch_anim_fade[HW_ANIM_GROUP_COUNT] = {};

/* Idle color for each anim group (inactive state) */
static const ImU32 SCH_IDLE_COLOR[HW_ANIM_GROUP_COUNT] = {
    IM_COL32( 80,  90, 130, 140),  /* 0  addr_bus    — blue-grey */
    IM_COL32( 80,  90, 130, 140),  /* 1  data_bus    — blue-grey */
    IM_COL32( 60, 110,  80, 140),  /* 2  wram_data   — green-grey */
    IM_COL32( 60, 110,  80, 130),  /* 3  wram_addr   — green-grey */
    IM_COL32(100,  80, 150, 100),  /* 4  clock       — dim violet */
    IM_COL32(100,  70,  40, 120),  /* 5  audio       — dim orange */
    IM_COL32( 40,  80,  60, 120),  /* 6  lcd         — dim teal */
    IM_COL32( 80,  50, 110, 120),  /* 7  irq         — dim purple */
    IM_COL32( 60,  40,  40, 100),  /* 8  power       — dim red */
    IM_COL32( 40,  80, 100, 100),  /* 9  serial      — dim cyan */
    IM_COL32( 80,  80,  50, 100),  /* 10 bus_ctrl    — dim gold */
};

/* Active color for each anim group (when fade > 0) */
static const ImU32 SCH_ACTIVE_COLOR[HW_ANIM_GROUP_COUNT] = {
    IM_COL32( 80, 160, 255, 255),  /* 0  addr_bus    — bright blue */
    IM_COL32(100, 180, 255, 255),  /* 1  data_bus    — light blue */
    IM_COL32( 60, 220, 120, 255),  /* 2  wram_data   — bright green */
    IM_COL32( 80, 200, 100, 230),  /* 3  wram_addr   — green */
    IM_COL32(200, 140, 255, 255),  /* 4  clock       — violet */
    IM_COL32(255, 160,  60, 255),  /* 5  audio       — orange */
    IM_COL32( 60, 220, 160, 255),  /* 6  lcd         — teal */
    IM_COL32(200, 100, 255, 255),  /* 7  irq         — purple */
    IM_COL32(255,  80,  80, 200),  /* 8  power       — red */
    IM_COL32( 60, 200, 220, 255),  /* 9  serial      — cyan */
    IM_COL32(220, 200,  60, 220),  /* 10 bus_ctrl    — gold */
};

static ImU32 sch_wire_color(int anim_group)
{
    if (anim_group < 0 || anim_group >= HW_ANIM_GROUP_COUNT)
        return IM_COL32(70, 75, 95, 120);  /* unlabeled — very dim */

    float fade = s_sch_anim_fade[anim_group];
    if (fade <= 0.0f)
        return SCH_IDLE_COLOR[anim_group];

    float t = fade > 1.0f ? 1.0f : fade;
    ImU32 idle   = SCH_IDLE_COLOR[anim_group];
    ImU32 active = SCH_ACTIVE_COLOR[anim_group];
    uint8_t r = (uint8_t)(((idle >> IM_COL32_R_SHIFT) & 0xFF) * (1-t) + ((active >> IM_COL32_R_SHIFT) & 0xFF) * t);
    uint8_t g = (uint8_t)(((idle >> IM_COL32_G_SHIFT) & 0xFF) * (1-t) + ((active >> IM_COL32_G_SHIFT) & 0xFF) * t);
    uint8_t b = (uint8_t)(((idle >> IM_COL32_B_SHIFT) & 0xFF) * (1-t) + ((active >> IM_COL32_B_SHIFT) & 0xFF) * t);
    uint8_t a = (uint8_t)(((idle >> IM_COL32_A_SHIFT) & 0xFF) * (1-t) + ((active >> IM_COL32_A_SHIFT) & 0xFF) * t);
    return IM_COL32(r, g, b, a);
}

void draw_panel_hw_schematic(struct gb *gb)
{
    float dt = ImGui::GetIO().DeltaTime;

    /* ── Decay + map emulator fades to anim groups ── */
    struct gb_sys_viz *sv = gb ? &gb->debug.sys_viz : nullptr;
    if (sv) {
        float decay = dt * 8.0f;
#define SCH_DECAY(x) ((x) > decay ? (x) - decay : 0.0f)
        sv->fade_cpu_rom  = SCH_DECAY(sv->fade_cpu_rom);
        sv->fade_cpu_wram = SCH_DECAY(sv->fade_cpu_wram);
        sv->fade_cpu_vram = SCH_DECAY(sv->fade_cpu_vram);
        sv->fade_cpu_oam  = SCH_DECAY(sv->fade_cpu_oam);
        sv->fade_cpu_io   = SCH_DECAY(sv->fade_cpu_io);
        sv->fade_dma_oam  = SCH_DECAY(sv->fade_dma_oam);
        sv->fade_ppu_vram = SCH_DECAY(sv->fade_ppu_vram);
        sv->fade_irq_cpu  = SCH_DECAY(sv->fade_irq_cpu);
        sv->fade_apu      = SCH_DECAY(sv->fade_apu);
#undef SCH_DECAY

        /* Map sys_viz fades to anim groups.
         * addr/data driven by ROM access (CPU-side bus activity).
         * wram_data/wram_addr driven by WRAM access.
         * bus_ctrl and lcd also pulse on ROM/WRAM access. */
        float f_bus  = sv->fade_cpu_rom;
        float f_wram = sv->fade_cpu_wram;
        float f_vram = sv->fade_cpu_vram > sv->fade_ppu_vram ? sv->fade_cpu_vram : sv->fade_ppu_vram;
        s_sch_anim_fade[HW_ANIM_ADDR]      = f_bus;
        s_sch_anim_fade[HW_ANIM_DATA]      = f_bus;
        s_sch_anim_fade[HW_ANIM_WRAM_DATA] = f_wram;
        s_sch_anim_fade[HW_ANIM_WRAM_ADDR] = f_wram;
        s_sch_anim_fade[HW_ANIM_CLOCK]     = 0.4f;   /* clock always on, dim */
        s_sch_anim_fade[HW_ANIM_AUDIO]     = sv->fade_apu;
        s_sch_anim_fade[HW_ANIM_LCD]       = f_vram;
        s_sch_anim_fade[HW_ANIM_IRQ]       = sv->fade_irq_cpu;
        s_sch_anim_fade[HW_ANIM_POWER]     = 0.2f;   /* power always on, very dim */
        s_sch_anim_fade[HW_ANIM_SERIAL]    = 0.0f;
        s_sch_anim_fade[HW_ANIM_BUS_CTRL]  = f_bus > f_wram ? f_bus : f_wram;
    }

    /* ── Canvas ── */
    ImVec2 canvas_pos  = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    if (canvas_size.x < 400) canvas_size.x = 400;
    if (canvas_size.y < 300) canvas_size.y = 300;

    if (!s_sch_view_init) {
        s_sch_view.canvas_w = canvas_size.x;
        s_sch_view.canvas_h = canvas_size.y;
        hw_schematic_view_fit(&s_sch_view);
        s_sch_view_init = true;
    }
    s_sch_view.canvas_x = canvas_pos.x;
    s_sch_view.canvas_y = canvas_pos.y;
    s_sch_view.canvas_w = canvas_size.x;
    s_sch_view.canvas_h = canvas_size.y;

    ImGui::InvisibleButton("##sch_canvas", canvas_size,
                           ImGuiButtonFlags_MouseButtonLeft);
    bool canvas_active  = ImGui::IsItemActive();

    /* ── Zoom & pan ── */
    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            ImVec2 mouse = ImGui::GetIO().MousePos;
            float mx_n = (mouse.x - canvas_pos.x) / s_sch_view.zoom + s_sch_view.pan_x;
            float sy   = s_sch_view.zoom * HW_SCHEMATIC_ASPECT;
            float my_n = (mouse.y - canvas_pos.y) / sy + s_sch_view.pan_y;
            float factor = wheel > 0 ? 1.15f : (1.0f / 1.15f);
            s_sch_view.zoom = s_sch_view.zoom * factor;
            if (s_sch_view.zoom < 100.0f)   s_sch_view.zoom = 100.0f;
            if (s_sch_view.zoom > 20000.0f) s_sch_view.zoom = 20000.0f;
            s_sch_view.pan_x = mx_n - (mouse.x - canvas_pos.x) / s_sch_view.zoom;
            s_sch_view.pan_y = my_n - (mouse.y - canvas_pos.y) / (s_sch_view.zoom * HW_SCHEMATIC_ASPECT);
        }
    }
    if (canvas_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        s_sch_view.pan_x -= delta.x / s_sch_view.zoom;
        s_sch_view.pan_y -= delta.y / (s_sch_view.zoom * HW_SCHEMATIC_ASPECT);
    }

    HwSchematicCache cache;
    hw_schematic_view_cache(&s_sch_view, &cache);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(canvas_pos,
                     ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), true);

    /* ── Dark background ── */
    dl->AddRectFilled(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                      IM_COL32(18, 20, 28, 255));

    /* ── Coordinate helpers ── */
    auto SX = [&](float nx) -> float { return cache.origin_x + nx * cache.scale_x; };
    auto SY = [&](float ny) -> float { return cache.origin_y + ny * cache.scale_y; };
    auto SP = [&](float nx, float ny) -> ImVec2 { return ImVec2(SX(nx), SY(ny)); };

    float zoom = s_sch_view.zoom;

    /* ── Dark schematic background ── */
    dl->AddRectFilled(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                      IM_COL32(14, 16, 22, 255));

    /* ── Grid lines (very subtle, only visible when zoomed in) ── */
    if (zoom > 600.0f) {
        float grid_step_n = 10.0f / 297.0f; /* 10mm grid */
        ImU32 grid_col = IM_COL32(30, 35, 45, 255);
        float x0 = cache.vp_nx0 - fmodf(cache.vp_nx0, grid_step_n);
        for (float nx = x0; nx < cache.vp_nx1; nx += grid_step_n) {
            float sx = SX(nx);
            dl->AddLine(ImVec2(sx, canvas_pos.y), ImVec2(sx, canvas_pos.y + canvas_size.y), grid_col);
        }
        float y0 = cache.vp_ny0 - fmodf(cache.vp_ny0, grid_step_n);
        for (float ny = y0; ny < cache.vp_ny1; ny += grid_step_n) {
            float sy = SY(ny);
            dl->AddLine(ImVec2(canvas_pos.x, sy), ImVec2(canvas_pos.x + canvas_size.x, sy), grid_col);
        }
    }

    /* ── Wire thickness ── */
    float wire_px = zoom * (0.4f / 297.0f);   /* 0.4mm wire */
    if (wire_px < 0.8f)  wire_px = 0.8f;
    if (wire_px > 6.0f)  wire_px = 6.0f;
    float bus_px = wire_px * 2.5f;
    if (bus_px > 12.0f) bus_px = 12.0f;

    /* ── Wires & buses — colored by net anim group ── */
    for (int i = 0; i < HW_WIRE_COUNT; i++) {
        const HwWire *w = &hw_wires[i];
        if (!hw_sch_line_in_viewport(&cache, w->nx1, w->ny1, w->nx2, w->ny2))
            continue;

        int anim = -1;
        if (w->net_id >= 0 && w->net_id < HW_NET_COUNT)
            anim = hw_nets[w->net_id].anim_group;

        ImU32 col = sch_wire_color(anim);
        float thick = w->is_bus ? bus_px : wire_px;
        dl->AddLine(SP(w->nx1, w->ny1), SP(w->nx2, w->ny2), col, thick);
    }

    /* ── Junctions ── */
    float jr = wire_px * 1.6f;
    if (jr < 2.0f) jr = 2.0f;
    for (int i = 0; i < HW_JUNCTION_COUNT; i++) {
        const HwJunction *j = &hw_junctions[i];
        if (!hw_sch_in_viewport(&cache, j->nx, j->ny)) continue;
        dl->AddCircleFilled(SP(j->nx, j->ny), jr, IM_COL32(200, 210, 230, 200));
    }

    /* ── Labels (only when zoomed in enough) ── */
    if (zoom > 300.0f) {
        float font_scale = zoom > 1000.0f ? 1.0f : zoom / 1000.0f;
        (void)font_scale; /* ImGui AddText uses default font size */
        for (int i = 0; i < HW_LABEL_COUNT; i++) {
            const HwLabel *lbl = &hw_labels[i];
            if (!hw_sch_in_viewport(&cache, lbl->nx, lbl->ny)) continue;
            float sx = SX(lbl->nx);
            float sy = SY(lbl->ny);
            /* Adjust for angle: 180° means the label attaches from the right */
            float offset = -2.0f;
            if (lbl->angle == 0.0f)
                dl->AddText(ImVec2(sx + 2, sy + offset), IM_COL32(180, 210, 160, 210), lbl->text);
            else
                dl->AddText(ImVec2(sx - ImGui::CalcTextSize(lbl->text).x - 2, sy + offset),
                            IM_COL32(180, 210, 160, 210), lbl->text);
        }
    }

    /* ── Component boxes ── */
    static int s_sch_sel_comp = -1;
    int hover_comp = -1;
    ImVec2 mouse = ImGui::GetIO().MousePos;

    for (int i = 0; i < HW_COMPONENT_COUNT; i++) {
        const HwComponent *comp = &hw_components[i];
        float cx = SX(comp->nx);
        float cy = SY(comp->ny);
        float hw2 = cache.scale_x * comp->nw * 0.5f;
        float hh2 = cache.scale_y * comp->nh * 0.5f;
        if (hw2 < 10.0f) hw2 = 10.0f;
        if (hh2 < 6.0f)  hh2 = 6.0f;

        ImVec2 tl = ImVec2(cx - hw2, cy - hh2);
        ImVec2 br = ImVec2(cx + hw2, cy + hh2);

        /* Cull if completely off canvas */
        if (br.x < canvas_pos.x || tl.x > canvas_pos.x + canvas_size.x) continue;
        if (br.y < canvas_pos.y || tl.y > canvas_pos.y + canvas_size.y) continue;

        /* Pick border color from signal group + fade */
        ImU32 border;
        if (comp->signal_group == 0)      border = sch_wire_color(HW_ANIM_ADDR);   /* CPU */
        else if (comp->signal_group == 1) border = sch_wire_color(HW_ANIM_WRAM_DATA);
        else if (comp->signal_group == 2) border = sch_wire_color(HW_ANIM_WRAM_DATA);
        else if (comp->signal_group == 3) border = sch_wire_color(HW_ANIM_ADDR);   /* Cart */
        else if (comp->signal_group == 4) border = sch_wire_color(HW_ANIM_CLOCK);  /* Xtal */
        else if (comp->signal_group == 5) border = sch_wire_color(HW_ANIM_AUDIO);
        else border = IM_COL32(80, 90, 100, 160);

        bool hovered = mouse.x >= tl.x && mouse.x <= br.x &&
                       mouse.y >= tl.y && mouse.y <= br.y;
        bool selected = (s_sch_sel_comp == i);

        if (hovered) hover_comp = i;

        ImU32 fill = selected ? IM_COL32(40, 55, 80, 230)
                   : hovered  ? IM_COL32(30, 38, 55, 220)
                              : IM_COL32(20, 24, 34, 200);
        float bthick = selected ? 2.5f : hovered ? 1.8f : 1.2f;

        dl->AddRectFilled(tl, br, fill, 3.0f);
        dl->AddRect(tl, br, border, 3.0f, 0, bthick);

        if (zoom > 80.0f) {
            dl->AddText(ImVec2(tl.x + 4, tl.y + 3), IM_COL32(230, 235, 255, 240), comp->ref);
            if (zoom > 200.0f)
                dl->AddText(ImVec2(tl.x + 4, tl.y + 16), IM_COL32(160, 175, 190, 190), comp->value);
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            s_sch_sel_comp = selected ? -1 : i;
    }

    dl->PopClipRect();

    /* ── Hover tooltip ── */
    if (hover_comp >= 0) {
        const HwComponent *comp = &hw_components[hover_comp];
        ImGui::BeginTooltip();
        ImGui::Text("%s  —  %s", comp->ref, comp->value);
        ImGui::TextDisabled("pos: (%.3f, %.3f) normalized", comp->nx, comp->ny);
        ImGui::EndTooltip();
    }

    /* ── Legend + info bar ── */
    ImGui::Separator();
    if (s_sch_sel_comp >= 0 && s_sch_sel_comp < HW_COMPONENT_COUNT) {
        const HwComponent *c = &hw_components[s_sch_sel_comp];
        ImGui::Text("Selecionado: %s  (%s)", c->ref, c->value);
    } else {
        ImGui::TextDisabled("Scroll=zoom  Arraste=pan  Clique=selecionar  |  DMG-CPU-06  (CC-BY 4.0, Gekkio)");
    }
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20);
    /* Mini legend */
    ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "Addr");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.5f, 1.0f), "WRAM");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Audio");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.6f, 1.0f), "LCD");
    ImGui::SameLine(); ImGui::TextColored(ImVec4(0.8f, 0.4f, 1.0f, 1.0f), "IRQ");
}
