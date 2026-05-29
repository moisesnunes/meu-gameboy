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
#include "sm83_node_map.h"
#include "sm83_signal_overlay.h"
#include "sm83_semantic_map.h"
#include "hw_schematic_data.h"
#include "hw_schematic_view.h"
#include "hw_schematic_map.h"
#include "hw_schematic_trace.h"
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

     ImVec2 win_size = ImGui::GetWindowSize();
     if (win_size.x < 960.0f || win_size.y < 660.0f)
          ImGui::SetWindowSize(ImVec2(win_size.x < 960.0f ? 960.0f : win_size.x,
                                      win_size.y < 660.0f ? 660.0f : win_size.y));

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

     auto max2 = [](float a, float b) -> float
     { return a > b ? a : b; };

     float f_cpu_bus = max2(max2(max2(sv->fade_cpu_rom, sv->fade_cpu_wram),
                                  max2(sv->fade_cpu_vram, sv->fade_cpu_oam)),
                            sv->fade_cpu_io);
     float f_video = max2(max2(sv->fade_cpu_vram, sv->fade_ppu_vram),
                          max2(sv->fade_cpu_oam, sv->fade_dma_oam));
     float f_dma = sv->fade_dma_oam;
     float f_irq = sv->fade_irq_cpu;
     float f_apu = sv->fade_apu;

     ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
     ImVec2 canvas_size = ImGui::GetContentRegionAvail();
     if (canvas_size.x < 760.0f)
          canvas_size.x = 760.0f;
     if (canvas_size.y < 560.0f)
          canvas_size.y = 560.0f;

     /* Invisible widget para reservar o espaço */
     ImGui::InvisibleButton("##hw_canvas", canvas_size);
     ImDrawList *dl = ImGui::GetWindowDrawList();

     const float design_w = 920.0f;
     const float design_h = 600.0f;
     float sx = canvas_size.x / design_w;
     float sy = canvas_size.y / design_h;
     float sc = sx < sy ? sx : sy;
     if (sc > 1.0f)
          sc = 1.0f;

     float ox = canvas_pos.x + (canvas_size.x - design_w * sc) * 0.5f;
     float oy = canvas_pos.y + 4.0f;

     auto P = [&](float x, float y) -> ImVec2
     { return ImVec2(ox + x * sc, oy + y * sc); };

     auto rect = [&](float x, float y, float w, float h, ImU32 col, float rounding)
     {
          dl->AddRectFilled(P(x, y), P(x + w, y + h), col, rounding * sc);
     };

     auto outline = [&](float x, float y, float w, float h, ImU32 col, float rounding, float thick = 1.0f)
     {
          dl->AddRect(P(x, y), P(x + w, y + h), col, rounding * sc, 0, thick * sc);
     };

     auto text = [&](float x, float y, ImU32 col, const char *txt)
     {
          dl->AddText(P(x, y), col, txt);
     };

     auto draw_block = [&](float x, float y, float w, float h,
                           const char *title, const char *body,
                           ImU32 active_col, float fade)
     {
          ImU32 border = viz_col(active_col, fade);
          ImVec2 tl = P(x, y);
          ImVec2 br = P(x + w, y + h);
          dl->AddRectFilled(tl, br, IM_COL32(24, 27, 34, 235), 7.0f * sc);
          dl->AddRectFilled(tl, P(x + w, y + 22), IM_COL32(32, 36, 46, 245), 7.0f * sc,
                            ImDrawFlags_RoundCornersTop);
          dl->AddRect(tl, br, border, 7.0f * sc, 0, (fade > 0.05f ? 2.2f : 1.1f) * sc);
          dl->AddText(P(x + 9, y + 5), IM_COL32(225, 230, 245, 245), title);
          if (body && body[0])
               dl->AddText(P(x + 9, y + 31), fade > 0.05f ? IM_COL32(230, 245, 220, 245)
                                                           : IM_COL32(165, 176, 186, 220),
                           body);
          if (fade > 0.01f)
          {
               float pulse = fade > 1.0f ? 1.0f : fade;
               dl->AddRectFilled(P(x + 8, y + h - 10), P(x + 8 + (w - 16) * pulse, y + h - 6),
                                 active_col, 2.0f * sc);
          }
     };

     auto draw_lane = [&](float x1, float y1, float x2, float y2,
                          ImU32 active_col, float fade, const char *label)
     {
          ImU32 base = IM_COL32(58, 62, 74, 150);
          ImU32 col = viz_col(active_col, fade);
          float thick = (fade > 0.04f ? 3.2f : 1.4f) * sc;
          dl->AddLine(P(x1, y1), P(x2, y2), base, 1.0f * sc);
          dl->AddLine(P(x1, y1), P(x2, y2), col, thick);
          if (label && label[0])
          {
               float mx = (x1 + x2) * 0.5f;
               float my = (y1 + y2) * 0.5f;
               dl->AddRectFilled(P(mx - 34, my - 9), P(mx + 34, my + 10), IM_COL32(18, 20, 26, 230), 4.0f * sc);
               ImVec2 ts = ImGui::CalcTextSize(label);
               dl->AddText(P(mx - ts.x * 0.5f / sc, my - 7), fade > 0.04f ? col : IM_COL32(135, 142, 154, 210), label);
          }
     };

     auto draw_chip = [&](float x, float y, const char *label, bool active, ImU32 col)
     {
          ImVec2 ts = ImGui::CalcTextSize(label);
          float w = ts.x / sc + 18.0f;
          dl->AddRectFilled(P(x, y), P(x + w, y + 21), active ? col : IM_COL32(39, 42, 50, 225), 10.0f * sc);
          dl->AddText(P(x + 9, y + 3), active ? IM_COL32(14, 16, 20, 255) : IM_COL32(145, 151, 164, 235), label);
          return w;
     };

     auto event_name = [](gb_hw_trace_event_type type) -> const char *
     {
          switch (type)
          {
          case GB_HW_EVT_CPU_FETCH: return "FETCH";
          case GB_HW_EVT_CPU_READ: return "READ";
          case GB_HW_EVT_CPU_WRITE: return "WRITE";
          case GB_HW_EVT_IRQ_REQUEST: return "IRQ_REQ";
          case GB_HW_EVT_IRQ_ACK: return "IRQ_ACK";
          case GB_HW_EVT_CPU_ALU: return "ALU";
          case GB_HW_EVT_CPU_WRITEBACK: return "WBACK";
          case GB_HW_EVT_DMA_READ: return "DMA_RD";
          case GB_HW_EVT_DMA_WRITE: return "DMA_WR";
          case GB_HW_EVT_PPU_MODE: return "PPU_MODE";
          case GB_HW_EVT_APU_SAMPLE: return "APU_SMP";
          case GB_HW_EVT_PPU_VBLANK: return "VBLANK";
          case GB_HW_EVT_PPU_HBLANK: return "HBLANK";
          case GB_HW_EVT_OAM_DMA: return "OAM_DMA";
          case GB_HW_EVT_TIMER_OVF: return "TMR_OVF";
          case GB_HW_EVT_APU_WRITE: return "APU_WR";
          case GB_HW_EVT_JOYPAD: return "JOYPAD";
          case GB_HW_EVT_SERIAL_START: return "SER_START";
          case GB_HW_EVT_SERIAL_DONE: return "SER_DONE";
          case GB_HW_EVT_MBC_SWITCH: return "MBC_SW";
          default: return "EVENT";
          }
     };

     /* Backdrop */
     rect(0, 0, design_w, design_h, IM_COL32(14, 16, 22, 245), 9.0f);
     outline(0, 0, design_w, design_h, IM_COL32(70, 78, 96, 180), 9.0f);
     rect(12, 12, design_w - 24, 56, IM_COL32(20, 23, 31, 245), 7.0f);

     text(28, 25, IM_COL32(230, 235, 248, 255), "Game Boy hardware bus view");
     char headline[160];
     snprintf(headline, sizeof(headline),
              "%s  %s  PC:%04X  SP:%04X  IF:%02X IE:%02X  bus:%04X/%02X",
              gb->gbc ? "CGB" : "DMG",
              gb->double_speed ? "2x" : "1x",
              gb->cpu.pc, gb->cpu.sp,
              gb->irq.irq_flags, gb->irq.irq_enable,
              sv->last_bus_addr, sv->last_bus_data);
     text(28, 45, IM_COL32(145, 155, 172, 235), headline);

     float chip_x = 610.0f;
     chip_x += draw_chip(chip_x, 28, gb->cpu.halted ? "HALT" : "RUN", !gb->cpu.halted, IM_COL32(100, 220, 120, 255)) + 8.0f;
     chip_x += draw_chip(chip_x, 28, gb->cpu.irq_enable ? "IME" : "DI", gb->cpu.irq_enable, IM_COL32(130, 200, 255, 255)) + 8.0f;
     chip_x += draw_chip(chip_x, 28, gb->bootrom_mapped ? "BOOT" : "CART", gb->bootrom_mapped, IM_COL32(255, 220, 80, 255)) + 8.0f;
     draw_chip(chip_x, 28, gb->debug.hw_trace.enabled ? "TRACE" : "TRACE OFF",
               gb->debug.hw_trace.enabled, IM_COL32(255, 160, 80, 255));

     /* Main rails */
     rect(70, 174, 780, 46, IM_COL32(24, 28, 38, 235), 7.0f);
     outline(70, 174, 780, 46, viz_col(IM_COL32(255, 220, 80, 255), f_cpu_bus), 7.0f, f_cpu_bus > 0.05f ? 2.0f : 1.0f);
     text(86, 188, f_cpu_bus > 0.05f ? IM_COL32(255, 230, 120, 255) : IM_COL32(150, 155, 168, 225),
          "CPU address/data bus");

     rect(70, 328, 780, 38, IM_COL32(21, 25, 34, 230), 7.0f);
     outline(70, 328, 780, 38, viz_col(IM_COL32(80, 190, 255, 255), f_video), 7.0f, f_video > 0.05f ? 2.0f : 1.0f);
     text(86, 339, f_video > 0.05f ? IM_COL32(120, 215, 255, 255) : IM_COL32(140, 150, 166, 220),
          "video memory path");

     /* Blocks */
     char cart_body[80];
     if (gb->cart.rom)
          snprintf(cart_body, sizeof(cart_body), "%s  ROM:%u  RAM:%u",
                   cart_model_str(gb->cart.model),
                   gb->cart.cur_rom_bank,
                   gb->cart.cur_ram_bank);
     else
          snprintf(cart_body, sizeof(cart_body), "(sem ROM)");
     draw_block(28, 94, 168, 66, "Cartridge / MBC", cart_body, IM_COL32(255, 220, 60, 255), sv->fade_cpu_rom);

     char cpu_body[96];
     snprintf(cpu_body, sizeof(cpu_body), "A:%02X BC:%02X%02X DE:%02X%02X HL:%02X%02X",
              gb->cpu.a, gb->cpu.b, gb->cpu.c, gb->cpu.d, gb->cpu.e, gb->cpu.h, gb->cpu.l);
     draw_block(278, 86, 364, 82, "CPU LR35902", cpu_body, IM_COL32(90, 180, 255, 255), max2(f_cpu_bus, f_irq));

     char irq_body[32];
     snprintf(irq_body, sizeof(irq_body), "IF:%02X  IE:%02X",
              gb->irq.irq_flags, gb->irq.irq_enable);
     draw_block(710, 94, 170, 66, "IRQ Controller", irq_body, IM_COL32(190, 105, 255, 255), f_irq);

     char wram_body[24];
     snprintf(wram_body, sizeof(wram_body), "%s  bk:%u",
              gb->gbc ? "32 KiB" : "8 KiB", gb->iram_high_bank);
     draw_block(28, 238, 164, 64, "WRAM", wram_body, IM_COL32(70, 215, 130, 255), sv->fade_cpu_wram);

     char vram_body[24];
     snprintf(vram_body, sizeof(vram_body), "%s  bk:%u",
              gb->gbc ? "16 KiB" : "8 KiB", gb->vram_high_bank ? 1 : 0);
     draw_block(244, 238, 164, 64, "VRAM", vram_body, IM_COL32(70, 170, 255, 255), max2(sv->fade_cpu_vram, sv->fade_ppu_vram));

     char timer_body[48];
     snprintf(timer_body, sizeof(timer_body), "DIV:%04X TIMA:%02X %s",
              gb->timer.divider_counter, gb->timer.counter,
              gb->timer.started ? "ON" : "OFF");
     draw_block(462, 238, 164, 64, "Timer", timer_body, IM_COL32(255, 220, 70, 255),
                gb->timer.reload_pending ? 1.0f : sv->fade_cpu_io * 0.55f);

     char io_body[48];
     snprintf(io_body, sizeof(io_body), "IO $FF00-$FF7F  SB:%02X", gb->serial_data);
     draw_block(680, 238, 200, 64, "IO / Serial / Joypad", io_body, IM_COL32(255, 165, 70, 255), sv->fade_cpu_io);

     char ppu_body[48];
     uint8_t ppu_mode = gb_gpu_get_mode(gb);
     snprintf(ppu_body, sizeof(ppu_body), "%s LY:%3d LCDC:%02X STAT:%02X",
              gpu_mode_str(ppu_mode), gb->gpu.ly,
              gb_gpu_get_lcdc(gb), gb_gpu_get_lcd_stat(gb));
     ImU32 ppu_mode_colors[4] = {
         IM_COL32(100, 200, 100, 255), /* 0 HBlank */
         IM_COL32(255, 200, 60, 255),  /* 1 VBlank */
         IM_COL32(60, 180, 255, 255),  /* 2 OAM Scan */
         IM_COL32(255, 100, 100, 255), /* 3 Drawing */
     };
     draw_block(308, 390, 304, 72, "PPU / LCD controller", ppu_body,
                gb->gpu.master_enable ? ppu_mode_colors[ppu_mode & 3] : IM_COL32(95, 95, 105, 220),
                gb->gpu.master_enable ? max2(sv->fade_ppu_vram, 0.25f) : 0.0f);

     char oam_body[24];
     snprintf(oam_body, sizeof(oam_body), "40 sprites  OBJ:%s",
              gb->gpu.sprite_enable ? "ON" : "OFF");
     draw_block(28, 390, 164, 64, "OAM", oam_body, IM_COL32(255, 140, 205, 255), max2(sv->fade_cpu_oam, sv->fade_dma_oam));

     char dma_body[32];
     if (gb->dma.running)
          snprintf(dma_body, sizeof(dma_body), "pos:%u  src:%04X",
                   gb->dma.position, gb->dma.source);
     else
          snprintf(dma_body, sizeof(dma_body), "idle");
     draw_block(28, 480, 164, 64, "DMA Engine", dma_body, IM_COL32(255, 150, 210, 255), f_dma);

     char apu_body[40];
     snprintf(apu_body, sizeof(apu_body), "CH:%c%c%c%c  %s",
              gb->spu.nr1.running ? '1' : '-',
              gb->spu.nr2.running ? '2' : '-',
              gb->spu.nr3.running ? '3' : '-',
              gb->spu.nr4.running ? '4' : '-',
              gb->spu.enable ? "ON" : "OFF");
     draw_block(680, 390, 200, 64, "APU", apu_body, IM_COL32(255, 200, 100, 255), f_apu);

     char lcd_body[24];
     snprintf(lcd_body, sizeof(lcd_body), "160x144 %s", gb->gpu.master_enable ? "ON" : "OFF");
     draw_block(308, 492, 304, 54, "LCD output", lcd_body, IM_COL32(120, 230, 130, 255),
                gb->gpu.master_enable ? 0.35f : 0.0f);

     /* Lanes on top of the rails. */
     draw_lane(196, 127, 278, 127, IM_COL32(255, 220, 60, 255), sv->fade_cpu_rom, "ROM");
     draw_lane(642, 127, 710, 127, IM_COL32(190, 105, 255, 255), f_irq, "IRQ");
     draw_lane(110, 220, 110, 238, IM_COL32(70, 215, 130, 255), sv->fade_cpu_wram, "WRAM");
     draw_lane(326, 220, 326, 238, IM_COL32(70, 170, 255, 255), sv->fade_cpu_vram, "VRAM");
     draw_lane(544, 220, 544, 238, IM_COL32(255, 220, 70, 255), sv->fade_cpu_io, "TIMER");
     draw_lane(780, 220, 780, 238, IM_COL32(255, 165, 70, 255), sv->fade_cpu_io, "IO");
     draw_lane(326, 302, 326, 328, IM_COL32(70, 170, 255, 255), sv->fade_ppu_vram, "PPU");
     draw_lane(110, 366, 110, 390, IM_COL32(255, 140, 205, 255), max2(sv->fade_cpu_oam, sv->fade_dma_oam), "OAM");
     draw_lane(110, 480, 110, 454, IM_COL32(255, 150, 210, 255), f_dma, "DMA");
     draw_lane(530, 366, 530, 390, IM_COL32(80, 190, 255, 255), sv->fade_ppu_vram, "PIX");
     draw_lane(780, 302, 780, 390, IM_COL32(255, 200, 100, 255), max2(f_apu, sv->fade_cpu_io), "APU");
     draw_lane(460, 462, 460, 492, IM_COL32(120, 230, 130, 255), gb->gpu.master_enable ? 0.45f : 0.0f, "LCD");

     /* PPU scanline progress bar */
     {
          float bar_x = ox + 326.0f * sc;
          float bar_y = oy + 448.0f * sc;
          float bar_w = 268.0f * sc;
          float bar_h = 8.0f * sc;
          dl->AddRectFilled(ImVec2(bar_x, bar_y), ImVec2(bar_x + bar_w, bar_y + bar_h),
                            IM_COL32(34, 36, 44, 230), 4.0f * sc);
          /* 154 total scanlines: 0-143 visible, 144-153 VBlank */
          float pct = gb->gpu.ly / 154.0f;
          ImU32 bar_col = gb->gpu.ly < 144
	                              ? ppu_mode_colors[ppu_mode & 3]
	                              : IM_COL32(255, 200, 60, 200);
          dl->AddRectFilled(ImVec2(bar_x, bar_y),
	                            ImVec2(bar_x + bar_w * pct, bar_y + bar_h),
	                            bar_col, 4.0f * sc);
          char bar_label[32];
          snprintf(bar_label, sizeof(bar_label), "LY %3d / 154", gb->gpu.ly);
          dl->AddText(ImVec2(bar_x + 4.0f, bar_y - 16.0f * sc),
	                      IM_COL32(220, 220, 220, 255), bar_label);
     }

     /* Activity meters */
     {
          struct
          {
               const char *name;
               float fade;
               const char *label;
               ImU32 col;
          } entries[] = {
              {"ROM", sv->fade_cpu_rom, "$0000-7FFF/$A000", IM_COL32(255, 220, 60, 255)},
              {"WRAM", sv->fade_cpu_wram, "$C000-DFFF", IM_COL32(70, 215, 130, 255)},
              {"VRAM", max2(sv->fade_cpu_vram, sv->fade_ppu_vram), "$8000-9FFF", IM_COL32(70, 170, 255, 255)},
              {"OAM", max2(sv->fade_cpu_oam, sv->fade_dma_oam), "$FE00-FE9F", IM_COL32(255, 140, 205, 255)},
              {"IO", sv->fade_cpu_io, "$FF00-FF7F", IM_COL32(255, 165, 70, 255)},
              {"IRQ", f_irq, "IF/IE", IM_COL32(190, 105, 255, 255)},
              {"APU", f_apu, "$FF10-FF3F", IM_COL32(255, 200, 100, 255)},
          };
          rect(626, 476, 254, 70, IM_COL32(19, 22, 30, 235), 7.0f);
          outline(626, 476, 254, 70, IM_COL32(70, 78, 96, 160), 7.0f);
          text(640, 488, IM_COL32(178, 186, 202, 235), "Activity");
          for (int i = 0; i < 7; ++i)
          {
               float x = 640.0f + (i % 4) * 58.0f;
               float y = 512.0f + (i / 4) * 17.0f;
               float t = entries[i].fade > 1.0f ? 1.0f : entries[i].fade;
               dl->AddRectFilled(P(x, y), P(x + 46, y + 7), IM_COL32(40, 43, 52, 220), 3.0f * sc);
               dl->AddRectFilled(P(x, y), P(x + 46 * t, y + 7), entries[i].col, 3.0f * sc);
               dl->AddText(P(x, y - 11), IM_COL32(145, 151, 164, 220), entries[i].name);
          }
     }

     /* Recent HW trace strip */
     {
          struct gb_hw_trace *tr = &gb->debug.hw_trace;
          rect(28, 560, 852, 26, IM_COL32(18, 20, 27, 235), 6.0f);
          if (!tr->enabled)
          {
               text(42, 566, IM_COL32(135, 142, 154, 220), "HW Trace disabled");
          }
          else if (tr->count == 0)
          {
               text(42, 566, IM_COL32(135, 142, 154, 220), "HW Trace enabled, waiting for events");
          }
          else
          {
               char trace_label[48];
               snprintf(trace_label, sizeof(trace_label), "Trace %u", tr->count);
               text(42, 566, IM_COL32(165, 174, 190, 235), trace_label);
               float x = 116.0f;
               uint32_t total = tr->count < GB_HW_TRACE_CAP ? tr->count : (uint32_t)GB_HW_TRACE_CAP;
               if (total > 9)
                    total = 9;
               for (uint32_t i = 0; i < total; i++)
               {
                    uint32_t idx = (tr->head + GB_HW_TRACE_CAP - 1 - i) & (GB_HW_TRACE_CAP - 1);
                    const gb_hw_trace_event *ev = &tr->events[idx];
                    const char *name = event_name(ev->type);
                    ImVec2 ts = ImGui::CalcTextSize(name);
                    float w = ts.x / sc + 18.0f;
                    ImU32 col = ev->write ? IM_COL32(255, 130, 70, 230) : IM_COL32(100, 185, 255, 230);
                    dl->AddRectFilled(P(x, 563), P(x + w, 582), col, 9.0f * sc);
                    dl->AddText(P(x + 9, 566), IM_COL32(14, 16, 20, 255), name);
                    x += w + 7.0f;
               }
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
     const float design_h = 448.0f;
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

     /* Capture badges: this panel is a per-instruction debug snapshot, not a
        cycle-accurate internal pipeline trace. */
     {
          const char *stages[] = {
              "FETCH SNAP",
              cv->bus_write ? "BUS WRITE" : "BUS READ",
              "A/FLAGS",
              "PROXY"};
          const bool active_state[] = {
              fade > 0.0f,
              fade > 0.0f,
              cv->m_cycles > 0,
              true};
          const ImU32 stage_cols[] = {
              IM_COL32(255, 220, 60, 255),
              cv->bus_write ? IM_COL32(255, 120, 60, 255) : IM_COL32(60, 200, 120, 255),
              IM_COL32(80, 220, 255, 255),
              IM_COL32(160, 150, 120, 230),
          };
          float sx2 = 318.0f;
          for (int i = 0; i < 4; ++i)
          {
               bool active = active_state[i];
               ImU32 col = active ? stage_cols[i] : IM_COL32(65, 65, 75, 180);
               float bw = (i == 0) ? 112.0f : (i == 1) ? 104.0f
                                                       : 78.0f;
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

     /* ALU block — shows captured A/src and the current A read-back. */
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

          /* Arrow + read-back */
          dl->AddText(ImVec2(ax + 230.0f, ay), IM_COL32(120, 120, 130, 200), "->");
          char rbuf[8];
          snprintf(rbuf, sizeof(rbuf), "%02X", cv->alu_result);
          dl->AddText(ImVec2(ax + 252.0f, ay),
                      alu_active ? IM_COL32(100, 255, 160, 255) : IM_COL32(140, 160, 140, 180), rbuf);
          dl->AddText(ImVec2(ax + 288.0f, ay),
                      IM_COL32(120, 130, 140, 190),
                      cv->dst == GB_VIZ_REG_A ? "A" : "A now");

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
               if (cv->dst != GB_VIZ_REG_A && cv->dst != GB_VIZ_REG_NONE)
                    dl->AddText(ImVec2(ax + 142.0f, ay + lh),
                                IM_COL32(160, 140, 100, 210),
                                "writeback inferred");
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
          dl->AddText(P(14, 424), IM_COL32(130, 135, 150, 190),
                      "Snapshot view: bus/ALU/source-destination are debug captures; not a transistor/cycle trace.");
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

/* Forward declarations for cross-link helpers (defined near draw_panel_hw_schematic) */
static int   die_net_to_sch_net_id(int sm83_net_id);
static void  sch_jump_to_net(int hw_net_id, ImVec2 canvas_size);
extern ImVec2 s_sch_last_canvas_size;

/* Persistent panel state */
static Sm83ViewTransform s_die_view;
static Sm83ViewCache s_die_cache; /* updated once per frame before draw loops */
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

/* ── Transistor panel UI toggles ── */
static bool s_show_func_regions = true;   /* functional region overlays */
static bool s_show_rails = false;         /* highlight VCC/GND power rail arcs */
static bool s_show_trans_inspect = false; /* persistent transistor inspector window */
static bool s_show_ntrans = true;         /* show NMOS transistors */
static bool s_show_ptrans = true;         /* show PMOS transistors */

/* ── Transistor inspector neighbour cache ── */
struct TransNeighbour
{
     int index;
     const char *via; /* "gate"/"s1"/"s2" */
};
struct TransNeighGroup
{
     int net_id;
     const char *role;
     TransNeighbour nb[6];
     int count;
};
static TransNeighGroup s_inspect_neigh[3]; /* one group per gate/s1/s2 */
static int s_inspect_cached_sel = -1;      /* transistor index whose neighbours are cached */

static void rebuild_inspect_neighbours(int tr_idx)
{
     if (tr_idx == s_inspect_cached_sel)
          return;
     s_inspect_cached_sel = tr_idx;
     const Sm83Transistor *tr = &sm83_transistors[tr_idx];
     int target_nets[3] = {tr->gate_net, tr->s1_net, tr->s2_net};
     const char *role_names[3] = {"gate", "s1", "s2"};
     for (int ri = 0; ri < 3; ri++)
     {
          s_inspect_neigh[ri].net_id = target_nets[ri];
          s_inspect_neigh[ri].role = role_names[ri];
          s_inspect_neigh[ri].count = 0;
          if (target_nets[ri] < 0)
               continue;
          for (int ti = 0; ti < SM83_TRANSISTOR_COUNT && s_inspect_neigh[ri].count < 6; ti++)
          {
               if (ti == tr_idx)
                    continue;
               const Sm83Transistor *nb = &sm83_transistors[ti];
               const char *which = nullptr;
               if (nb->gate_net == target_nets[ri])
                    which = "gate";
               else if (nb->s1_net == target_nets[ri])
                    which = "s1";
               else if (nb->s2_net == target_nets[ri])
                    which = "s2";
               if (!which)
                    continue;
               int idx = s_inspect_neigh[ri].count++;
               s_inspect_neigh[ri].nb[idx] = {ti, which};
          }
     }
}

/* Flat boolean lookup arrays for net highlight (arc, node, transistor).
 * Allocated once, never freed; zeroed on each rebuild. */
static uint8_t *s_arc_highlight_flags   = nullptr;
static uint8_t *s_node_highlight_flags  = nullptr;
static uint8_t *s_trans_highlight_flags = nullptr;
static int s_highlight_flags_cap = 0; /* tracks SM83_ARC_COUNT at alloc time */
static int s_highlight_net_id    = -1; /* net_id currently highlighted, -1 = none */

static void rebuild_highlight(void)
{
     if (s_highlight_flags_cap < SM83_ARC_COUNT)
     {
          delete[] s_arc_highlight_flags;
          delete[] s_node_highlight_flags;
          delete[] s_trans_highlight_flags;
          s_arc_highlight_flags   = new uint8_t[SM83_ARC_COUNT]();
          s_node_highlight_flags  = new uint8_t[SM83_NODE_COUNT]();
          s_trans_highlight_flags = new uint8_t[SM83_TRANSISTOR_COUNT]();
          s_highlight_flags_cap   = SM83_ARC_COUNT;
     }
     else
     {
          memset(s_arc_highlight_flags,   0, SM83_ARC_COUNT);
          memset(s_node_highlight_flags,  0, SM83_NODE_COUNT);
          memset(s_trans_highlight_flags, 0, SM83_TRANSISTOR_COUNT);
     }
     s_highlight_net_id = -1;

     if (s_die_sel.index < 0 || s_die_sel.type == SM83_SEL_NONE)
          return;

     /* Resolve the net_id for the selected element */
     int net_id = -1;
     if (s_die_sel.type == SM83_SEL_TRANSISTOR)
     {
          /* For a transistor, highlight whichever terminal the user last hovered,
           * defaulting to gate. All three nets are shown via s_trans_highlight_flags
           * already; use gate for the arc highlight. */
          const Sm83Transistor *tr = &sm83_transistors[s_die_sel.index];
          net_id = tr->gate_net >= 0 ? tr->gate_net :
                   tr->s1_net  >= 0 ? tr->s1_net  : tr->s2_net;
     }
     else if (s_die_sel.type == SM83_SEL_NODE)
     {
          /* Check if the selected node's first arc has a net_id */
          int arc_buf[16];
          int n = sm83_node_arcs(s_die_sel.index, arc_buf, 16);
          for (int i = 0; i < n && net_id < 0; i++)
               if (sm83_arcs[arc_buf[i]].net_id >= 0)
                    net_id = sm83_arcs[arc_buf[i]].net_id;
     }

     if (net_id >= 0)
     {
          /* Electrical highlight: mark every arc and transistor on this net */
          sm83_net_highlight_by_netid(net_id, s_arc_highlight_flags, s_trans_highlight_flags);
          s_highlight_net_id = net_id;
     }
     else if (s_die_sel.type == SM83_SEL_NODE)
     {
          /* Fallback: geometric BFS for nodes whose arcs have no net_id */
          sm83_net_flood(s_die_sel.index, s_arc_highlight_flags, s_node_highlight_flags);
     }
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
          sm83_semantic_map_init();
          s_sim_view_init = true;
     }

     float dt = ImGui::GetIO().DeltaTime;
     sm83_overlay_update(&s_die_overlay, gb);
     sm83_overlay_tick(&s_die_overlay, dt);

     /* Run sim step when enabled (only if paused and rail anchors are present) */
     bool gb_paused = gb && gb->debug.state == GB_DEBUG_PAUSED;
     if (s_netlist_sim.enabled && s_netlist_sim.initialized && s_netlist_sim.rails_found && gb_paused)
     {
          sm83_sim_seed_from_gb(&s_netlist_sim, gb);
          sm83_sim_step(&s_netlist_sim, 64);
          sm83_semantic_audit(&s_netlist_sim, gb, SM83_CONF_PROBABLE);
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
          bool sim_ready = s_netlist_sim.initialized && s_netlist_sim.rails_found;
          bool heuristic = s_netlist_sim.rail_source == SM83_RAILS_HEURISTIC;
          const char *btn_label = sim_ready
                                      ? (heuristic ? "Netlist Sim [~RAILS]" : "Netlist Sim [EXP]")
                                      : "Netlist Sim [NO RAILS]";
          ImU32 btn_col = sim_on
                              ? (sim_ready ? (heuristic ? IM_COL32(180, 160, 60, 210) : IM_COL32(100, 255, 180, 200))
                                           : IM_COL32(160, 80, 60, 210))
                              : IM_COL32(50, 50, 60, 200);
          ImU32 btn_hov = sim_ready
                              ? (heuristic ? IM_COL32(200, 180, 80, 220) : IM_COL32(80, 200, 140, 200))
                              : IM_COL32(180, 100, 80, 220);
          ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, btn_hov);
          if (ImGui::Button(btn_label))
          {
               s_netlist_sim.enabled = !s_netlist_sim.enabled;
               if (!s_netlist_sim.enabled)
                    sm83_sim_reset(&s_netlist_sim);
          }
          ImGui::PopStyleColor(2);
          if (ImGui::IsItemHovered())
          {
               static const char *rail_src_desc[] = {
                   "Rails: NOT FOUND — sim cannot propagate.",
                   "Rails: named (vcc/gnd found by name).",
                   "Rails: HEURISTIC — identified by connectivity, not confirmed by inspection.",
                   "Rails: manual override.",
               };
               ImGui::SetTooltip("Experimental transistor-level simulation.\n"
                                 "Runs only while paused. Not authoritative.\n"
                                 "%s\n"
                                 "VCC net_id=%d  GND net_id=%d",
                                 rail_src_desc[s_netlist_sim.rail_source & 3],
                                 s_netlist_sim.vcc_net, s_netlist_sim.gnd_net);
          }
     }
     ImGui::SameLine();
     /* Functional Regions toggle */
     {
          if (s_show_func_regions)
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(80, 140, 200, 200));
          else
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 60, 200));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(100, 160, 220, 200));
          if (ImGui::Button("Regions"))
               s_show_func_regions = !s_show_func_regions;
          ImGui::PopStyleColor(2);
          if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Toggle functional region overlays (ALU, registers, IDU, etc.)");
     }
     ImGui::SameLine();
     /* Power rails highlight toggle */
     {
          if (s_show_rails)
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(220, 60, 60, 200));
          else
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 60, 200));
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(240, 80, 80, 200));
          if (ImGui::Button("Rails"))
               s_show_rails = !s_show_rails;
          ImGui::PopStyleColor(2);
          if (ImGui::IsItemHovered())
               ImGui::SetTooltip("Highlight power rail arcs:\n"
                                 "  Red  = VCC (net_id=%d, ratio P:N=14:1)\n"
                                 "  Blue = GND (net_id=%d, ratio N:P=219:1)\n"
                                 "Status: HEURISTIC — not confirmed by die inspection.",
                                 SM83_VCC_NET_HEURISTIC, SM83_GND_NET_HEURISTIC);
     }
     ImGui::SameLine();
     /* N/P transistor type filter */
     {
          auto type_btn = [&](const char *label, bool &flag, ImU32 on_col, ImU32 hov_col, const char *tip)
          {
               ImGui::PushStyleColor(ImGuiCol_Button, flag ? on_col : IM_COL32(50, 50, 60, 200));
               ImGui::PushStyleColor(ImGuiCol_ButtonHovered, flag ? hov_col : IM_COL32(70, 70, 80, 200));
               if (ImGui::Button(label))
                    flag = !flag;
               ImGui::PopStyleColor(2);
               if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tip);
          };
          type_btn("N", s_show_ntrans,
                   IM_COL32(60, 200, 100, 210), IM_COL32(80, 220, 120, 220),
                   "Show NMOS transistors (conduct when gate=HIGH)");
          ImGui::SameLine();
          type_btn("P", s_show_ptrans,
                   IM_COL32(220, 100, 60, 210), IM_COL32(240, 120, 80, 220),
                   "Show PMOS transistors (conduct when gate=LOW, used as pull-ups)");
     }
     ImGui::SameLine();
     ImGui::Text("  zoom:%.2fx", s_die_view.zoom);
     ImGui::PopStyleVar();

     /* ── Canvas + Sidebar layout ── */
     static const float SIDEBAR_W = 160.0f;
     ImVec2 total_avail = ImGui::GetContentRegionAvail();
     if (total_avail.x < 460.0f)
          total_avail.x = 460.0f;
     if (total_avail.y < 300.0f)
          total_avail.y = 300.0f;

     /* ── Canvas ── */
     ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
     ImVec2 canvas_size = ImVec2(total_avail.x - SIDEBAR_W - 6.0f, total_avail.y);
     if (canvas_size.x < 300.0f)
          canvas_size.x = 300.0f;
     if (canvas_size.y < 300.0f)
          canvas_size.y = 300.0f;

     /* Update transform with current canvas */
     s_die_view.canvas_x = canvas_pos.x;
     s_die_view.canvas_y = canvas_pos.y;
     s_die_view.canvas_w = canvas_size.x;
     s_die_view.canvas_h = canvas_size.y;

     /* Save cursor position so we can place the sidebar next to the canvas */
     ImVec2 cursor_before_canvas = ImGui::GetCursorPos();

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

     /* ── Functional region overlays ── */
     {
          if (s_show_func_regions)
          {
               /* Each region: {label, nx_min, ny_min, nx_max, ny_max, color}
                * Coordinates derived from sm83_instances bounding boxes with 0.01 padding.
                * y-axis in die is 0=bottom, 1=top; screen y is inverted. */
               struct FuncRegion
               {
                    const char *label;
                    float nx0, ny0, nx1, ny1;
                    ImU32 fill_col;
                    ImU32 border_col;
               };
               /* Bounding boxes validated against SGB-CPU-01 die SVG annotation.
                * Coords are netlist-normalized [0,1] with die-Y convention:
                * ny=0 is die bottom (high screen-y); rendering flips with (1-ny). */
               /* Bounding boxes derived from sm83_instances[] coordinates and
                * validated against SGB-CPU-01 annotated die SVG (img/ directory).
                * Coordinate convention: nx,ny in [0,1]; ny=0 is die bottom
                * (high screen-y); rendering flips with (1-ny). */
               static const FuncRegion regions[] = {
                   /* ── Register file columns (left→right by nx) ───────────────── */
                   /* IR:  reg_ir nx=0.138 */
                   {"IR", 0.118f, 0.035f, 0.150f, 0.456f, IM_COL32(200, 140, 255, 35), IM_COL32(200, 140, 255, 150)},
                   /* A:   reg_a 0.163..0.196, reg_a_out 0.160 */
                   {"A", 0.150f, 0.035f, 0.208f, 0.456f, IM_COL32(60, 200, 255, 35), IM_COL32(60, 200, 255, 150)},
                   /* L:   reg_l 0.218 */
                   {"L", 0.208f, 0.060f, 0.232f, 0.456f, IM_COL32(80, 160, 255, 28), IM_COL32(80, 160, 255, 110)},
                   /* H:   reg_h 0.242..0.264, reg_hl 0.236 */
                   {"H", 0.232f, 0.035f, 0.278f, 0.456f, IM_COL32(80, 160, 255, 28), IM_COL32(80, 160, 255, 110)},
                   /* E:   reg_e 0.287, reg_de_out 0.293 */
                   {"E", 0.278f, 0.060f, 0.300f, 0.456f, IM_COL32(80, 200, 180, 28), IM_COL32(80, 200, 180, 110)},
                   /* D:   reg_d 0.309..0.331, reg_d_c 0.331 */
                   {"D", 0.300f, 0.035f, 0.343f, 0.456f, IM_COL32(80, 200, 180, 28), IM_COL32(80, 200, 180, 110)},
                   /* C:   reg_c 0.353, reg_bc_out 0.364 */
                   {"C", 0.343f, 0.060f, 0.368f, 0.456f, IM_COL32(100, 220, 140, 28), IM_COL32(100, 220, 140, 110)},
                   /* B:   reg_b 0.371, reg_bus_pch_b 0.393 */
                   {"B", 0.368f, 0.060f, 0.403f, 0.456f, IM_COL32(100, 220, 140, 28), IM_COL32(100, 220, 140, 110)},
                   /* W/Z: reg_z 0.415, reg_wz 0.431, reg_w 0.509 */
                   {"W/Z", 0.403f, 0.060f, 0.522f, 0.456f, IM_COL32(160, 220, 100, 28), IM_COL32(160, 220, 100, 110)},
                   /* SP:  reg_spl 0.556, reg_sp 0.589, reg_sph 0.608..0.633 */
                   {"SP", 0.533f, 0.035f, 0.645f, 0.456f, IM_COL32(180, 140, 255, 30), IM_COL32(180, 140, 255, 120)},
                   /* PC:  reg_pcl 0.662, reg_pc 0.697, reg_pch 0.710 */
                   {"PC", 0.645f, 0.060f, 0.722f, 0.456f, IM_COL32(255, 220, 60, 30), IM_COL32(255, 220, 60, 130)},
                   /* IDU: 0.739..0.848 */
                   {"IDU", 0.722f, 0.035f, 0.860f, 0.476f, IM_COL32(255, 140, 60, 28), IM_COL32(255, 140, 60, 130)},
                   /* IE:  reg_ie 0.833 (nested inside IDU) */
                   {"IE", 0.820f, 0.058f, 0.845f, 0.448f, IM_COL32(200, 80, 80, 35), IM_COL32(220, 100, 100, 140)},
                   /* IRQ: irq_latch/prio 0.804..0.890 (overlaps IDU) */
                   {"IRQ", 0.790f, 0.030f, 0.912f, 0.476f, IM_COL32(200, 60, 60, 22), IM_COL32(200, 80, 80, 100)},

                   /* ── Middle band: Flags + ALU ────────────────────────────── */
                   /* Flags: flag_z/n/h/c at ny=0.579, nx=0.111..0.344 — thin strip */
                   {"Flags", 0.100f, 0.568f, 0.356f, 0.592f, IM_COL32(80, 220, 200, 45), IM_COL32(80, 220, 200, 170)},
                   /* ALU:   alu_* nx=0.103..0.352, ny=0.579..0.908 */
                   {"ALU", 0.090f, 0.558f, 0.363f, 0.920f, IM_COL32(220, 80, 80, 28), IM_COL32(255, 100, 100, 130)},

                   /* ── Bottom band: Data bus + 3-stage Decoder ─────────────── */
                   /* DBUS:  dbus_bridge/nand/not nx=0.020..0.316, ny=0.608..0.924 */
                   {"DBUS", 0.007f, 0.595f, 0.329f, 0.937f, IM_COL32(255, 160, 40, 32), IM_COL32(255, 160, 40, 140)},
                   /* Dec 1: 107 dynamic columns, ny=0.895..0.971 */
                   {"Dec 1", 0.415f, 0.883f, 0.888f, 0.982f, IM_COL32(220, 220, 60, 22), IM_COL32(240, 240, 80, 110)},
                   /* Dec 2: 38 static outputs, ny=0.794..0.797 (thin) */
                   {"Dec 2", 0.408f, 0.782f, 0.845f, 0.810f, IM_COL32(200, 220, 60, 22), IM_COL32(220, 240, 80, 100)},
                   /* Dec 3: 57 control outputs, ny=0.579..0.714 */
                   {"Dec 3", 0.432f, 0.567f, 0.938f, 0.726f, IM_COL32(180, 210, 60, 22), IM_COL32(200, 230, 80, 100)},
               };

               for (int ri = 0; ri < (int)(sizeof(regions) / sizeof(regions[0])); ri++)
               {
                    const FuncRegion *r = &regions[ri];
                    float sx0, sy0, sx1, sy1;
                    sm83_die_to_screen_fast(vc, r->nx0, 1.0f - r->ny0, &sx0, &sy0);
                    sm83_die_to_screen_fast(vc, r->nx1, 1.0f - r->ny1, &sx1, &sy1);
                    /* ensure sx0<sx1, sy0<sy1 */
                    if (sx0 > sx1)
                    {
                         float t = sx0;
                         sx0 = sx1;
                         sx1 = t;
                    }
                    if (sy0 > sy1)
                    {
                         float t = sy0;
                         sy0 = sy1;
                         sy1 = t;
                    }
                    /* skip if off-screen */
                    if (sx1 < canvas_pos.x || sx0 > canvas_pos.x + canvas_size.x)
                         continue;
                    if (sy1 < canvas_pos.y || sy0 > canvas_pos.y + canvas_size.y)
                         continue;

                    dl->AddRectFilled(ImVec2(sx0, sy0), ImVec2(sx1, sy1), r->fill_col, 3.0f);
                    dl->AddRect(ImVec2(sx0, sy0), ImVec2(sx1, sy1), r->border_col, 3.0f, 0, 1.2f);

                    /* Label at top-left of region if large enough */
                    float region_w = sx1 - sx0;
                    float region_h = sy1 - sy0;
                    if (region_w > 24.0f && region_h > 10.0f)
                    {
                         ImVec2 label_pos(sx0 + 3.0f, sy0 + 2.0f);
                         dl->AddText(label_pos, r->border_col | IM_COL32(0, 0, 0, 220), r->label);
                    }
               }
          }
     }

     float die_scale_x = vc->scale_x;
     float arc_width_norm = 1.0f / (SM83_BBOX_X_MAX - SM83_BBOX_X_MIN);

     s_drawn_arcs = 0;
     s_drawn_trans = 0;

     /* ── Draw power rail arcs (VCC=red, GND=blue) ── */
     if (s_show_rails)
     {
          const ImU32 vcc_col = IM_COL32(255, 60, 60, 180);
          const ImU32 gnd_col = IM_COL32(60, 120, 255, 180);
          int vcc_id = s_netlist_sim.initialized ? s_netlist_sim.vcc_net : SM83_VCC_NET_HEURISTIC;
          int gnd_id = s_netlist_sim.initialized ? s_netlist_sim.gnd_net : SM83_GND_NET_HEURISTIC;

          for (int i = 0; i < SM83_ARC_COUNT; i++)
          {
               const Sm83Arc *a = &sm83_arcs[i];
               if (a->net_id != vcc_id && a->net_id != gnd_id)
                    continue;
               if (!sm83_arc_in_viewport_fast(vc, a->ntx, 1.0f - a->nty, a->nhx, 1.0f - a->nhy))
                    continue;

               float sx0, sy0, sx1, sy1;
               sm83_die_to_screen_fast(vc, a->ntx, 1.0f - a->nty, &sx0, &sy0);
               sm83_die_to_screen_fast(vc, a->nhx, 1.0f - a->nhy, &sx1, &sy1);

               float thickness = a->width > 0.0f ? a->width * arc_width_norm * die_scale_x : 1.0f;
               if (thickness < 1.0f)
                    thickness = 1.0f;
               if (thickness > 10.0f)
                    thickness = 10.0f;

               ImU32 col = (a->net_id == vcc_id) ? vcc_col : gnd_col;
               dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), col, thickness + 1.5f);
          }
     }

     /* ── Draw arcs (wire segments) ── */
     for (int i = 0; i < SM83_ARC_COUNT; i++)
     {
          const Sm83Arc *a = &sm83_arcs[i];
          unsigned int lbit = 1u << a->layer;
          if (!(s_layer_mask & lbit))
               continue;
          if (s_arc_highlight_flags && s_arc_highlight_flags[i])
               continue; /* drawn in highlight pass below */
          if (!sm83_arc_in_viewport_fast(vc, a->ntx, 1.0f - a->nty, a->nhx, 1.0f - a->nhy))
               continue;

          float sx0, sy0, sx1, sy1;
          sm83_die_to_screen_fast(vc, a->ntx, 1.0f - a->nty, &sx0, &sy0);
          sm83_die_to_screen_fast(vc, a->nhx, 1.0f - a->nhy, &sx1, &sy1);

          float thickness = a->width > 0.0f ? a->width * arc_width_norm * die_scale_x : 1.0f;
          if (thickness < 0.5f)
               thickness = 0.5f;
          if (thickness > 8.0f)
               thickness = 8.0f;

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
               if (!sm83_arc_in_viewport_fast(vc, a->ntx, 1.0f - a->nty, a->nhx, 1.0f - a->nhy))
                    continue;

               float sx0, sy0, sx1, sy1;
               sm83_die_to_screen_fast(vc, a->ntx, 1.0f - a->nty, &sx0, &sy0);
               sm83_die_to_screen_fast(vc, a->nhx, 1.0f - a->nhy, &sx1, &sy1);

               float thickness = a->width > 0.0f ? a->width * arc_width_norm * die_scale_x : 1.0f;
               if (thickness < 1.0f)
                    thickness = 1.0f;
               thickness += 1.5f;

               dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), IM_COL32(255, 255, 80, 220), thickness);
               s_drawn_arcs++;
          }
     }

     /* ── Draw transistors ── */
     float trans_r = 3.5f * s_die_view.zoom;
     if (trans_r < 1.5f)
          trans_r = 1.5f;
     if (trans_r > 10.0f)
          trans_r = 10.0f;

     bool sim_active = s_netlist_sim.enabled && s_netlist_sim.initialized;

     if (s_die_view.zoom > 0.8f)
     {
          for (int i = 0; i < SM83_TRANSISTOR_COUNT; i++)
          {
               const Sm83Transistor *tr = &sm83_transistors[i];
               unsigned int lbit = 1u << tr->layer;
               if (!(s_layer_mask & lbit))
                    continue;

               bool is_nmos = (tr->layer == SM83_LAYER_NTRANS);
               if (is_nmos && !s_show_ntrans)
                    continue;
               if (!is_nmos && !s_show_ptrans)
                    continue;

               if (!sm83_in_viewport_fast(vc, tr->nx, 1.0f - tr->ny, 0.0f))
                    continue;

               float sx, sy;
               sm83_die_to_screen_fast(vc, tr->nx, 1.0f - tr->ny, &sx, &sy);

               bool is_sel = (s_die_sel.type == SM83_SEL_TRANSISTOR && s_die_sel.index == i);

               bool is_net_hi = s_trans_highlight_flags && s_trans_highlight_flags[i] && !is_sel;

               ImU32 col;
               float r = trans_r;
               if (is_sel)
               {
                    col = IM_COL32(255, 255, 80, 255);
               }
               else if (is_net_hi)
               {
                    /* electrically connected to selected net — cyan ring */
                    col = is_nmos ? IM_COL32(80, 220, 255, 220) : IM_COL32(255, 160, 80, 220);
                    r *= 1.15f;
               }
               else if (sim_active)
               {
                    Sm83SimState gate_st = sm83_sim_net_state(&s_netlist_sim, tr->gate_net);
                    bool conducts = is_nmos ? SM83_SIM_IS_HIGH(gate_st) : SM83_SIM_IS_LOW(gate_st);
                    bool known = SM83_SIM_IS_HIGH(gate_st) || SM83_SIM_IS_LOW(gate_st);
                    if (!known)
                    {
                         /* sim has no opinion — dim */
                         col = is_nmos ? IM_COL32(40, 70, 40, 120) : IM_COL32(70, 40, 30, 120);
                    }
                    else if (conducts)
                    {
                         /* conducting: bright */
                         col = is_nmos ? IM_COL32(60, 220, 100, 230) : IM_COL32(255, 130, 60, 230);
                         r *= 1.25f; /* slightly larger to pop */
                    }
                    else
                    {
                         /* off: muted */
                         col = is_nmos ? IM_COL32(30, 80, 40, 150) : IM_COL32(90, 40, 20, 150);
                    }
               }
               else
               {
                    col = s_layer_colors[tr->layer];
               }

               dl->AddCircleFilled(ImVec2(sx, sy), r, col);
               if (is_sel)
                    dl->AddCircle(ImVec2(sx, sy), r + 2.0f, IM_COL32(255, 255, 255, 200), 0, 1.5f);
               else if (is_net_hi)
                    dl->AddCircle(ImVec2(sx, sy), r + 1.5f, IM_COL32(255, 255, 255, 120), 0, 1.0f);
               s_drawn_trans++;
          }
     }

     /* ── Draw selected node highlight ── */
     if (s_die_sel.type == SM83_SEL_NODE && s_die_sel.index >= 0)
     {
          const Sm83Node *n = &sm83_nodes[s_die_sel.index];
          float sx, sy;
          sm83_die_to_screen_fast(vc, n->nx, 1.0f - n->ny, &sx, &sy);
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
               if (!sm83_in_viewport_fast(vc, n->nx, 1.0f - n->ny, 0.005f))
                    continue;
               float sx, sy;
               sm83_die_to_screen_fast(vc, n->nx, 1.0f - n->ny, &sx, &sy);
               dl->AddCircleFilled(ImVec2(sx, sy), 2.5f, IM_COL32(255, 255, 80, 120));
          }
     }

     /* ── Overlay: named signals from emulator state ── */
     if (s_layer_mask & SM83_VIEW_LAYER_OVERLAY)
     {
          /* Colors mirror CPU Datapath panel: same hue per component so
           * activity in one panel is immediately recognizable in the other. */
          static const ImU32 s_group_colors[] = {
              IM_COL32(255, 220, 60, 255),  /* PC    - yellow      (= Datapath PC)   */
              IM_COL32(60, 220, 255, 255),  /* A     - cyan        (= Datapath A src) */
              IM_COL32(100, 255, 100, 255), /* B     - green       (= Datapath dst)   */
              IM_COL32(200, 160, 255, 255), /* IR    - violet      (= Datapath IR)    */
              IM_COL32(80, 220, 255, 255),  /* flags - cyan-blue   (= Datapath Z)     */
              IM_COL32(100, 200, 255, 255), /* IDU   - light blue  (= Datapath IDU)   */
              IM_COL32(180, 255, 180, 255), /* C     - pale green                     */
              IM_COL32(255, 180, 180, 255), /* D     - pale red                       */
              IM_COL32(255, 255, 140, 255), /* E     - pale yellow                    */
              IM_COL32(140, 220, 255, 255), /* H     - sky blue                       */
              IM_COL32(255, 160, 255, 255), /* L     - pink                           */
              IM_COL32(180, 140, 255, 255), /* SP    - violet      (= Datapath SP)    */
              IM_COL32(255, 120, 60, 255),  /* DBUS  - coral       (= Datapath bus WR)*/
          };
          float ov_r = 4.0f * s_die_view.zoom + 1.0f;
          float ov_ring = 6.0f * s_die_view.zoom + 2.0f;

          /* Map GB_VIZ_REG_* -> SM83_SIG_GROUP_* for src/dst highlighting.
           * -1 means "no group highlight for this register type". */
          static const int reg_to_group[] = {
              -1,                /* NONE  */
              SM83_SIG_GROUP_A,  /* A     */
              SM83_SIG_GROUP_B,  /* B     */
              SM83_SIG_GROUP_C,  /* C     */
              SM83_SIG_GROUP_D,  /* D     */
              SM83_SIG_GROUP_E,  /* E     */
              SM83_SIG_GROUP_H,  /* H     */
              SM83_SIG_GROUP_L,  /* L     */
              -1,                /* HL    */
              -1,                /* BC    */
              -1,                /* DE    */
              SM83_SIG_GROUP_SP, /* SP    */
              -1,                /* IMM8  */
              -1,                /* IMM16 */
              -1,                /* MEM   */
          };
          int src_group = -1, dst_group = -1;
          if (gb)
          {
               int src = gb->debug.cpu_viz.src;
               int dst = gb->debug.cpu_viz.dst;
               if (src >= 0 && src < (int)(sizeof(reg_to_group) / sizeof(reg_to_group[0])))
                    src_group = reg_to_group[src];
               if (dst >= 0 && dst < (int)(sizeof(reg_to_group) / sizeof(reg_to_group[0])))
                    dst_group = reg_to_group[dst];
          }

          for (int i = 0; i < s_die_overlay.count; i++)
          {
               const Sm83OverlaySignal *sig = &s_die_overlay.signals[i];
               if (!sig->valid)
                    continue;
               if (!sm83_in_viewport_fast(vc, sig->nx, 1.0f - sig->ny, 0.002f))
                    continue;

               float sx, sy;
               sm83_die_to_screen_fast(vc, sig->nx, 1.0f - sig->ny, &sx, &sy);

               int group_idx = sig->group < (int)(sizeof(s_group_colors) / sizeof(s_group_colors[0])) ? sig->group : 0;
               ImU32 base_col = s_group_colors[group_idx];
               uint8_t alpha = sig->value ? (uint8_t)(180 + (uint8_t)(sig->fade * 75.0f))
                                          : (uint8_t)(60 + (uint8_t)(sig->fade * 60.0f));
               ImU32 col = (base_col & 0x00FFFFFFu) | ((ImU32)alpha << 24);
               dl->AddCircleFilled(ImVec2(sx, sy), ov_r, col);
               if (sig->fade > 0.1f)
                    dl->AddCircle(ImVec2(sx, sy), ov_ring,
                                  IM_COL32(255, 255, 255, (uint8_t)(sig->fade * 180)), 0, 1.0f);

               /* src/dst highlight rings */
               bool is_src = (sig->group == src_group && src_group >= 0);
               bool is_dst = (sig->group == dst_group && dst_group >= 0);
               float hi_r = ov_ring + 2.5f;
               if (is_src && is_dst)
                    dl->AddCircle(ImVec2(sx, sy), hi_r, IM_COL32(255, 160, 60, 220), 0, 2.0f);
               else if (is_src)
                    dl->AddCircle(ImVec2(sx, sy), hi_r, IM_COL32(255, 220, 60, 220), 0, 2.0f);
               else if (is_dst)
                    dl->AddCircle(ImVec2(sx, sy), hi_r, IM_COL32(80, 220, 80, 220), 0, 2.0f);
          }
     }

     /* ── Netlist Sim overlay: color arcs by net state (EXP, separate from Emulator Overlay) ── */
     if (s_netlist_sim.enabled && s_netlist_sim.initialized)
     {
          /* Colors indexed by Sm83SimState enum value:
           *  0=UNKNOWN  1=FLOAT  2=LOW_WEAK  3=HIGH_WEAK  4=LOW  5=HIGH  6=CONFLICT */
          static const ImU32 sim_colors[] = {
              IM_COL32(50, 50, 60, 0),     /* UNKNOWN    — invisible */
              IM_COL32(100, 100, 120, 90), /* FLOAT      — dim grey */
              IM_COL32(60, 160, 60, 140),  /* LOW_WEAK   — dim green */
              IM_COL32(160, 60, 60, 140),  /* HIGH_WEAK  — dim red */
              IM_COL32(80, 240, 80, 210),  /* LOW        — bright green */
              IM_COL32(240, 80, 80, 210),  /* HIGH       — bright red */
              IM_COL32(255, 40, 255, 240), /* CONFLICT   — magenta flash */
          };
          static const float sim_widths[] = {
              0.0f, 1.0f, 1.5f, 1.5f, 2.5f, 2.5f, 3.0f};
          for (int i = 0; i < SM83_ARC_COUNT; i++)
          {
               const Sm83Arc *a = &sm83_arcs[i];
               if (a->net_id < 0)
                    continue;
               unsigned int lbit = 1u << a->layer;
               if (!(s_layer_mask & lbit))
                    continue;
               if (!sm83_arc_in_viewport_fast(vc, a->ntx, 1.0f - a->nty, a->nhx, 1.0f - a->nhy))
                    continue;

               Sm83SimState st = sm83_sim_net_state(&s_netlist_sim, a->net_id);
               if (st == SM83_SIM_UNKNOWN)
                    continue;

               float sx0, sy0, sx1, sy1;
               sm83_die_to_screen_fast(vc, a->ntx, 1.0f - a->nty, &sx0, &sy0);
               sm83_die_to_screen_fast(vc, a->nhx, 1.0f - a->nhy, &sx1, &sy1);

               int si = (int)st < SM83_SIM_STATE_COUNT ? (int)st : 0;
               dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), sim_colors[si], sim_widths[si]);
          }

          /* ── Rail highlight: draw VCC and GND arcs in distinct colors ──
           * This makes the power distribution network visible on the die and
           * allows visual confirmation that the heuristic rails are correct. */
          int rail_ids[2] = {s_netlist_sim.vcc_net, s_netlist_sim.gnd_net};
          ImU32 rail_cols[2] = {
              IM_COL32(255, 80, 255, 200), /* VCC — magenta */
              IM_COL32(80, 200, 255, 200), /* GND — cyan    */
          };
          for (int r = 0; r < 2; r++)
          {
               int rail_id = rail_ids[r];
               if (rail_id < 0 || rail_id >= SM83_NET_COUNT)
                    continue;
               for (int i = 0; i < SM83_ARC_COUNT; i++)
               {
                    const Sm83Arc *a = &sm83_arcs[i];
                    if (a->net_id != rail_id)
                         continue;
                    if (!sm83_arc_in_viewport_fast(vc, a->ntx, 1.0f - a->nty, a->nhx, 1.0f - a->nhy))
                         continue;
                    float sx0, sy0, sx1, sy1;
                    sm83_die_to_screen_fast(vc, a->ntx, 1.0f - a->nty, &sx0, &sy0);
                    sm83_die_to_screen_fast(vc, a->nhx, 1.0f - a->nhy, &sx1, &sy1);
                    dl->AddLine(ImVec2(sx0, sy0), ImVec2(sx1, sy1), rail_cols[r], 2.5f);
               }
          }
     }

     dl->PopClipRect();

     /* ── Sidebar: cycle info + stage badge ── */
     /* Position sidebar to the right of the canvas at the same vertical origin */
     ImGui::SetCursorPos(ImVec2(cursor_before_canvas.x + canvas_size.x + 6.0f,
                                cursor_before_canvas.y));
     ImGui::BeginChild("##die_sidebar", ImVec2(SIDEBAR_W, canvas_size.y), true,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
     {
          const struct gb_cpu_viz *cv = gb ? &gb->debug.cpu_viz : nullptr;

          /* Stage badge */
          static const char *stage_names[] = {"FETCH", "DECODE", "EXECUTE", "IRQ"};
          static const ImU32 stage_colors[] = {
              IM_COL32(60, 160, 255, 220), /* FETCH   - blue  */
              IM_COL32(255, 200, 60, 220), /* DECODE  - yellow */
              IM_COL32(80, 220, 80, 220),  /* EXECUTE - green  */
              IM_COL32(220, 80, 80, 220),  /* IRQ     - red    */
          };
          int stage = cv ? (cv->stage & 3) : 0;
          ImGui::PushStyleColor(ImGuiCol_Button, stage_colors[stage]);
          ImGui::PushStyleColor(ImGuiCol_ButtonHovered, stage_colors[stage]);
          ImGui::Button(stage_names[stage], ImVec2(-1, 0));
          ImGui::PopStyleColor(2);

          ImGui::Separator();

          /* Opcode + mnemonic */
          if (cv)
          {
               ImGui::TextDisabled("OP");
               ImGui::SameLine();
               ImGui::Text("%02X  %s", cv->opcode, cv->mnemonic);
          }

          ImGui::Spacing();

          /* ALU info */
          static const char *alu_op_names[] = {
              "---", "ADD", "SUB", "AND", "OR ", "XOR", "CP ", "INC", "DEC", "SHF", "BIT"};
          if (cv && cv->alu_op != 0 /* GB_VIZ_ALU_NONE */)
          {
               ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "ALU");
               ImGui::SameLine();
               ImGui::Text("%s", alu_op_names[cv->alu_op < 11 ? cv->alu_op : 0]);
               ImGui::Text("  A: %02X", cv->alu_a);
               ImGui::Text("  B: %02X", cv->alu_b);
               ImGui::Text("  R: %02X", cv->alu_result);
          }
          else
          {
               ImGui::TextDisabled("ALU ---");
          }

          ImGui::Separator();

          /* Flags before/after */
          if (cv)
          {
               ImGui::TextDisabled("Flags");
               static const char *flag_names[] = {"Z", "N", "H", "C"};
               static const int flag_bits[] = {3, 2, 1, 0};
               for (int fi = 0; fi < 4; fi++)
               {
                    int bit = flag_bits[fi];
                    bool before = (cv->flags_before >> bit) & 1;
                    bool after = (cv->flags_after >> bit) & 1;
                    bool changed = (cv->flags_changed >> bit) & 1;
                    if (changed)
                         ImGui::TextColored(before ? ImVec4(1.0f, 0.8f, 0.2f, 1.0f) : ImVec4(0.6f, 0.6f, 0.7f, 1.0f),
                                            " %s %d->%d *", flag_names[fi], (int)before, (int)after);
                    else
                    {
                         ImGui::TextDisabled(" %s %d", flag_names[fi], (int)after);
                    }
               }
          }

          ImGui::Separator();

          /* Bus info */
          if (cv)
          {
               ImGui::TextDisabled("Bus");
               ImGui::Text(" A:%04X", cv->addr_bus);
               ImGui::Text(" D:%02X  %s", cv->data_bus, cv->bus_write ? "WR" : "RD");
               ImGui::Text(" M:%d", (int)cv->m_cycles);
          }

          ImGui::Separator();

          /* Netlist sim convergence + rail status */
          if (s_netlist_sim.enabled && s_netlist_sim.initialized)
          {
               ImGui::Separator();
               ImGui::TextDisabled("Netlist Sim");
               if (!s_netlist_sim.rails_found)
               {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f), "NO RAILS");
               }
               else
               {
                    static const char *src_labels[] = {"missing", "named", "heuristic", "manual"};
                    static const ImVec4 src_colors[] = {
                        ImVec4(1.0f, 0.4f, 0.2f, 1.0f),  /* missing  */
                        ImVec4(0.4f, 1.0f, 0.6f, 1.0f),  /* named    */
                        ImVec4(1.0f, 0.85f, 0.3f, 1.0f), /* heuristic */
                        ImVec4(0.6f, 0.8f, 1.0f, 1.0f),  /* manual   */
                    };
                    int rs = s_netlist_sim.rail_source & 3;
                    ImGui::TextColored(src_colors[rs], "rails: %s", src_labels[rs]);
                    ImGui::TextDisabled("VCC #%d", s_netlist_sim.vcc_net);
                    ImGui::TextDisabled("GND #%d", s_netlist_sim.gnd_net);
                    if (s_netlist_sim.iterations < 64)
                         ImGui::TextDisabled("itr: %d", s_netlist_sim.iterations);
                    else
                         ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.1f, 1.0f), "DIVERGED");
                    if (s_netlist_sim.conflict_count > 0)
                         ImGui::TextColored(ImVec4(1.0f, 0.1f, 1.0f, 1.0f),
                                            "conflicts: %d", s_netlist_sim.conflict_count);
                    else
                         ImGui::TextDisabled("conflicts: 0");
               }

               /* ── Mismatch audit ── */
               ImGui::Separator();
               if (!gb_paused)
               {
                    ImGui::TextDisabled("Audit: run paused");
               }
               else if (sm83_mismatch_count == 0)
               {
                    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "Audit OK");
               }
               else
               {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.15f, 1.0f),
                                       "Mismatches: %d", sm83_mismatch_count);

                    /* Signal name abbreviations matching Sm83SemanticSignal enum */
                    static const char *sig_names[] = {
                        "PCL", "PCH", "A", "B", "C", "D", "E", "H", "L",
                        "SPL", "SPH", "IR", "FZ", "FN", "FH", "FC", "DBUS", "IDU", "VCC", "GND"};
                    /* Show up to 8 mismatches from the ring (most recent) */
                    int show = sm83_mismatch_count < 8 ? sm83_mismatch_count : 8;
                    int base = sm83_mismatch_head - sm83_mismatch_count;
                    for (int mi = 0; mi < show; mi++)
                    {
                         int idx = (base + mi + SM83_MISMATCH_BUF_SIZE) & (SM83_MISMATCH_BUF_SIZE - 1);
                         const Sm83DieMismatch *mm = &sm83_mismatch_buf[idx];
                         int si = (int)mm->signal < 20 ? (int)mm->signal : 20;
                         const char *sname = si < 20 ? sig_names[si] : "?";
                         ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                                            " %s[%d] e=%d s=%d",
                                            sname, mm->bit,
                                            mm->emulator_bit, mm->sim_bit);
                         if (ImGui::IsItemHovered())
                              ImGui::SetTooltip("net_id=%d  conf=%d", mm->net_id, (int)mm->confidence);
                    }
                    if (sm83_mismatch_count > 8)
                         ImGui::TextDisabled(" +%d more", sm83_mismatch_count - 8);
               }
          }
     }
     ImGui::EndChild();

     /* ── Tooltip on hover ── */
     if (hovered)
     {
          ImVec2 mp = ImGui::GetMousePos();
          Sm83Selection hover_sel;
          if (sm83_hit_test(&s_die_view, mp.x, mp.y, 6.0f, s_layer_mask, &hover_sel))
          {
               ImGui::BeginTooltip();
               static const char *sim_state_names[] = {
                   "UNKNOWN", "FLOAT", "LOW~", "HIGH~", "LOW", "HIGH", "CONFLICT"};
               static const char *sim_src_names[] = {
                   "none", "rail", "seed", "prop", "conflict"};
               static const ImVec4 sim_state_cols[] = {
                   ImVec4(0.5f, 0.5f, 0.5f, 1.0f), /* UNKNOWN  */
                   ImVec4(0.6f, 0.6f, 0.7f, 1.0f), /* FLOAT    */
                   ImVec4(0.4f, 0.8f, 0.4f, 1.0f), /* LOW~     */
                   ImVec4(0.8f, 0.4f, 0.4f, 1.0f), /* HIGH~    */
                   ImVec4(0.2f, 1.0f, 0.2f, 1.0f), /* LOW      */
                   ImVec4(1.0f, 0.2f, 0.2f, 1.0f), /* HIGH     */
                   ImVec4(1.0f, 0.1f, 1.0f, 1.0f), /* CONFLICT */
               };

               auto net_tooltip_line = [&](const char *prefix, int net_id)
               {
                    if (net_id < 0 || net_id >= SM83_NET_COUNT)
                         return;
                    Sm83SimState st = sm83_sim_net_state(&s_netlist_sim, net_id);
                    Sm83NetSource sr = sm83_sim_net_source(&s_netlist_sim, net_id);
                    int si = (int)st < SM83_SIM_STATE_COUNT ? (int)st : 0;
                    int sri = (int)sr <= 4 ? (int)sr : 0;
                    ImGui::Text("%s %s", prefix, sm83_nets[net_id]);
                    ImGui::SameLine();
                    ImGui::TextColored(sim_state_cols[si], "[%s/%s]",
                                       sim_state_names[si], sim_src_names[sri]);
               };

               if (hover_sel.type == SM83_SEL_TRANSISTOR)
               {
                    const Sm83Transistor *tr = &sm83_transistors[hover_sel.index];
                    Sm83SimState gs = sm83_sim_net_state(&s_netlist_sim, tr->gate_net);
                    bool conducts = (tr->layer == SM83_LAYER_NTRANS)
                                        ? SM83_SIM_IS_HIGH(gs)
                                        : SM83_SIM_IS_LOW(gs);
                    ImGui::Text("%s transistor #%d  %s",
                                tr->layer == SM83_LAYER_NTRANS ? "N" : "P",
                                hover_sel.index,
                                conducts ? "[CONDUCTING]" : "[off]");
                    ImGui::Text("pos: (%.1f, %.1f)  norm: (%.4f, %.4f)",
                                tr->x, tr->y, tr->nx, tr->ny);
                    if (s_netlist_sim.enabled && s_netlist_sim.initialized)
                    {
                         ImGui::Separator();
                         net_tooltip_line("gate:", tr->gate_net);
                         net_tooltip_line("s1:  ", tr->s1_net);
                         net_tooltip_line("s2:  ", tr->s2_net);
                    }
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

     /* ── Transistor Inspector window (persistent, appears on click) ── */
     if (s_die_sel.type == SM83_SEL_TRANSISTOR && s_die_sel.index >= 0)
          s_show_trans_inspect = true;
     if (s_die_sel.type == SM83_SEL_NONE)
          s_show_trans_inspect = false;

     if (s_show_trans_inspect && s_die_sel.type == SM83_SEL_TRANSISTOR)
     {
          rebuild_inspect_neighbours(s_die_sel.index);
          ImGui::SetNextWindowSize(ImVec2(340.0f, 420.0f), ImGuiCond_FirstUseEver);
          ImGui::SetNextWindowPos(ImVec2(canvas_pos.x + canvas_size.x - 350.0f,
                                         canvas_pos.y + 10.0f),
                                  ImGuiCond_FirstUseEver);
          bool open = true;
          if (ImGui::Begin("Transistor Inspector", &open,
                           ImGuiWindowFlags_NoCollapse))
          {
               const Sm83Transistor *tr = &sm83_transistors[s_die_sel.index];
               bool sim_ready = s_netlist_sim.enabled && s_netlist_sim.initialized;

               static const char *sim_state_names[] = {
                   "UNKNOWN", "FLOAT", "LOW~", "HIGH~", "LOW", "HIGH", "CONFLICT"};
               static const ImVec4 sim_state_cols[] = {
                   ImVec4(0.5f, 0.5f, 0.5f, 1.0f), /* UNKNOWN  */
                   ImVec4(0.6f, 0.6f, 0.7f, 1.0f), /* FLOAT    */
                   ImVec4(0.4f, 0.9f, 0.4f, 1.0f), /* LOW~     */
                   ImVec4(0.9f, 0.4f, 0.4f, 1.0f), /* HIGH~    */
                   ImVec4(0.2f, 1.0f, 0.2f, 1.0f), /* LOW      */
                   ImVec4(1.0f, 0.3f, 0.3f, 1.0f), /* HIGH     */
                   ImVec4(1.0f, 0.1f, 1.0f, 1.0f), /* CONFLICT */
               };
               static const char *conf_names[] = {"UNKNOWN", "PROXY", "PROBABLE", "CONFIRMED"};
               static const ImVec4 conf_cols[] = {
                   ImVec4(0.5f, 0.5f, 0.5f, 1.0f), /* UNKNOWN   */
                   ImVec4(0.8f, 0.6f, 0.2f, 1.0f), /* PROXY     */
                   ImVec4(0.4f, 0.8f, 1.0f, 1.0f), /* PROBABLE  */
                   ImVec4(0.3f, 1.0f, 0.5f, 1.0f), /* CONFIRMED */
               };
               static const char *sig_names[] = {
                   "PCL", "PCH", "A", "B", "C", "D", "E", "H", "L",
                   "SPL", "SPH", "IR", "FZ", "FN", "FH", "FC", "DBUS", "IDU", "VCC", "GND"};

               /* ── Header ── */
               Sm83SimState gate_st = sim_ready
                                          ? sm83_sim_net_state(&s_netlist_sim, tr->gate_net)
                                          : SM83_SIM_UNKNOWN;
               bool conducts = (tr->layer == SM83_LAYER_NTRANS)
                                   ? SM83_SIM_IS_HIGH(gate_st)
                                   : SM83_SIM_IS_LOW(gate_st);
               const char *type_str = (tr->layer == SM83_LAYER_NTRANS) ? "NMOS" : "PMOS";
               ImU32 hdr_col = conducts ? IM_COL32(80, 220, 80, 255) : IM_COL32(120, 120, 140, 255);
               ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(hdr_col),
                                  "%s #%d  %s", type_str, s_die_sel.index,
                                  conducts ? "CONDUCTING" : "off");
               ImGui::Text("pos: (%.1f, %.1f)  nx=%.3f ny=%.3f", tr->x, tr->y, tr->nx, tr->ny);

               /* Functional region from normalized position */
               {
                    struct
                    {
                         const char *label;
                         float nx0, ny0, nx1, ny1;
                    } region_defs[] = {
                        {"IR", 0.118f, 0.035f, 0.150f, 0.456f},
                        {"A", 0.150f, 0.035f, 0.208f, 0.456f},
                        {"L", 0.208f, 0.060f, 0.232f, 0.456f},
                        {"H", 0.232f, 0.035f, 0.278f, 0.456f},
                        {"E", 0.278f, 0.060f, 0.300f, 0.456f},
                        {"D", 0.300f, 0.035f, 0.343f, 0.456f},
                        {"C", 0.343f, 0.060f, 0.368f, 0.456f},
                        {"B", 0.368f, 0.060f, 0.403f, 0.456f},
                        {"W/Z", 0.403f, 0.060f, 0.522f, 0.456f},
                        {"SP", 0.533f, 0.035f, 0.645f, 0.456f},
                        {"PC", 0.645f, 0.060f, 0.722f, 0.456f},
                        {"IDU", 0.722f, 0.035f, 0.860f, 0.476f},
                        {"IE", 0.820f, 0.058f, 0.845f, 0.448f},
                        {"IRQ", 0.790f, 0.030f, 0.912f, 0.476f},
                        {"Flags", 0.100f, 0.568f, 0.356f, 0.592f},
                        {"ALU", 0.090f, 0.558f, 0.363f, 0.920f},
                        {"DBUS", 0.007f, 0.595f, 0.329f, 0.937f},
                        {"Dec 1", 0.415f, 0.883f, 0.888f, 0.982f},
                        {"Dec 2", 0.408f, 0.782f, 0.845f, 0.810f},
                        {"Dec 3", 0.432f, 0.567f, 0.938f, 0.726f},
                    };
                    const char *region = NULL;
                    for (int ri = 0; ri < (int)(sizeof(region_defs) / sizeof(region_defs[0])); ri++)
                    {
                         auto &rd = region_defs[ri];
                         if (tr->nx >= rd.nx0 && tr->nx <= rd.nx1 &&
                             tr->ny >= rd.ny0 && tr->ny <= rd.ny1)
                         {
                              region = rd.label;
                              break;
                         }
                    }
                    /* Nearest instance */
                    float best_dist = 1e9f;
                    const char *nearest_inst = NULL;
                    for (int ii = 0; ii < SM83_INSTANCE_COUNT; ii++)
                    {
                         float dx = sm83_instances[ii].x - tr->x;
                         float dy = sm83_instances[ii].y - tr->y;
                         float d = dx * dx + dy * dy;
                         if (d < best_dist)
                         {
                              best_dist = d;
                              nearest_inst = sm83_instances[ii].name;
                         }
                    }
                    if (region)
                         ImGui::TextDisabled("region: %s", region);
                    else
                         ImGui::TextDisabled("region: (decoder interior)");
                    if (nearest_inst)
                         ImGui::TextDisabled("nearest: %s  (%.0f u)", nearest_inst, sqrtf(best_dist));
               }
               ImGui::Separator();

               /* ── Helper: draw one net row with sim state + semantic mapping ── */
               auto net_row = [&](const char *role, int net_id)
               {
                    if (net_id < 0 || net_id >= SM83_NET_COUNT)
                    {
                         ImGui::TextDisabled("%s  —", role);
                         return;
                    }
                    Sm83SimState st = sim_ready
                                          ? sm83_sim_net_state(&s_netlist_sim, net_id)
                                          : SM83_SIM_UNKNOWN;
                    int si = (int)st < SM83_SIM_STATE_COUNT ? (int)st : 0;

                    /* role + net name */
                    ImGui::TextDisabled("%s", role);
                    ImGui::SameLine();
                    ImGui::Text("#%d %s", net_id, sm83_nets[net_id]);
                    ImGui::SameLine();
                    ImGui::TextColored(sim_state_cols[si], "[%s]", sim_state_names[si]);

                    /* semantic mapping for this net */
                    const Sm83NetSemanticEntry *se =
                        sm83_semantic_map_find(net_id, SM83_CONF_UNKNOWN);
                    if (se && se->signal < SM83_SEM_COUNT)
                    {
                         int ci = (int)se->confidence <= 3 ? (int)se->confidence : 0;
                         int sni = (int)se->signal < 20 ? (int)se->signal : 19;
                         ImGui::TextColored(conf_cols[ci],
                                            "      → %s[%d]  (%s)",
                                            sig_names[sni], se->bit,
                                            conf_names[ci]);
                    }
               };

               ImGui::Text("Nets:");
               net_row("gate:", tr->gate_net);
               net_row("s1:  ", tr->s1_net);
               net_row("s2:  ", tr->s2_net);

               /* ── Cross-link to schematic ── */
               ImGui::Spacing();
               ImGui::TextDisabled("Jump to schematic net:");
               ImGui::SameLine();

               /* Helper: one button per net that has a schematic mapping */
               auto cross_btn = [&](const char *label, int sm83_net_id)
               {
                    int sch_id = die_net_to_sch_net_id(sm83_net_id);
                    if (sch_id < 0)
                         return; /* no mapping */
                    const HwNetSemantic *sem = hw_map_find_net(sch_id);
                    char btn_lbl[32];
                    snprintf(btn_lbl, sizeof(btn_lbl), "%s: %s##xlink_%s",
                             label,
                             sem && sem->canonical_name ? sem->canonical_name : "?",
                             label);
                    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(40,  80, 160, 220));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(60, 120, 220, 240));
                    if (ImGui::Button(btn_lbl))
                         sch_jump_to_net(sch_id, s_sch_last_canvas_size);
                    ImGui::PopStyleColor(2);
                    ImGui::SameLine();
               };

               cross_btn("gate", tr->gate_net);
               cross_btn("s1",   tr->s1_net);
               cross_btn("s2",   tr->s2_net);
               ImGui::NewLine();
               ImGui::Separator();

               /* ── Neighbours: transistors sharing any of gate/s1/s2 ── */
               ImGui::Text("Neighbours (shared nets, max 6 each):");
               ImGui::Spacing();

               for (int ri = 0; ri < 3; ri++)
               {
                    const TransNeighGroup *g = &s_inspect_neigh[ri];
                    if (g->net_id < 0)
                         continue;
                    ImGui::TextDisabled("  via %s (net #%d):", g->role, g->net_id);
                    if (g->count == 0)
                    {
                         ImGui::TextDisabled("    (none)");
                         continue;
                    }
                    for (int ni = 0; ni < g->count; ni++)
                    {
                         const Sm83Transistor *nb = &sm83_transistors[g->nb[ni].index];
                         const char *nb_type = (nb->layer == SM83_LAYER_NTRANS) ? "N" : "P";
                         ImGui::Text("    #%d %s [%s]", g->nb[ni].index, nb_type, g->nb[ni].via);
                    }
               }
          }
          ImGui::End();

          if (!open)
          {
               s_show_trans_inspect = false;
               s_die_sel.type = SM83_SEL_NONE;
               s_die_sel.index = -1;
          }
     }

     /* ── Trace Timeline: all CPU events with phase-aware sim seeding ── */
     if (gb && gb->debug.hw_trace.enabled)
     {
          static int s_trace_selected = -1; /* seq of selected event, -1 = live */
          static bool s_trace_open = true;
          static bool s_trace_all_ev = false; /* false = FETCH only, true = all */

          ImGui::Separator();
          ImGui::SetNextItemOpen(s_trace_open, ImGuiCond_Once);
          if ((s_trace_open = ImGui::CollapsingHeader("Trace Timeline")))
          {
               const struct gb_hw_trace *tr = &gb->debug.hw_trace;
               int total = (int)tr->count;

               /* "Live" button */
               bool is_live = (s_trace_selected < 0);
               if (is_live)
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(80, 200, 80, 200));
               else
                    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 50, 60, 200));
               ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(100, 220, 100, 200));
               if (ImGui::Button("Live##trace"))
               {
                    s_trace_selected = -1;
                    if (s_netlist_sim.enabled && s_netlist_sim.initialized && gb_paused)
                    {
                         sm83_sim_seed_from_gb(&s_netlist_sim, gb);
                         sm83_sim_step(&s_netlist_sim, 64);
                    }
               }
               ImGui::PopStyleColor(2);
               if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Seed sim from current emulator state (live)");

               ImGui::SameLine();
               ImGui::Checkbox("All events##trace_filter", &s_trace_all_ev);
               if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Show all trace events (not just FETCH).\n"
                                      "Non-FETCH events trigger phase-aware partial seed.");

               /* Collect events (most recent first, up to 96) */
               const int MAX_SHOW = 96;
               int ev_indices[MAX_SHOW];
               int ev_count = 0;
               for (int i = total - 1; i >= 0 && ev_count < MAX_SHOW; i--)
               {
                    int idx = (int)((tr->head - 1 - i + GB_HW_TRACE_CAP) & (GB_HW_TRACE_CAP - 1));
                    gb_hw_trace_event_type t = tr->events[idx].type;
                    bool want = s_trace_all_ev
                                    ? (t == GB_HW_EVT_CPU_FETCH || t == GB_HW_EVT_CPU_READ ||
                                       t == GB_HW_EVT_CPU_WRITE || t == GB_HW_EVT_CPU_ALU ||
                                       t == GB_HW_EVT_CPU_WRITEBACK)
                                    : (t == GB_HW_EVT_CPU_FETCH);
                    if (want)
                         ev_indices[ev_count++] = idx;
               }

               /* Color legend for event types */
               static const ImVec4 col_fetch = {0.47f, 0.70f, 1.00f, 1.0f};
               static const ImVec4 col_read = {0.40f, 0.85f, 0.60f, 1.0f};
               static const ImVec4 col_write = {1.00f, 0.55f, 0.35f, 1.0f};
               static const ImVec4 col_alu = {1.00f, 0.85f, 0.30f, 1.0f};
               static const ImVec4 col_writeback = {0.80f, 0.50f, 1.00f, 1.0f};

               float list_h = ImGui::GetTextLineHeightWithSpacing() * 7.0f;
               ImGui::BeginChild("##die_trace_list", ImVec2(0, list_h), true);
               for (int fi = 0; fi < ev_count; fi++)
               {
                    int idx = ev_indices[fi];
                    const gb_hw_trace_event *ev = &tr->events[idx];
                    bool selected = ((int)ev->seq == s_trace_selected);

                    /* Choose color and label by event type */
                    char label[96];
                    ImVec4 col = col_fetch;
                    switch (ev->type)
                    {
                    case GB_HW_EVT_CPU_FETCH:
                         col = col_fetch;
                         snprintf(label, sizeof(label),
                                  "#%-5llu FETCH  PC=%04X %02X  A=%02X F=%c%c%c%c",
                                  (unsigned long long)ev->seq,
                                  ev->pc, ev->opcode, ev->snap_a,
                                  (ev->snap_flags & 8) ? 'Z' : '-',
                                  (ev->snap_flags & 4) ? 'N' : '-',
                                  (ev->snap_flags & 2) ? 'H' : '-',
                                  (ev->snap_flags & 1) ? 'C' : '-');
                         break;
                    case GB_HW_EVT_CPU_READ:
                         col = col_read;
                         snprintf(label, sizeof(label),
                                  "#%-5llu READ   [%04X]=%02X",
                                  (unsigned long long)ev->seq, ev->addr, ev->data);
                         break;
                    case GB_HW_EVT_CPU_WRITE:
                         col = col_write;
                         snprintf(label, sizeof(label),
                                  "#%-5llu WRITE  [%04X]=%02X",
                                  (unsigned long long)ev->seq, ev->addr, ev->data);
                         break;
                    case GB_HW_EVT_CPU_ALU:
                         col = col_alu;
                         snprintf(label, sizeof(label),
                                  "#%-5llu ALU    op=%d res=%02X F:%02X->%02X",
                                  (unsigned long long)ev->seq,
                                  ev->extra, ev->data,
                                  (uint8_t)(ev->addr & 0xFF),
                                  (uint8_t)(ev->addr >> 8));
                         break;
                    case GB_HW_EVT_CPU_WRITEBACK:
                         col = col_writeback;
                         snprintf(label, sizeof(label),
                                  "#%-5llu WBACK  dst=%d val=%02X/%04X",
                                  (unsigned long long)ev->seq,
                                  ev->extra, ev->data, ev->addr);
                         break;
                    default:
                         snprintf(label, sizeof(label), "#%-5llu type=%d",
                                  (unsigned long long)ev->seq, (int)ev->type);
                         break;
                    }

                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    if (ImGui::Selectable(label, selected))
                    {
                         s_trace_selected = (int)ev->seq;
                         if (s_netlist_sim.enabled && s_netlist_sim.initialized)
                         {
                              if (ev->type == GB_HW_EVT_CPU_FETCH)
                              {
                                   /* Full register snapshot seed */
                                   Sm83CpuSnapshot snap = {0};
                                   snap.pc = ev->pc;
                                   snap.a = ev->snap_a;
                                   snap.b = ev->snap_b;
                                   snap.c = ev->snap_c;
                                   snap.d = ev->snap_d;
                                   snap.e = ev->snap_e;
                                   snap.h = ev->snap_h;
                                   snap.l = ev->snap_l;
                                   snap.sp = ev->snap_sp;
                                   snap.flags = ev->snap_flags;
                                   snap.ir = ev->snap_ir;
                                   snap.dbus = ev->snap_dbus;
                                   snap.alu_op = ev->snap_alu_op;
                                   snap.src = ev->snap_src;
                                   snap.dst = ev->snap_dst;
                                   sm83_sim_seed_from_snapshot(&s_netlist_sim, &snap);
                              }
                              else
                              {
                                   /* Phase-aware partial seed: only update changed nets */
                                   uint8_t ev_flags = 0;
                                   if (ev->type == GB_HW_EVT_CPU_ALU)
                                        ev_flags = (uint8_t)(ev->addr >> 8); /* flags_after */
                                   sm83_sim_phase_seed(&s_netlist_sim,
                                                       (int)ev->type,
                                                       ev->addr,
                                                       ev->data,
                                                       ev->extra,
                                                       ev_flags,
                                                       nullptr);
                              }
                              sm83_sim_step(&s_netlist_sim, 64);
                         }
                    }
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered())
                    {
                         if (ev->type == GB_HW_EVT_CPU_FETCH)
                              ImGui::SetTooltip(
                                  "FETCH  seq=%llu  PC=%04X  SP=%04X\n"
                                  "A=%02X B=%02X C=%02X D=%02X E=%02X H=%02X L=%02X\n"
                                  "flags: Z=%d N=%d H=%d C=%d\n"
                                  "IR=%02X  DBUS=%02X  alu=%d src=%d dst=%d",
                                  (unsigned long long)ev->seq,
                                  ev->pc, ev->snap_sp,
                                  ev->snap_a, ev->snap_b, ev->snap_c,
                                  ev->snap_d, ev->snap_e, ev->snap_h, ev->snap_l,
                                  (ev->snap_flags >> 3) & 1,
                                  (ev->snap_flags >> 2) & 1,
                                  (ev->snap_flags >> 1) & 1,
                                  (ev->snap_flags >> 0) & 1,
                                  ev->snap_ir, ev->snap_dbus,
                                  ev->snap_alu_op, ev->snap_src, ev->snap_dst);
                         else
                              ImGui::SetTooltip(
                                  "seq=%llu  type=%d  PC=%04X\n"
                                  "addr=%04X  data=%02X  extra=%02X\n"
                                  "Phase seed: only nets relevant to this event are updated.",
                                  (unsigned long long)ev->seq, (int)ev->type,
                                  ev->pc, ev->addr, ev->data, ev->extra);
                    }
               }
               if (ev_count == 0)
                    ImGui::TextDisabled("(no events — enable trace)");
               ImGui::EndChild();
          }
     }

     /* ── Info bar ── */
     ImGui::Separator();
     float fps = ImGui::GetIO().Framerate;
     if (s_netlist_sim.enabled && !s_netlist_sim.rails_found)
          ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.2f, 1.0f),
                             "arcs:%d  trans:%d  %.0f fps  |  sim: NO RAILS — cannot propagate",
                             s_drawn_arcs, s_drawn_trans, fps);
     else if (s_netlist_sim.enabled)
     {
          bool converged = s_netlist_sim.iterations < 64;
          bool heuristic = s_netlist_sim.rail_source == SM83_RAILS_HEURISTIC;
          const char *rail_tag = heuristic ? " [rails:heuristic]" : "";
          if (converged)
               ImGui::TextDisabled("arcs:%d  trans:%d  %.0f fps  |  sim: %d iters%s  nets:%d",
                                   s_drawn_arcs, s_drawn_trans, fps,
                                   s_netlist_sim.iterations, rail_tag, SM83_NET_COUNT);
          else
               ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.1f, 1.0f),
                                  "arcs:%d  trans:%d  %.0f fps  |  sim: DIVERGED (64 iters)%s  nets:%d",
                                  s_drawn_arcs, s_drawn_trans, fps, rail_tag, SM83_NET_COUNT);
     }
     else if (s_highlight_net_id >= 0 && s_highlight_net_id < SM83_NET_COUNT)
          ImGui::TextDisabled("arcs:%d  trans:%d  %.0f fps  |  net #%d: %s",
                              s_drawn_arcs, s_drawn_trans, fps,
                              s_highlight_net_id, sm83_nets[s_highlight_net_id]);
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

static HwSchematicView s_sch_view = {};
static bool s_sch_view_init = false;
static bool s_sch_show_wires = true;
static bool s_sch_show_junctions = true;
static bool s_sch_show_labels = true;
static bool s_sch_show_components = true;
static bool s_sch_show_activity = true;
static bool s_sch_paper_mode = false;

/* ── Projection activity state (Fase D) ── */
static HwSchematicActivityState s_hw_activity;
static bool s_hw_activity_init = false;
static uint64_t s_hw_last_seq = 0; /* highest event seq consumed */

/* Per-animation-group fade value (driven by sys_viz each frame) */
static float s_sch_anim_fade[HW_ANIM_GROUP_COUNT] = {};

/* Idle color for each anim group (inactive state) */
static const ImU32 SCH_IDLE_COLOR[HW_ANIM_GROUP_COUNT] = {
    IM_COL32(80, 90, 130, 140),  /* 0  addr_bus    — blue-grey */
    IM_COL32(80, 90, 130, 140),  /* 1  data_bus    — blue-grey */
    IM_COL32(60, 110, 80, 140),  /* 2  wram_data   — green-grey */
    IM_COL32(60, 110, 80, 130),  /* 3  wram_addr   — green-grey */
    IM_COL32(100, 80, 150, 100), /* 4  clock       — dim violet */
    IM_COL32(100, 70, 40, 120),  /* 5  audio       — dim orange */
    IM_COL32(40, 80, 60, 120),   /* 6  lcd         — dim teal */
    IM_COL32(80, 50, 110, 120),  /* 7  irq         — dim purple */
    IM_COL32(60, 40, 40, 100),   /* 8  power       — dim red */
    IM_COL32(40, 80, 100, 100),  /* 9  serial      — dim cyan */
    IM_COL32(80, 80, 50, 100),   /* 10 bus_ctrl    — dim gold */
};

/* Active color for each anim group (when fade > 0) */
static const ImU32 SCH_ACTIVE_COLOR[HW_ANIM_GROUP_COUNT] = {
    IM_COL32(80, 160, 255, 255),  /* 0  addr_bus    — bright blue */
    IM_COL32(100, 180, 255, 255), /* 1  data_bus    — light blue */
    IM_COL32(60, 220, 120, 255),  /* 2  wram_data   — bright green */
    IM_COL32(80, 200, 100, 230),  /* 3  wram_addr   — green */
    IM_COL32(200, 140, 255, 255), /* 4  clock       — violet */
    IM_COL32(255, 160, 60, 255),  /* 5  audio       — orange */
    IM_COL32(60, 220, 160, 255),  /* 6  lcd         — teal */
    IM_COL32(200, 100, 255, 255), /* 7  irq         — purple */
    IM_COL32(255, 80, 80, 200),   /* 8  power       — red */
    IM_COL32(60, 200, 220, 255),  /* 9  serial      — cyan */
    IM_COL32(220, 200, 60, 220),  /* 10 bus_ctrl    — gold */
};

/* Colors indexed by HwSignalKind (16 entries) */
static const ImU32 SCH_KIND_COLOR[16] = {
    IM_COL32(80, 140, 255, 200),  /* 0  ADDR       — blue */
    IM_COL32(80, 220, 110, 200),  /* 1  DATA       — green */
    IM_COL32(255, 90, 90, 220),   /* 2  CTRL_RD    — red */
    IM_COL32(255, 130, 50, 220),  /* 3  CTRL_WR    — orange */
    IM_COL32(220, 200, 60, 220),  /* 4  CTRL_CS    — gold */
    IM_COL32(200, 140, 255, 200), /* 5  CLOCK      — violet */
    IM_COL32(255, 50, 50, 240),   /* 6  RESET      — bright red */
    IM_COL32(220, 100, 255, 220), /* 7  IRQ        — purple */
    IM_COL32(60, 210, 180, 200),  /* 8  LCD        — teal */
    IM_COL32(255, 160, 60, 200),  /* 9  AUDIO      — amber */
    IM_COL32(60, 200, 230, 200),  /* 10 SERIAL     — cyan */
    IM_COL32(200, 230, 80, 200),  /* 11 JOYPAD     — lime */
    IM_COL32(255, 80, 80, 160),   /* 12 POWER      — red-dim */
    IM_COL32(120, 190, 255, 190), /* 13 WRAM_ADDR  — light blue */
    IM_COL32(100, 240, 140, 190), /* 14 WRAM_DATA  — light green */
    IM_COL32(70, 75, 95, 100),    /* 15 UNKNOWN    — very dim */
};

static bool s_sch_show_kinds = true;
static bool s_sch_show_levels = true; /* net 0/1 labels on wires */

/* ── Frozen frame ── */
static bool s_sch_frozen = false;
static HwSchematicActivityState s_hw_frozen; /* snapshot taken at freeze time */

static ImU32 sch_wire_color(int anim_group)
{
     if (anim_group < 0 || anim_group >= HW_ANIM_GROUP_COUNT)
          return IM_COL32(70, 75, 95, 120); /* unlabeled — very dim */

     float fade = s_sch_anim_fade[anim_group];
     if (fade <= 0.0f)
          return SCH_IDLE_COLOR[anim_group];

     float t = fade > 1.0f ? 1.0f : fade;
     ImU32 idle = SCH_IDLE_COLOR[anim_group];
     ImU32 active = SCH_ACTIVE_COLOR[anim_group];
     uint8_t r = (uint8_t)(((idle >> IM_COL32_R_SHIFT) & 0xFF) * (1 - t) + ((active >> IM_COL32_R_SHIFT) & 0xFF) * t);
     uint8_t g = (uint8_t)(((idle >> IM_COL32_G_SHIFT) & 0xFF) * (1 - t) + ((active >> IM_COL32_G_SHIFT) & 0xFF) * t);
     uint8_t b = (uint8_t)(((idle >> IM_COL32_B_SHIFT) & 0xFF) * (1 - t) + ((active >> IM_COL32_B_SHIFT) & 0xFF) * t);
     uint8_t a = (uint8_t)(((idle >> IM_COL32_A_SHIFT) & 0xFF) * (1 - t) + ((active >> IM_COL32_A_SHIFT) & 0xFF) * t);
     return IM_COL32(r, g, b, a);
}

static ImU32 sch_wire_color_kind(int net_id)
{
     const HwNetSemantic *sem = hw_map_find_net(net_id);
     HwSignalKind kind = sem ? sem->kind : HW_SIG_UNKNOWN;
     return SCH_KIND_COLOR[kind];
}

static void sch_component_reference_body(const HwComponent *comp,
                                         float *nx, float *ny,
                                         float *nw, float *nh)
{
     *nx = comp->nx;
     *ny = comp->ny;
     *nw = comp->nw;
     *nh = comp->nh;

     if (strcmp(comp->ref, "U1") == 0)
     {
          *nx = 0.285000f;
          *ny = 0.508991f;
          *nw = 0.090080f;
          *nh = 0.612979f;
     }
     else if (strcmp(comp->ref, "U2") == 0)
     {
          *nx = 0.509651f;
          *ny = 0.315090f;
          *nw = 0.053083f;
          *nh = 0.184519f;
     }
     else if (strcmp(comp->ref, "U3") == 0)
     {
          *nx = 0.734316f;
          *ny = 0.655981f;
          *nw = 0.053083f;
          *nh = 0.184519f;
     }
     else if (strcmp(comp->ref, "P1") == 0)
     {
          *nx = 0.894638f;
          *ny = 0.535966f;
          *nw = 0.016622f;
          *nh = 0.438624f;
     }
     else if (strcmp(comp->ref, "J2") == 0)
     {
          *nx = 0.555228f;
          *ny = 0.682565f;
          *nw = 0.006971f;
          *nh = 0.278342f;
     }
}

/* ── Cross-link: die transistor net → schematic net ──────────────────────────
 * Converts a sm83 netlist net_id to a hw_schematic net_id by:
 *   1. Looking up the semantic entry (signal + bit)
 *   2. Building the canonical name  (e.g. "A0", "D3", "MA5")
 *   3. Linear-scanning hw_net_map[] for a matching canonical_name
 * Returns -1 if no mapping found.
 * --------------------------------------------------------------------------- */
static int die_net_to_sch_net_id(int sm83_net_id)
{
     if (sm83_net_id < 0 || sm83_net_id >= SM83_NET_COUNT)
          return -1;

     const Sm83NetSemanticEntry *se =
         sm83_semantic_map_find(sm83_net_id, SM83_CONF_UNKNOWN);
     if (!se || se->signal >= SM83_SEM_COUNT)
          return -1;

     /* Build canonical name for the signal bit */
     static const char *const sig_prefix[] = {
         "PC", "PC", "A", "B", "C", "D", "E", "H", "L",
         "SP", "SP", "IR",
         "FZ", "FN", "FH", "FC",
         "D", "IDU", "VCC", "GND",
     };
     /* Address bus and data bus use different prefix conventions */
     char canon[16];
     int sni = (int)se->signal;
     if (sni == 0)       /* PCL → PC low byte, not in schematic directly */
          return -1;
     else if (sni == 16) /* DBUS → D0..D7 */
          snprintf(canon, sizeof(canon), "D%d", se->bit);
     else if (sni == 17) /* IDU → no direct schematic net */
          return -1;
     else
          snprintf(canon, sizeof(canon), "%s%d", sig_prefix[sni], se->bit);

     /* Search hw_net_map for matching canonical_name */
     for (int i = 0; i < hw_net_map_count; i++)
     {
          if (hw_net_map[i].canonical_name &&
              strcmp(hw_net_map[i].canonical_name, canon) == 0)
               return hw_net_map[i].net_id;
     }
     return -1;
}

/* Schematic selection & cross-link state (file-scope so die panel can access) */
int    s_sch_sel_net          = -1;            /* selected net_id, -1 = none */
int    s_sch_cross_net        = -1;            /* net_id to flash, -1 = none */
float  s_sch_cross_timer      = 0.0f;          /* seconds remaining */
ImVec2 s_sch_last_canvas_size = {800.0f, 500.0f}; /* updated each schematic frame */

/* Navigate schematic view to centre on a net and start flash */
static void sch_jump_to_net(int hw_net_id, ImVec2 canvas_size)
{
     if (hw_net_id < 0)
          return;
     s_sch_sel_net    = hw_net_id;
     s_sch_cross_net  = hw_net_id;
     s_sch_cross_timer = 2.0f;

     /* Compute bounding box of all wires on this net */
     float nx0 = 1e9f, ny0 = 1e9f, nx1 = -1e9f, ny1 = -1e9f;
     for (int i = 0; i < HW_WIRE_COUNT; i++)
     {
          if (hw_wires[i].net_id != hw_net_id)
               continue;
          const HwWire *w = &hw_wires[i];
          if (w->nx1 < nx0) nx0 = w->nx1;
          if (w->nx2 < nx0) nx0 = w->nx2;
          if (w->ny1 < ny0) ny0 = w->ny1;
          if (w->ny2 < ny0) ny0 = w->ny2;
          if (w->nx1 > nx1) nx1 = w->nx1;
          if (w->nx2 > nx1) nx1 = w->nx2;
          if (w->ny1 > ny1) ny1 = w->ny1;
          if (w->ny2 > ny1) ny1 = w->ny2;
     }
     if (nx1 <= nx0 || ny1 <= ny0)
          return;

     float pad = 0.04f;
     nx0 -= pad; ny0 -= pad; nx1 += pad; ny1 += pad;
     float zx = canvas_size.x / (nx1 - nx0);
     float zy = canvas_size.y / ((ny1 - ny0) * HW_SCHEMATIC_ASPECT);
     s_sch_view.zoom = zx < zy ? zx : zy;
     if (s_sch_view.zoom > 8000.0f) s_sch_view.zoom = 8000.0f;
     s_sch_view.pan_x = (nx0 + nx1) * 0.5f
                        - canvas_size.x * 0.5f / s_sch_view.zoom;
     s_sch_view.pan_y = (ny0 + ny1) * 0.5f
                        - canvas_size.y * 0.5f / (s_sch_view.zoom * HW_SCHEMATIC_ASPECT);
}

void draw_panel_hw_schematic(struct gb *gb)
{
     float dt = ImGui::GetIO().DeltaTime;

     /* ── Cross-link flash timer decay ── */
     if (s_sch_cross_timer > 0.0f)
     {
          s_sch_cross_timer -= dt;
          if (s_sch_cross_timer <= 0.0f)
          {
               s_sch_cross_timer = 0.0f;
               s_sch_cross_net   = -1;
          }
     }

     /* ── Decay + map emulator fades to anim groups ── */
     struct gb_sys_viz *sv = gb ? &gb->debug.sys_viz : nullptr;
     if (sv)
     {
          float decay = dt * 8.0f;
#define SCH_DECAY(x) ((x) > decay ? (x) - decay : 0.0f)
          sv->fade_cpu_rom = SCH_DECAY(sv->fade_cpu_rom);
          sv->fade_cpu_wram = SCH_DECAY(sv->fade_cpu_wram);
          sv->fade_cpu_vram = SCH_DECAY(sv->fade_cpu_vram);
          sv->fade_cpu_oam = SCH_DECAY(sv->fade_cpu_oam);
          sv->fade_cpu_io = SCH_DECAY(sv->fade_cpu_io);
          sv->fade_dma_oam = SCH_DECAY(sv->fade_dma_oam);
          sv->fade_ppu_vram = SCH_DECAY(sv->fade_ppu_vram);
          sv->fade_irq_cpu = SCH_DECAY(sv->fade_irq_cpu);
          sv->fade_apu = SCH_DECAY(sv->fade_apu);
#undef SCH_DECAY

          /* Map sys_viz fades to anim groups.
           * addr/data driven by ROM access (CPU-side bus activity).
           * wram_data/wram_addr driven by WRAM access.
           * bus_ctrl and lcd also pulse on ROM/WRAM access. */
          float f_bus = sv->fade_cpu_rom;
          float f_wram = sv->fade_cpu_wram;
          float f_vram = sv->fade_cpu_vram > sv->fade_ppu_vram ? sv->fade_cpu_vram : sv->fade_ppu_vram;
          s_sch_anim_fade[HW_ANIM_ADDR] = f_bus;
          s_sch_anim_fade[HW_ANIM_DATA] = f_bus;
          s_sch_anim_fade[HW_ANIM_WRAM_DATA] = f_wram;
          s_sch_anim_fade[HW_ANIM_WRAM_ADDR] = f_wram;
          s_sch_anim_fade[HW_ANIM_CLOCK] = 0.4f; /* clock always on, dim */
          s_sch_anim_fade[HW_ANIM_AUDIO] = sv->fade_apu;
          s_sch_anim_fade[HW_ANIM_LCD] = f_vram;
          s_sch_anim_fade[HW_ANIM_IRQ] = sv->fade_irq_cpu;
          s_sch_anim_fade[HW_ANIM_POWER] = 0.2f; /* power always on, very dim */
          s_sch_anim_fade[HW_ANIM_SERIAL] = 0.0f;
          s_sch_anim_fade[HW_ANIM_BUS_CTRL] = f_bus > f_wram ? f_bus : f_wram;
     }

     /* ── Projection activity state init + tick + consume ── */
     if (!s_hw_activity_init)
     {
          hw_activity_reset(&s_hw_activity);
          s_hw_activity_init = true;
     }
     hw_activity_tick(&s_hw_activity, dt, 3.0f);
     if (gb && gb->debug.hw_trace.enabled)
          hw_activity_consume_trace(&s_hw_activity, &gb->debug.hw_trace, &s_hw_last_seq);

     /* When frozen, all rendering reads from the snapshot instead of live state */
     const HwSchematicActivityState *act = s_sch_frozen ? &s_hw_frozen : &s_hw_activity;

     ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 4));
     if (ImGui::Button("Fit"))
          hw_schematic_view_fit(&s_sch_view);
     ImGui::SameLine(0, 6);
     if (ImGui::Button("Reset"))
     {
          s_sch_view.pan_x = 0.0f;
          s_sch_view.pan_y = 0.0f;
          hw_schematic_view_fit(&s_sch_view);
     }
     ImGui::SameLine(0, 16);
     ImGui::Checkbox("Paper", &s_sch_paper_mode);
     ImGui::SameLine(0, 14);
     ImGui::Checkbox("Wires", &s_sch_show_wires);
     ImGui::SameLine(0, 14);
     ImGui::Checkbox("Labels", &s_sch_show_labels);
     ImGui::SameLine(0, 14);
     ImGui::Checkbox("Junctions", &s_sch_show_junctions);
     ImGui::SameLine(0, 14);
     ImGui::Checkbox("Components", &s_sch_show_components);
     ImGui::SameLine(0, 16);
     if (!s_sch_paper_mode)
     {
          ImGui::Checkbox("Activity", &s_sch_show_activity);
          ImGui::SameLine(0, 14);
          ImGui::Checkbox("Kinds", &s_sch_show_kinds);
          ImGui::SameLine(0, 14);
          ImGui::Checkbox("0/1", &s_sch_show_levels);
          ImGui::SameLine(0, 16);
          /* Freeze button: captures current activity state */
          if (s_sch_frozen)
          {
               ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(80, 40, 10, 220));
               ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(120, 60, 15, 240));
               if (ImGui::Button("Unfreeze"))
                    s_sch_frozen = false;
               ImGui::PopStyleColor(2);
          }
          else
          {
               if (ImGui::Button("Freeze"))
               {
                    s_hw_frozen = s_hw_activity;
                    s_sch_frozen = true;
               }
          }
          ImGui::SameLine(0, 16);
     }
     ImGui::TextDisabled("zoom:%.0f", s_sch_view.zoom);
     ImGui::PopStyleVar();

     /* ── Canvas child (resizable — drag bottom border to resize) ── */
     static float s_canvas_child_h = 420.0f;
     ImVec2 total_avail = ImGui::GetContentRegionAvail();
     if (s_canvas_child_h > total_avail.y - 80.0f)
          s_canvas_child_h = total_avail.y - 80.0f;
     if (s_canvas_child_h < 200.0f)
          s_canvas_child_h = 200.0f;

     ImGui::BeginChild("##sch_canvas_child",
                       ImVec2(0.0f, s_canvas_child_h),
                       ImGuiChildFlags_ResizeY,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

     /* Sync stored height with the child's actual size after user resizes */
     s_canvas_child_h = ImGui::GetContentRegionAvail().y;

     /* ── Canvas ── */
     ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
     ImVec2 canvas_size = ImGui::GetContentRegionAvail();
     if (canvas_size.x < 400)
          canvas_size.x = 400;
     if (canvas_size.y < 120)
          canvas_size.y = 120;
     /* Persist canvas size so cross-link from die panel can use it */
     s_sch_last_canvas_size = canvas_size;

     if (!s_sch_view_init)
     {
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
     bool canvas_active = ImGui::IsItemActive();

     /* ── Zoom & pan ── */
     if (ImGui::IsItemHovered())
     {
          float wheel = ImGui::GetIO().MouseWheel;
          if (wheel != 0.0f)
          {
               ImVec2 mouse = ImGui::GetIO().MousePos;
               float mx_n = (mouse.x - canvas_pos.x) / s_sch_view.zoom + s_sch_view.pan_x;
               float sy = s_sch_view.zoom * HW_SCHEMATIC_ASPECT;
               float my_n = (mouse.y - canvas_pos.y) / sy + s_sch_view.pan_y;
               float factor = wheel > 0 ? 1.15f : (1.0f / 1.15f);
               s_sch_view.zoom = s_sch_view.zoom * factor;
               if (s_sch_view.zoom < 100.0f)
                    s_sch_view.zoom = 100.0f;
               if (s_sch_view.zoom > 20000.0f)
                    s_sch_view.zoom = 20000.0f;
               s_sch_view.pan_x = mx_n - (mouse.x - canvas_pos.x) / s_sch_view.zoom;
               s_sch_view.pan_y = my_n - (mouse.y - canvas_pos.y) / (s_sch_view.zoom * HW_SCHEMATIC_ASPECT);
          }
     }
     if (canvas_active && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
     {
          ImVec2 delta = ImGui::GetIO().MouseDelta;
          s_sch_view.pan_x -= delta.x / s_sch_view.zoom;
          s_sch_view.pan_y -= delta.y / (s_sch_view.zoom * HW_SCHEMATIC_ASPECT);
     }

     HwSchematicCache cache;
     hw_schematic_view_cache(&s_sch_view, &cache);
     ImDrawList *dl = ImGui::GetWindowDrawList();
     dl->PushClipRect(canvas_pos,
                      ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y), true);

     /* ── Coordinate helpers ── */
     auto SX = [&](float nx) -> float
     { return cache.origin_x + nx * cache.scale_x; };
     auto SY = [&](float ny) -> float
     { return cache.origin_y + ny * cache.scale_y; };
     auto SP = [&](float nx, float ny) -> ImVec2
     { return ImVec2(SX(nx), SY(ny)); };

     float zoom = s_sch_view.zoom;

     /* ── Background ── */
     if (s_sch_paper_mode)
     {
          /* outer margin — medium grey */
          dl->AddRectFilled(canvas_pos,
                            ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                            IM_COL32(160, 160, 165, 255));
          /* paper */
          ImVec2 paper_tl = SP(0.0f, 0.0f);
          ImVec2 paper_br = SP(1.0f, 1.0f);
          dl->AddRectFilled(paper_tl, paper_br, IM_COL32(255, 253, 245, 255));
          /* border (KiCad red frame) */
          dl->AddRect(paper_tl, paper_br, IM_COL32(180, 20, 20, 200), 0.0f, 0, 1.5f);
     }
     else
     {
          dl->AddRectFilled(canvas_pos,
                            ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                            IM_COL32(14, 16, 22, 255));
     }

     /* ── Grid lines — two-level adaptive (ref 50mm + fine 10mm) ── */
     if (zoom > 300.0f)
     {
          float ref_step_n = 50.0f / 297.0f;
          float fine_step_n = 10.0f / 297.0f;

          float ref_alpha = (zoom - 300.0f) / 400.0f;
          float fine_alpha = (zoom - 600.0f) / 600.0f;
          if (ref_alpha > 1.0f)
               ref_alpha = 1.0f;
          if (fine_alpha > 1.0f)
               fine_alpha = 1.0f;
          if (fine_alpha < 0.0f)
               fine_alpha = 0.0f;

          ImU32 ref_col = IM_COL32(40, 45, 60, (uint8_t)(ref_alpha * 120));
          ImU32 fine_col = IM_COL32(30, 35, 45, (uint8_t)(fine_alpha * 80));

          /* Reference grid (50mm) */
          float x0 = cache.vp_nx0 - fmodf(cache.vp_nx0, ref_step_n);
          for (float nx = x0; nx < cache.vp_nx1; nx += ref_step_n)
          {
               float sx = SX(nx);
               dl->AddLine(ImVec2(sx, canvas_pos.y), ImVec2(sx, canvas_pos.y + canvas_size.y), ref_col, 0.8f);
          }
          float y0 = cache.vp_ny0 - fmodf(cache.vp_ny0, ref_step_n);
          for (float ny = y0; ny < cache.vp_ny1; ny += ref_step_n)
          {
               float sy = SY(ny);
               dl->AddLine(ImVec2(canvas_pos.x, sy), ImVec2(canvas_pos.x + canvas_size.x, sy), ref_col, 0.8f);
          }

          /* Fine grid (10mm) */
          if (fine_alpha > 0.0f)
          {
               x0 = cache.vp_nx0 - fmodf(cache.vp_nx0, fine_step_n);
               for (float nx = x0; nx < cache.vp_nx1; nx += fine_step_n)
               {
                    float sx = SX(nx);
                    dl->AddLine(ImVec2(sx, canvas_pos.y), ImVec2(sx, canvas_pos.y + canvas_size.y), fine_col, 0.5f);
               }
               y0 = cache.vp_ny0 - fmodf(cache.vp_ny0, fine_step_n);
               for (float ny = y0; ny < cache.vp_ny1; ny += fine_step_n)
               {
                    float sy = SY(ny);
                    dl->AddLine(ImVec2(canvas_pos.x, sy), ImVec2(canvas_pos.x + canvas_size.x, sy), fine_col, 0.5f);
               }
          }
     }

     /* ── Wire thickness — three-level hierarchy ── */
     float wire_px = zoom * (0.4f / 297.0f); /* 0.4mm wire */
     if (wire_px < 0.8f)
          wire_px = 0.8f;
     if (wire_px > 3.0f)
          wire_px = 3.0f;
     float power_px = wire_px * 1.6f; /* power nets thicker */
     if (power_px > 5.0f)
          power_px = 5.0f;
     float bus_px = wire_px * 2.5f;
     if (bus_px > 7.0f)
          bus_px = 7.0f;

     /* ── Selection / hover state ── */
     static int s_sch_sel_comp = -1;
     static int s_sch_hover_wire = -1; /* wire index hovered this frame */
     int hover_comp = -1;
     ImVec2 mouse = ImGui::GetIO().MousePos;

     /* Hit-test wires for hover (pick closest within 5px, skip if canvas being dragged) */
     s_sch_hover_wire = -1;
     if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
     {
          float best_dist2 = 25.0f; /* 5px^2 */
          for (int i = 0; i < HW_WIRE_COUNT; i++)
          {
               const HwWire *w = &hw_wires[i];
               if (!hw_sch_line_in_viewport(&cache, w->nx1, w->ny1, w->nx2, w->ny2))
                    continue;
               /* segment-to-point distance^2 in screen space */
               float ax = SX(w->nx1), ay = SY(w->ny1);
               float bx = SX(w->nx2), by = SY(w->ny2);
               float px = mouse.x, py = mouse.y;
               float dx = bx - ax, dy = by - ay;
               float len2 = dx * dx + dy * dy;
               float t2 = 0.0f;
               if (len2 > 0.0f)
               {
                    t2 = ((px - ax) * dx + (py - ay) * dy) / len2;
                    if (t2 < 0.0f)
                         t2 = 0.0f;
                    if (t2 > 1.0f)
                         t2 = 1.0f;
               }
               float cx2 = ax + t2 * dx - px, cy2 = ay + t2 * dy - py;
               float d2 = cx2 * cx2 + cy2 * cy2;
               if (d2 < best_dist2)
               {
                    best_dist2 = d2;
                    s_sch_hover_wire = i;
               }
          }
     }

     /* Left-click on wire selects its net; clicking same net again deselects */
     if (s_sch_hover_wire >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
     {
          int nid = hw_wires[s_sch_hover_wire].net_id;
          s_sch_sel_net = (s_sch_sel_net == nid) ? -1 : nid;
          s_sch_sel_comp = -1; /* clear component selection */
     }

     /* Zoom-to-selected net (F key) */
     if (s_sch_sel_net >= 0 && ImGui::IsItemHovered() && ImGui::IsKeyPressed(ImGuiKey_F))
     {
          float nx_min = 1e9f, ny_min = 1e9f, nx_max = -1e9f, ny_max = -1e9f;
          for (int i = 0; i < HW_WIRE_COUNT; i++)
          {
               if (hw_wires[i].net_id != s_sch_sel_net)
                    continue;
               nx_min = hw_wires[i].nx1 < nx_min ? hw_wires[i].nx1 : nx_min;
               nx_max = hw_wires[i].nx1 > nx_max ? hw_wires[i].nx1 : nx_max;
               ny_min = hw_wires[i].ny1 < ny_min ? hw_wires[i].ny1 : ny_min;
               ny_max = hw_wires[i].ny1 > ny_max ? hw_wires[i].ny1 : ny_max;
               nx_min = hw_wires[i].nx2 < nx_min ? hw_wires[i].nx2 : nx_min;
               nx_max = hw_wires[i].nx2 > nx_max ? hw_wires[i].nx2 : nx_max;
               ny_min = hw_wires[i].ny2 < ny_min ? hw_wires[i].ny2 : ny_min;
               ny_max = hw_wires[i].ny2 > ny_max ? hw_wires[i].ny2 : ny_max;
          }
          if (nx_max > nx_min || ny_max > ny_min)
          {
               float pad = 0.05f;
               nx_min -= pad;
               ny_min -= pad;
               nx_max += pad;
               ny_max += pad;
               float zx = canvas_size.x / ((nx_max - nx_min) * 1.0f);
               float zy = canvas_size.y / ((ny_max - ny_min) * HW_SCHEMATIC_ASPECT);
               s_sch_view.zoom = zx < zy ? zx : zy;
               if (s_sch_view.zoom > 20000.0f)
                    s_sch_view.zoom = 20000.0f;
               s_sch_view.pan_x = (nx_min + nx_max) * 0.5f - canvas_size.x * 0.5f / s_sch_view.zoom;
               s_sch_view.pan_y = (ny_min + ny_max) * 0.5f - canvas_size.y * 0.5f / (s_sch_view.zoom * HW_SCHEMATIC_ASPECT);
          }
     }

     /* ── Wires & buses ── */
     for (int i = 0; i < HW_WIRE_COUNT; i++)
     {
          const HwWire *w = &hw_wires[i];
          if (!hw_sch_line_in_viewport(&cache, w->nx1, w->ny1, w->nx2, w->ny2))
               continue;

          bool net_selected = (s_sch_sel_net >= 0 && w->net_id == s_sch_sel_net);
          bool net_hover = (s_sch_hover_wire == i);

          ImU32 col;
          if (s_sch_paper_mode)
          {
               /* KiCad-like paper mode: green wires and blue buses. */
               col = w->is_bus ? IM_COL32(0, 20, 170, 230) : IM_COL32(0, 150, 0, 220);
          }
          else
          {
               /* Trace-driven projection: use per-net fade when trace is active */
               bool trace_active = gb && gb->debug.hw_trace.enabled && w->net_id >= 0 && w->net_id < HW_NET_COUNT && act->net_fade[w->net_id] > 0.01f;
               if (trace_active)
               {
                    /* Colour by event type of last touch */
                    float fade = act->net_fade[w->net_id];
                    gb_hw_trace_event_type lt = act->net_last_type[w->net_id];
                    ImU32 active_col;
                    switch (lt)
                    {
                    case GB_HW_EVT_CPU_FETCH:
                         active_col = IM_COL32(255, 220, 60, 255);
                         break; /* yellow */
                    case GB_HW_EVT_CPU_READ:
                         active_col = IM_COL32(60, 200, 255, 255);
                         break; /* cyan   */
                    case GB_HW_EVT_CPU_WRITE:
                         active_col = IM_COL32(255, 120, 60, 255);
                         break; /* orange */
                    case GB_HW_EVT_DMA_READ:
                    case GB_HW_EVT_DMA_WRITE:
                         active_col = IM_COL32(200, 80, 255, 255);
                         break; /* purple */
                    case GB_HW_EVT_IRQ_REQUEST:
                    case GB_HW_EVT_IRQ_ACK:
                         active_col = IM_COL32(255, 60, 60, 255);
                         break; /* red    */
                    default:
                         active_col = IM_COL32(180, 180, 220, 255);
                         break; /* grey   */
                    }
                    /* Dim idle colour for this kind, blend with active */
                    ImU32 base_col = s_sch_show_kinds
                                         ? sch_wire_color_kind(w->net_id)
                                         : IM_COL32(70, 80, 100, 120);
                    float t = fade > 1.0f ? 1.0f : fade;
                    uint8_t r = (uint8_t)(((base_col >> IM_COL32_R_SHIFT) & 0xFF) * (1 - t) + ((active_col >> IM_COL32_R_SHIFT) & 0xFF) * t);
                    uint8_t g = (uint8_t)(((base_col >> IM_COL32_G_SHIFT) & 0xFF) * (1 - t) + ((active_col >> IM_COL32_G_SHIFT) & 0xFF) * t);
                    uint8_t b = (uint8_t)(((base_col >> IM_COL32_B_SHIFT) & 0xFF) * (1 - t) + ((active_col >> IM_COL32_B_SHIFT) & 0xFF) * t);
                    uint8_t a = (uint8_t)(120 + (uint8_t)(fade * 135));
                    col = IM_COL32(r, g, b, a);
               }
               else if (s_sch_show_kinds)
                    col = sch_wire_color_kind(w->net_id);
               else if (s_sch_show_activity)
               {
                    int anim = -1;
                    if (w->net_id >= 0 && w->net_id < HW_NET_COUNT)
                         anim = hw_nets[w->net_id].anim_group;
                    col = sch_wire_color(anim);
               }
               else
                    col = IM_COL32(90, 96, 120, 150);

               /* Net selection: brighten wires of the selected net */
               if (net_selected)
               {
                    col = IM_COL32(255, 230, 80, 255); /* bright yellow highlight */
               }
               else if (s_sch_sel_net >= 0 && w->net_id != s_sch_sel_net)
               {
                    /* dim other nets when one is selected */
                    uint8_t a = (col >> IM_COL32_A_SHIFT) & 0xFF;
                    col = (col & ~(0xFFu << IM_COL32_A_SHIFT)) | ((uint32_t)(a / 4) << IM_COL32_A_SHIFT);
               }

               /* Cross-link flash: pulse orange→yellow on the jumped-to net */
               if (s_sch_cross_net >= 0 && w->net_id == s_sch_cross_net
                   && s_sch_cross_timer > 0.0f)
               {
                    float phase = s_sch_cross_timer * 6.0f; /* ~3 pulses over 2s */
                    float pulse = (sinf(phase * 3.14159f) + 1.0f) * 0.5f;
                    uint8_t r = (uint8_t)(200 + pulse * 55);
                    uint8_t g = (uint8_t)(100 + pulse * 130);
                    col = IM_COL32(r, g, 0, 255);
               }
          }
          /* Power/reset nets get thicker strokes */
          bool is_power_wire = false;
          if (w->net_id >= 0 && w->net_id < HW_NET_COUNT)
          {
               const HwNetSemantic *ws = hw_map_find_net(w->net_id);
               is_power_wire = ws && (ws->kind == HW_SIG_POWER || ws->kind == HW_SIG_RESET);
          }
          float thick = w->is_bus ? bus_px : (is_power_wire ? power_px : wire_px);
          if (net_selected)
               thick = thick * 2.0f > wire_px * 3.0f ? thick : wire_px * 2.5f;
          if (net_hover && s_sch_sel_net < 0)
               thick = thick * 1.6f;
          if (s_sch_show_wires)
          {
               if (net_selected)
                    dl->AddLine(SP(w->nx1, w->ny1), SP(w->nx2, w->ny2),
                                IM_COL32(255, 230, 80, 50), thick * 3.0f); /* halo */
               dl->AddLine(SP(w->nx1, w->ny1), SP(w->nx2, w->ny2), col, thick);
          }

          /* Net 0/1 label at wire midpoint — only when zoomed in and trace active */
          if (s_sch_show_levels && !s_sch_paper_mode && zoom > 900.0f && !w->is_bus && w->net_id >= 0 && w->net_id < HW_NET_COUNT && act->net_fade[w->net_id] > 0.05f)
          {
               int8_t lvl = act->net_level[w->net_id];
               if (lvl >= 0)
               {
                    float mx = SX((w->nx1 + w->nx2) * 0.5f);
                    float my = SY((w->ny1 + w->ny2) * 0.5f);
                    const char *lv_str = lvl ? "1" : "0";
                    ImU32 lv_col = lvl ? IM_COL32(120, 255, 120, 255) /* green=high */
                                       : IM_COL32(255, 100, 80, 255); /* red=low    */
                    float sz = ImGui::GetFontSize() * 1.4f;
                    /* background pill for readability */
                    ImVec2 tp = ImVec2(mx - sz * 0.35f, my - sz * 0.55f);
                    dl->AddRectFilled(ImVec2(tp.x - 2, tp.y - 1),
                                      ImVec2(tp.x + sz * 0.7f + 2, tp.y + sz + 1),
                                      IM_COL32(0, 0, 0, 160), 3.0f);
                    dl->AddText(nullptr, sz, tp, lv_col, lv_str);
               }
          }
     }

     /* ── Unconnected markers (× on wires with net_id == -1) ── */
     if (s_sch_show_wires && zoom > 400.0f)
     {
          ImU32 unc_col = IM_COL32(200, 50, 50, 200);
          float r = wire_px * 2.5f;
          if (r < 3.0f)
               r = 3.0f;
          for (int i = 0; i < HW_WIRE_COUNT; i++)
          {
               const HwWire *w = &hw_wires[i];
               if (w->net_id >= 0)
                    continue;
               if (!hw_sch_line_in_viewport(&cache, w->nx1, w->ny1, w->nx2, w->ny2))
                    continue;
               /* mark both endpoints */
               for (int ep = 0; ep < 2; ep++)
               {
                    float ex = SX(ep == 0 ? w->nx1 : w->nx2);
                    float ey = SY(ep == 0 ? w->ny1 : w->ny2);
                    dl->AddLine(ImVec2(ex - r, ey - r), ImVec2(ex + r, ey + r), unc_col, wire_px);
                    dl->AddLine(ImVec2(ex + r, ey - r), ImVec2(ex - r, ey + r), unc_col, wire_px);
               }
          }
     }

     /* ── Wire hover tooltip ── */
     if (s_sch_hover_wire >= 0 && !s_sch_paper_mode)
     {
          const HwWire *hw = &hw_wires[s_sch_hover_wire];
          ImGui::BeginTooltip();
          if (hw->net_id >= 0 && hw->net_id < HW_NET_COUNT)
          {
               const HwNet *net = &hw_nets[hw->net_id];
               ImGui::Text("Net: %s  (#%d)", net->name, hw->net_id);
               const HwNetSemantic *sem = hw_map_find_net(hw->net_id);
               if (sem)
               {
                    ImGui::TextDisabled("kind: %s  [%s]",
                                        hw_signal_kind_name(sem->kind),
                                        hw_confidence_name(sem->confidence));
                    if (sem->canonical_name && sem->canonical_name[0])
                         ImGui::TextDisabled("canonical: %s", sem->canonical_name);
               }
               if (gb && gb->debug.hw_trace.enabled && act->net_fade[hw->net_id] > 0.01f)
               {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f),
                                       "active  fade=%.2f", act->net_fade[hw->net_id]);
               }
          }
          else
          {
               ImGui::TextDisabled("wire (no net)");
          }
          ImGui::TextDisabled(hw->is_bus ? "bus" : "wire");
          ImGui::EndTooltip();
     }

     /* ── Junctions (colored by net kind) ── */
     float jr = wire_px * 1.8f;
     if (jr < 2.5f)
          jr = 2.5f;
     if (s_sch_show_junctions)
     {
          for (int i = 0; i < HW_JUNCTION_COUNT; i++)
          {
               const HwJunction *j = &hw_junctions[i];
               if (!hw_sch_in_viewport(&cache, j->nx, j->ny))
                    continue;

               ImU32 jcol;
               if (s_sch_paper_mode)
               {
                    jcol = IM_COL32(0, 150, 0, 240);
               }
               else
               {
                    /* Find nearest wire to determine net kind for this junction */
                    int jnet = -1;
                    float best_j2 = 1e-6f; /* within ~1mm normalized */
                    for (int wi = 0; wi < HW_WIRE_COUNT; wi++)
                    {
                         const HwWire *wj = &hw_wires[wi];
                         if (wj->net_id < 0)
                              continue;
                         auto near_end = [&](float ex, float ey)
                         {
                              float dx = ex - j->nx, dy = ey - j->ny;
                              return dx * dx + dy * dy;
                         };
                         float d1 = near_end(wj->nx1, wj->ny1);
                         float d2 = near_end(wj->nx2, wj->ny2);
                         float d = d1 < d2 ? d1 : d2;
                         if (d < best_j2)
                         {
                              best_j2 = d;
                              jnet = wj->net_id;
                         }
                    }
                    if (jnet >= 0)
                    {
                         bool jsel = (s_sch_sel_net == jnet);
                         jcol = jsel ? IM_COL32(255, 230, 80, 255)
                                     : sch_wire_color_kind(jnet);
                         /* bump alpha for visibility */
                         uint8_t a = (jcol >> IM_COL32_A_SHIFT) & 0xFF;
                         if (a < 200)
                              a = 200;
                         jcol = (jcol & ~(0xFFu << IM_COL32_A_SHIFT)) | ((uint32_t)a << IM_COL32_A_SHIFT);
                    }
                    else
                         jcol = IM_COL32(200, 210, 230, 200);
               }
               dl->AddCircleFilled(SP(j->nx, j->ny), jr, jcol);
          }
     }

     /* ── Labels with semantic shapes (KiCad / IEC 60617) ── */
     float lbl_zoom_thresh = s_sch_paper_mode ? 200.0f : 300.0f;
     if (s_sch_show_labels && zoom > lbl_zoom_thresh)
     {
          ImU32 lbl_col = s_sch_paper_mode ? IM_COL32(0, 110, 110, 230) : IM_COL32(180, 210, 160, 210);
          ImU32 lbl_fill = s_sch_paper_mode ? IM_COL32(230, 250, 240, 80) : IM_COL32(30, 50, 40, 60);
          ImU32 pwr_col = s_sch_paper_mode ? IM_COL32(180, 10, 10, 230) : IM_COL32(255, 80, 80, 200);
          float fsz = ImGui::GetFontSize();
          float th = fsz + 4.0f;                     /* label body height */
          float tip = (zoom > 600.0f) ? 8.0f : 5.0f; /* arrowhead length */
          float lw = wire_px > 1.0f ? wire_px : 1.0f;

          /* GND/VCC power symbol helper — called for power-rail labels */
          auto draw_power_sym = [&](float px, float py, const char *name)
          {
               float sc = (zoom / 1000.0f) > 1.0f ? 1.0f : zoom / 1000.0f;
               if (sc < 0.4f)
                    sc = 0.4f;
               bool is_gnd = (strncmp(name, "GND", 3) == 0 || strncmp(name, "VSS", 3) == 0 || strncmp(name, "VEE", 3) == 0);
               if (is_gnd)
               {
                    float w0 = 12.0f * sc, w1 = 8.0f * sc, w2 = 4.0f * sc;
                    float st = 4.0f * sc;
                    dl->AddLine(ImVec2(px - w0, py), ImVec2(px + w0, py), pwr_col, lw * 1.5f);
                    dl->AddLine(ImVec2(px - w1, py + st), ImVec2(px + w1, py + st), pwr_col, lw * 1.2f);
                    dl->AddLine(ImVec2(px - w2, py + st * 2), ImVec2(px + w2, py + st * 2), pwr_col, lw);
               }
               else /* VCC / VDD / VIN */
               {
                    float ht = 10.0f * sc;
                    dl->AddLine(ImVec2(px, py), ImVec2(px, py - ht), pwr_col, lw * 1.5f);
                    ImVec2 tri[3] = {{px, py - ht - 6.0f * sc},
                                     {px - 5.0f * sc, py - ht},
                                     {px + 5.0f * sc, py - ht}};
                    dl->AddTriangleFilled(tri[0], tri[1], tri[2], pwr_col);
                    float nw = ImGui::CalcTextSize(name).x;
                    dl->AddText(ImVec2(px - nw * 0.5f, py - ht - 6.0f * sc - fsz - 2),
                                pwr_col, name);
               }
          };

          for (int i = 0; i < HW_LABEL_COUNT; i++)
          {
               const HwLabel *lbl = &hw_labels[i];
               if (!hw_sch_in_viewport(&cache, lbl->nx, lbl->ny))
                    continue;
               float sx = SX(lbl->nx);
               float sy = SY(lbl->ny);
               float cy = sy; /* vertical center of label */
               float tw = ImGui::CalcTextSize(lbl->text).x;

               /* Classify label type from text content:
                * - starts with '{' and length > 20 chars → global label (hexagon)
                * - GND/VSS/VCC/VDD/VIN → power symbol
                * - everything else       → local label (rect + triangle) */
               const char *t = lbl->text;
               bool is_global = (t[0] == '{' && (int)strlen(t) > 10);
               bool is_power = (strncmp(t, "GND", 3) == 0 || strncmp(t, "VCC", 3) == 0 || strncmp(t, "VDD", 3) == 0 || strncmp(t, "VIN", 3) == 0 || strncmp(t, "VSS", 3) == 0 || strncmp(t, "VEE", 3) == 0);

               /* angle==0: connection point LEFT, text opens rightward
                * angle!=0: connection point RIGHT, text opens leftward  */
               bool left_conn = (lbl->angle == 0.0f);

               if (is_power && zoom > 300.0f)
               {
                    draw_power_sym(sx, sy, t);
                    continue;
               }

               if (is_global)
               {
                    /* Hexagon: ponta esquerda ─ rect ─ ponta direita */
                    float body_w = tw + 8.0f;
                    float hh = th * 0.5f;
                    float lx, rx;
                    if (left_conn)
                    {
                         lx = sx;
                         rx = sx + tip + body_w;
                    }
                    else
                    {
                         lx = sx - tip - body_w;
                         rx = sx;
                    }
                    ImVec2 pts[6] = {
                        ImVec2(lx, cy),
                        ImVec2(lx + tip, cy - hh),
                        ImVec2(rx - tip, cy - hh),
                        ImVec2(rx, cy),
                        ImVec2(rx - tip, cy + hh),
                        ImVec2(lx + tip, cy + hh),
                    };
                    dl->AddConvexPolyFilled(pts, 6, lbl_fill);
                    dl->AddPolyline(pts, 6, lbl_col, ImDrawFlags_Closed, lw);
                    float tx = left_conn ? lx + tip + 4 : lx + tip + 4;
                    dl->AddText(ImVec2(tx, cy - fsz * 0.5f), lbl_col, lbl->text);
               }
               else
               {
                    /* Local label: rectangle body + directional triangle at connection point */
                    float body_w = tw + 8.0f;
                    float hh = th * 0.5f;
                    float lx, rx;
                    if (left_conn)
                    {
                         lx = sx;
                         rx = sx + body_w;
                    }
                    else
                    {
                         lx = sx - body_w;
                         rx = sx;
                    }

                    if (left_conn)
                    {
                         /* connection on left: triangle tip at sx pointing left, rect to the right */
                         ImVec2 pts[5] = {
                             ImVec2(sx + tip, cy - hh),
                             ImVec2(rx, cy - hh),
                             ImVec2(rx, cy + hh),
                             ImVec2(sx + tip, cy + hh),
                             ImVec2(sx, cy),
                         };
                         dl->AddConvexPolyFilled(pts, 5, lbl_fill);
                         dl->AddPolyline(pts, 5, lbl_col, ImDrawFlags_Closed, lw);
                         dl->AddText(ImVec2(sx + tip + 4, cy - fsz * 0.5f), lbl_col, lbl->text);
                    }
                    else
                    {
                         /* connection on right: rect to the left, triangle tip at sx pointing right */
                         ImVec2 pts[5] = {
                             ImVec2(lx, cy - hh),
                             ImVec2(sx - tip, cy - hh),
                             ImVec2(sx, cy),
                             ImVec2(sx - tip, cy + hh),
                             ImVec2(lx, cy + hh),
                         };
                         dl->AddConvexPolyFilled(pts, 5, lbl_fill);
                         dl->AddPolyline(pts, 5, lbl_col, ImDrawFlags_Closed, lw);
                         dl->AddText(ImVec2(lx + 4, cy - fsz * 0.5f), lbl_col, lbl->text);
                    }
               }
          }
     }

     /* ── Component boxes ── */
     if (s_sch_show_components)
     {
          for (int i = 0; i < HW_COMPONENT_COUNT; i++)
          {
               const HwComponent *comp = &hw_components[i];
               float comp_nx, comp_ny, comp_nw, comp_nh;
               sch_component_reference_body(comp, &comp_nx, &comp_ny, &comp_nw, &comp_nh);
               float cx = SX(comp_nx);
               float cy = SY(comp_ny);
               float hw2 = cache.scale_x * comp_nw * 0.5f;
               float hh2 = cache.scale_y * comp_nh * 0.5f;
               if (hw2 < 4.0f)
                    hw2 = 4.0f;
               if (hh2 < 4.0f)
                    hh2 = 4.0f;

               ImVec2 tl = ImVec2(cx - hw2, cy - hh2);
               ImVec2 br = ImVec2(cx + hw2, cy + hh2);

               /* Cull if completely off canvas */
               if (br.x < canvas_pos.x || tl.x > canvas_pos.x + canvas_size.x)
                    continue;
               if (br.y < canvas_pos.y || tl.y > canvas_pos.y + canvas_size.y)
                    continue;

               bool hovered = mouse.x >= tl.x && mouse.x <= br.x &&
                              mouse.y >= tl.y && mouse.y <= br.y;
               bool selected = (s_sch_sel_comp == i);

               if (hovered)
                    hover_comp = i;

               if (s_sch_paper_mode)
               {
                    /* ── Paper mode: IC-style schematic component ── */
                    bool is_large = (comp_nh >= 0.10f); /* U1/U2/U3/P1/J2 */
                    bool is_resistor = (comp->ref[0] == 'R');
                    bool is_capacitor = (comp->ref[0] == 'C');

                    ImU32 outline = selected  ? IM_COL32(200, 40, 40, 255)
                                    : hovered ? IM_COL32(180, 80, 0, 255)
                                              : IM_COL32(180, 20, 20, 220);
                    float bthick = selected ? 2.5f : hovered ? 2.0f
                                                             : 1.5f;

                    if (is_resistor && zoom > 120.0f)
                    {
                         /* Zigzag resistor symbol — horizontal body */
                         float cx2 = (tl.x + br.x) * 0.5f;
                         float cy2 = (tl.y + br.y) * 0.5f;
                         float bw = (br.x - tl.x) * 0.5f + 3.0f; /* half body width */
                         float bh = (br.y - tl.y) * 0.5f;        /* half height = amplitude */
                         /* lead lines */
                         dl->AddLine(ImVec2(cx2 - bw - 4, cy2), ImVec2(cx2 - bw, cy2), outline, bthick);
                         dl->AddLine(ImVec2(cx2 + bw, cy2), ImVec2(cx2 + bw + 4, cy2), outline, bthick);
                         /* zigzag: 6 segments */
                         const int ZN = 6;
                         float zstep = (2.0f * bw) / ZN;
                         for (int z = 0; z < ZN; z++)
                         {
                              float x0 = cx2 - bw + z * zstep;
                              float x1 = x0 + zstep;
                              float y0 = cy2 + ((z % 2 == 0) ? bh : -bh);
                              float y1 = cy2 + ((z % 2 == 0) ? -bh : bh);
                              dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), outline, bthick);
                         }
                         if (zoom > 200.0f)
                              dl->AddText(ImVec2(cx2 - ImGui::CalcTextSize(comp->ref).x * 0.5f, cy2 - bh - 12),
                                          IM_COL32(10, 10, 80, 220), comp->ref);
                    }
                    else if (is_capacitor && zoom > 120.0f)
                    {
                         /* Two parallel bars capacitor symbol — horizontal */
                         float cx2 = (tl.x + br.x) * 0.5f;
                         float cy2 = (tl.y + br.y) * 0.5f;
                         float bh = (br.y - tl.y) * 0.5f + 2.0f; /* half bar height */
                         float gap = 2.5f;
                         /* lead lines */
                         dl->AddLine(ImVec2(tl.x - 4, cy2), ImVec2(cx2 - gap, cy2), outline, bthick);
                         dl->AddLine(ImVec2(cx2 + gap, cy2), ImVec2(br.x + 4, cy2), outline, bthick);
                         /* plates */
                         dl->AddLine(ImVec2(cx2 - gap, cy2 - bh), ImVec2(cx2 - gap, cy2 + bh), outline, bthick + 0.5f);
                         dl->AddLine(ImVec2(cx2 + gap, cy2 - bh), ImVec2(cx2 + gap, cy2 + bh), outline, bthick + 0.5f);
                         if (zoom > 200.0f)
                              dl->AddText(ImVec2(cx2 - ImGui::CalcTextSize(comp->ref).x * 0.5f, cy2 - bh - 12),
                                          IM_COL32(10, 10, 80, 220), comp->ref);
                    }
                    else
                    {
                         /* IC / connector: filled rectangle */
                         ImU32 fill = is_large ? IM_COL32(255, 255, 180, 240)
                                               : IM_COL32(255, 253, 245, 230);
                         dl->AddRectFilled(tl, br, fill);
                         dl->AddRect(tl, br, outline, 0.0f, 0, bthick);

                         /* ── Real pin labels from net map (IC only, zoomed in) ── */
                         if (is_large && zoom > 400.0f)
                         {
                              float pin_len = 7.0f;
                              ImU32 pin_col = IM_COL32(180, 20, 20, 190);
                              float fsz = ImGui::GetFontSize() * 0.65f;
                              /* Walk all nets in hw_net_map; for nets whose wires
                               * connect to this component box, draw the pin stub
                               * and label at the wire endpoint closest to the box. */
                              for (int ni = 0; ni < hw_net_map_count; ni++)
                              {
                                   const HwNetSemantic *ns = &hw_net_map[ni];
                                   if (ns->net_id < 0 || ns->net_id >= HW_NET_COUNT)
                                        continue;
                                   /* Find wire endpoint closest to this IC box edge */
                                   float best_d2 = 1e9f;
                                   float best_ex = 0, best_ey = 0;
                                   bool best_left = false;
                                   for (int wi = 0; wi < HW_WIRE_COUNT; wi++)
                                   {
                                        if (hw_wires[wi].net_id != ns->net_id)
                                             continue;
                                        auto check_ep = [&](float enx, float eny)
                                        {
                                             float esx = SX(enx), esy = SY(eny);
                                             /* must be just outside the box edge */
                                             bool near_left = (esx >= tl.x - pin_len - 2) && (esx <= tl.x + 2) && (esy >= tl.y - 4) && (esy <= br.y + 4);
                                             bool near_right = (esx >= br.x - 2) && (esx <= br.x + pin_len + 2) && (esy >= tl.y - 4) && (esy <= br.y + 4);
                                             if (!near_left && !near_right)
                                                  return;
                                             float dx = esx - (near_left ? tl.x : br.x);
                                             float dy = esy - (tl.y + br.y) * 0.5f;
                                             float d2 = dx * dx + dy * dy;
                                             if (d2 < best_d2)
                                             {
                                                  best_d2 = d2;
                                                  best_ex = esx;
                                                  best_ey = esy;
                                                  best_left = near_left;
                                             }
                                        };
                                        check_ep(hw_wires[wi].nx1, hw_wires[wi].ny1);
                                        check_ep(hw_wires[wi].nx2, hw_wires[wi].ny2);
                                   }
                                   if (best_d2 >= 1e8f)
                                        continue;
                                   /* draw stub */
                                   float edge_x = best_left ? tl.x : br.x;
                                   dl->AddLine(ImVec2(edge_x, best_ey),
                                               ImVec2(best_left ? edge_x - pin_len : edge_x + pin_len, best_ey),
                                               pin_col, 1.0f);
                                   /* label: inside the box near pin */
                                   const char *pname = ns->canonical_name && ns->canonical_name[0]
                                                           ? ns->canonical_name
                                                           : ns->schematic_name;
                                   float tw = ImGui::CalcTextSize(pname).x * (fsz / ImGui::GetFontSize());
                                   float tx = best_left ? edge_x + 2 : edge_x - tw - 2;
                                   dl->AddText(nullptr, fsz, ImVec2(tx, best_ey - fsz * 0.5f),
                                               IM_COL32(20, 20, 100, 210), pname);
                              }
                         }
                         else if (is_large && zoom > 150.0f)
                         {
                              /* Fallback: generic evenly-spaced pin stubs */
                              float pin_len = 4.0f + zoom * 0.003f;
                              if (pin_len > 8.0f)
                                   pin_len = 8.0f;
                              int n_pins = (int)((br.y - tl.y) / 6.0f);
                              if (n_pins > 40)
                                   n_pins = 40;
                              float pin_step = (br.y - tl.y) / (float)(n_pins + 1);
                              for (int p = 1; p <= n_pins; p++)
                              {
                                   float py = tl.y + p * pin_step;
                                   dl->AddLine(ImVec2(tl.x - pin_len, py), ImVec2(tl.x, py),
                                               IM_COL32(180, 20, 20, 190), 1.0f);
                                   dl->AddLine(ImVec2(br.x, py), ImVec2(br.x + pin_len, py),
                                               IM_COL32(180, 20, 20, 190), 1.0f);
                              }
                         }

                         /* Labels: ref above box, value inside */
                         if (zoom > 60.0f)
                         {
                              dl->AddText(ImVec2(tl.x, tl.y - 14),
                                          IM_COL32(10, 10, 80, 230), comp->ref);
                              if (zoom > 120.0f && is_large)
                                   dl->AddText(ImVec2(tl.x + 4, tl.y + 4),
                                               IM_COL32(60, 40, 10, 200), comp->value);
                         }
                    }
               }
               else
               {
                    /* ── Dark mode component ── */
                    static const ImU32 kind_comp_col[] = {
                        IM_COL32(80, 140, 255, 220),  /* CPU         — blue */
                        IM_COL32(220, 200, 60, 220),  /* CART        — gold */
                        IM_COL32(80, 220, 110, 220),  /* WRAM        — green */
                        IM_COL32(60, 210, 180, 220),  /* VRAM        — teal */
                        IM_COL32(60, 210, 180, 220),  /* PPU_LCD     — teal */
                        IM_COL32(255, 160, 60, 220),  /* APU_AUDIO   — amber */
                        IM_COL32(200, 140, 255, 220), /* TIMER_CLOCK — violet */
                        IM_COL32(200, 230, 80, 220),  /* JOYPAD      — lime */
                        IM_COL32(60, 200, 230, 220),  /* SERIAL      — cyan */
                        IM_COL32(255, 80, 80, 180),   /* POWER       — red */
                        IM_COL32(120, 120, 130, 160), /* MISC        — grey */
                        IM_COL32(70, 75, 95, 130),    /* UNKNOWN     — dim */
                    };
                    ImU32 border;
                    bool comp_trace_active = gb && gb->debug.hw_trace.enabled && act->comp_fade[i] > 0.01f;
                    if (comp_trace_active)
                    {
                         float fade = act->comp_fade[i];
                         gb_hw_trace_event_type lt = act->comp_last_type[i];
                         ImU32 active_col;
                         switch (lt)
                         {
                         case GB_HW_EVT_CPU_FETCH:
                              active_col = IM_COL32(255, 220, 60, 255);
                              break;
                         case GB_HW_EVT_CPU_READ:
                              active_col = IM_COL32(60, 200, 255, 255);
                              break;
                         case GB_HW_EVT_CPU_WRITE:
                              active_col = IM_COL32(255, 120, 60, 255);
                              break;
                         case GB_HW_EVT_DMA_READ:
                         case GB_HW_EVT_DMA_WRITE:
                              active_col = IM_COL32(200, 80, 255, 255);
                              break;
                         case GB_HW_EVT_IRQ_REQUEST:
                         case GB_HW_EVT_IRQ_ACK:
                              active_col = IM_COL32(255, 60, 60, 255);
                              break;
                         default:
                              active_col = IM_COL32(180, 180, 220, 255);
                              break;
                         }
                         float t = fade > 1.0f ? 1.0f : fade;
                         uint8_t r = (uint8_t)(80 * (1 - t) + ((active_col >> IM_COL32_R_SHIFT) & 0xFF) * t);
                         uint8_t g = (uint8_t)(90 * (1 - t) + ((active_col >> IM_COL32_G_SHIFT) & 0xFF) * t);
                         uint8_t b = (uint8_t)(120 * (1 - t) + ((active_col >> IM_COL32_B_SHIFT) & 0xFF) * t);
                         uint8_t a = (uint8_t)(160 + (uint8_t)(t * 95));
                         border = IM_COL32(r, g, b, a);
                    }
                    else if (s_sch_show_kinds)
                    {
                         const HwComponentSemantic *csem = hw_map_find_component(i);
                         HwComponentKind ck = csem ? csem->kind : HW_COMP_UNKNOWN;
                         border = kind_comp_col[ck];
                    }
                    else if (!s_sch_show_activity)
                         border = IM_COL32(90, 100, 120, 150);
                    else if (comp->signal_group == 0)
                         border = sch_wire_color(HW_ANIM_ADDR);
                    else if (comp->signal_group == 1)
                         border = sch_wire_color(HW_ANIM_WRAM_DATA);
                    else if (comp->signal_group == 2)
                         border = sch_wire_color(HW_ANIM_WRAM_DATA);
                    else if (comp->signal_group == 3)
                         border = sch_wire_color(HW_ANIM_ADDR);
                    else if (comp->signal_group == 4)
                         border = sch_wire_color(HW_ANIM_CLOCK);
                    else if (comp->signal_group == 5)
                         border = sch_wire_color(HW_ANIM_AUDIO);
                    else
                         border = IM_COL32(80, 90, 100, 160);

                    /* Fill brightens when trace-active */
                    ImU32 fill = selected  ? IM_COL32(40, 55, 80, 230)
                                 : hovered ? IM_COL32(30, 38, 55, 220)
                                 : comp_trace_active
                                     ? IM_COL32(28, 35, 55, 230)
                                     : IM_COL32(20, 24, 34, 200);
                    float bthick = selected ? 2.5f : hovered ? 1.8f
                                                             : 1.2f;

                    dl->AddRectFilled(tl, br, fill, 3.0f);
                    dl->AddRect(tl, br, border, 3.0f, 0, bthick);

                    /* ── Pin grouping stripes for U1/U2/U3 (zoom > 300) ── */
                    if (zoom > 300.0f && comp_nh >= 0.10f)
                    {
                         struct PinGroup
                         {
                              const char *name;
                              int side; /* 0=left 1=right */
                              ImU32 color;
                              float frac0, frac1; /* fraction of box height */
                         };
                         /* Layouts for the three ICs; indexed by ref first char + digit */
                         static const PinGroup u1_groups[] = {
                             {"ADDR", 0, IM_COL32(80, 140, 255, 200), 0.05f, 0.44f},
                             {"DATA", 0, IM_COL32(80, 220, 110, 200), 0.46f, 0.70f},
                             {"PWR", 0, IM_COL32(255, 80, 80, 180), 0.72f, 0.95f},
                             {"CTRL", 1, IM_COL32(255, 130, 50, 200), 0.05f, 0.38f},
                             {"LCD", 1, IM_COL32(60, 210, 180, 200), 0.40f, 0.65f},
                             {"APU", 1, IM_COL32(255, 160, 60, 200), 0.67f, 0.82f},
                             {"CLK", 1, IM_COL32(200, 140, 255, 200), 0.84f, 0.95f},
                         };
                         static const PinGroup u2_groups[] = {
                             {"DATA", 0, IM_COL32(80, 220, 110, 200), 0.05f, 0.50f},
                             {"ADDR", 0, IM_COL32(80, 140, 255, 200), 0.52f, 0.95f},
                             {"DATA", 1, IM_COL32(80, 220, 110, 200), 0.05f, 0.50f},
                             {"ADDR", 1, IM_COL32(80, 140, 255, 200), 0.52f, 0.95f},
                         };
                         /* U3 same layout as U2 */
                         const PinGroup *groups = nullptr;
                         int group_count = 0;
                         if (strcmp(comp->ref, "U1") == 0)
                         {
                              groups = u1_groups;
                              group_count = 7;
                         }
                         else if (strcmp(comp->ref, "U2") == 0 || strcmp(comp->ref, "U3") == 0)
                         {
                              groups = u2_groups;
                              group_count = 4;
                         }

                         if (groups)
                         {
                              float stripe_w = (zoom > 600.0f) ? 7.0f : 4.0f;
                              float box_h = br.y - tl.y;
                              for (int g = 0; g < group_count; g++)
                              {
                                   const PinGroup *pg = &groups[g];
                                   float gx0 = pg->side == 0 ? tl.x : br.x - stripe_w;
                                   float gx1 = pg->side == 0 ? tl.x + stripe_w : br.x;
                                   float gy0 = tl.y + box_h * pg->frac0;
                                   float gy1 = tl.y + box_h * pg->frac1;
                                   ImU32 sc = pg->color & IM_COL32(255, 255, 255, 80);
                                   dl->AddRectFilled(ImVec2(gx0, gy0), ImVec2(gx1, gy1), sc);
                                   dl->AddRect(ImVec2(gx0, gy0), ImVec2(gx1, gy1),
                                               pg->color, 0.0f, 0, 0.5f);
                                   if (zoom > 500.0f)
                                   {
                                        float tx = pg->side == 0 ? gx1 + 2 : gx0 - ImGui::CalcTextSize(pg->name).x - 2;
                                        float ty = (gy0 + gy1) * 0.5f - ImGui::GetFontSize() * 0.5f;
                                        dl->AddText(ImVec2(tx, ty), pg->color, pg->name);
                                   }
                              }
                         }
                    }

                    if (zoom > 80.0f)
                    {
                         dl->AddText(ImVec2(tl.x + 4, tl.y + 3), IM_COL32(230, 235, 255, 240), comp->ref);
                         if (zoom > 200.0f)
                              dl->AddText(ImVec2(tl.x + 4, tl.y + 16), IM_COL32(160, 175, 190, 190), comp->value);
                    }
               }

               if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    s_sch_sel_comp = selected ? -1 : i;
          }
     }

     /* ── Paper title block (bottom-right of schematic sheet) ── */
     if (s_sch_paper_mode)
     {
          ImVec2 br_paper = SP(1.0f, 1.0f);
          if (br_paper.x > canvas_pos.x && br_paper.y > canvas_pos.y)
          {
               float tb_w = 184.0f, tb_h = 38.0f;
               ImVec2 tb_tl = ImVec2(br_paper.x - tb_w, br_paper.y - tb_h);
               ImVec2 tb_br = ImVec2(br_paper.x - 2, br_paper.y - 2);
               dl->AddRectFilled(tb_tl, tb_br, IM_COL32(245, 240, 220, 240));
               dl->AddRect(tb_tl, tb_br, IM_COL32(180, 20, 20, 180), 0.0f, 0, 1.0f);
               dl->AddText(ImVec2(tb_tl.x + 4, tb_tl.y + 4),
                           IM_COL32(20, 20, 80, 230), "Original Game Boy (DMG)");
               dl->AddText(ImVec2(tb_tl.x + 4, tb_tl.y + 20),
                           IM_COL32(80, 40, 10, 200), "DMG-CPU-06  CC-BY 4.0, Gekkio");
          }
     }

     /* ── Minimap (bottom-left corner of canvas, dark mode only) ── */
     if (!s_sch_paper_mode)
     {
          float mm_w = 90.0f, mm_h = mm_w * HW_SCHEMATIC_ASPECT / canvas_size.x * canvas_size.y;
          /* clamp height to roughly A4 aspect */
          mm_h = mm_w * (210.0f / 297.0f);
          float mm_x = canvas_pos.x + 6.0f;
          float mm_y = canvas_pos.y + canvas_size.y - mm_h - 6.0f;

          ImVec2 mm_tl = ImVec2(mm_x, mm_y);
          ImVec2 mm_br = ImVec2(mm_x + mm_w, mm_y + mm_h);
          dl->AddRectFilled(mm_tl, mm_br, IM_COL32(10, 12, 18, 210));
          dl->AddRect(mm_tl, mm_br, IM_COL32(60, 65, 85, 180), 0.0f, 0, 1.0f);

          /* Viewport rectangle in minimap space */
          float vx0 = mm_x + cache.vp_nx0 * mm_w;
          float vy0 = mm_y + cache.vp_ny0 * mm_h;
          float vx1 = mm_x + cache.vp_nx1 * mm_w;
          float vy1 = mm_y + cache.vp_ny1 * mm_h;
          if (vx0 < mm_x)
               vx0 = mm_x;
          if (vy0 < mm_y)
               vy0 = mm_y;
          if (vx1 > mm_br.x)
               vx1 = mm_br.x;
          if (vy1 > mm_br.y)
               vy1 = mm_br.y;
          dl->AddRectFilled(ImVec2(vx0, vy0), ImVec2(vx1, vy1), IM_COL32(80, 120, 200, 50));
          dl->AddRect(ImVec2(vx0, vy0), ImVec2(vx1, vy1), IM_COL32(120, 170, 255, 200), 0.0f, 0, 1.0f);

          /* Dot for selected net centroid */
          if (s_sch_sel_net >= 0)
          {
               float cnx = 0.0f, cny = 0.0f;
               int cn = 0;
               for (int i = 0; i < HW_WIRE_COUNT; i++)
               {
                    if (hw_wires[i].net_id != s_sch_sel_net)
                         continue;
                    cnx += (hw_wires[i].nx1 + hw_wires[i].nx2) * 0.5f;
                    cny += (hw_wires[i].ny1 + hw_wires[i].ny2) * 0.5f;
                    cn++;
               }
               if (cn > 0)
               {
                    cnx /= cn;
                    cny /= cn;
                    dl->AddCircleFilled(ImVec2(mm_x + cnx * mm_w, mm_y + cny * mm_h),
                                        3.0f, IM_COL32(255, 230, 80, 240));
               }
          }
     }

     dl->PopClipRect();

     /* ── Live HW activity status strip (top-left corner of canvas) ── */
     if (gb && !s_sch_paper_mode)
     {
          struct
          {
               const char *label;
               float fade; /* 0..1 activity level */
               ImU32 color;
          } hw_status[] = {
              {"CPU", sv ? (sv->fade_cpu_rom > sv->fade_cpu_wram ? sv->fade_cpu_rom : sv->fade_cpu_wram) : 0.0f,
               IM_COL32(80, 160, 255, 255)},
              {"PPU", sv ? (sv->fade_cpu_vram > sv->fade_ppu_vram ? sv->fade_cpu_vram : sv->fade_ppu_vram) : 0.0f,
               IM_COL32(60, 210, 180, 255)},
              {"DMA", sv ? sv->fade_dma_oam : 0.0f,
               IM_COL32(200, 80, 255, 255)},
              {"APU", sv ? sv->fade_apu : 0.0f,
               IM_COL32(255, 160, 60, 255)},
              {"IRQ", sv ? sv->fade_irq_cpu : 0.0f,
               IM_COL32(255, 60, 60, 255)},
          };
          const int hw_status_n = 5;

          float pill_x = canvas_pos.x + 8.0f;
          float pill_y = canvas_pos.y + 6.0f;
          float pill_h = 16.0f;
          float pill_pad = 5.0f;
          float text_scale = 1.0f;
          (void)text_scale;

          for (int si = 0; si < hw_status_n; si++)
          {
               float fw = ImGui::CalcTextSize(hw_status[si].label).x + pill_pad * 2;
               float alpha_t = hw_status[si].fade > 1.0f ? 1.0f : hw_status[si].fade;

               /* Background pill: dark when idle, colored when active */
               ImU32 bg = IM_COL32(
                   (uint8_t)(20 + alpha_t * (((hw_status[si].color >> IM_COL32_R_SHIFT) & 0xFF) - 20)),
                   (uint8_t)(22 + alpha_t * (((hw_status[si].color >> IM_COL32_G_SHIFT) & 0xFF) - 22)),
                   (uint8_t)(30 + alpha_t * (((hw_status[si].color >> IM_COL32_B_SHIFT) & 0xFF) - 30)),
                   (uint8_t)(160 + alpha_t * 80));
               ImU32 txt_col = IM_COL32(
                   (uint8_t)(100 + alpha_t * (((hw_status[si].color >> IM_COL32_R_SHIFT) & 0xFF) - 100)),
                   (uint8_t)(100 + alpha_t * (((hw_status[si].color >> IM_COL32_G_SHIFT) & 0xFF) - 100)),
                   (uint8_t)(100 + alpha_t * (((hw_status[si].color >> IM_COL32_B_SHIFT) & 0xFF) - 100)),
                   220);

               dl->AddRectFilled(ImVec2(pill_x, pill_y),
                                 ImVec2(pill_x + fw, pill_y + pill_h),
                                 bg, 4.0f);
               if (alpha_t > 0.05f)
                    dl->AddRect(ImVec2(pill_x, pill_y),
                                ImVec2(pill_x + fw, pill_y + pill_h),
                                hw_status[si].color & IM_COL32(255, 255, 255, (uint8_t)(alpha_t * 180)),
                                4.0f, 0, 1.0f);
               dl->AddText(ImVec2(pill_x + pill_pad, pill_y + 2), txt_col, hw_status[si].label);
               pill_x += fw + 4.0f;
          }

          /* Trace indicator */
          if (gb->debug.hw_trace.enabled)
          {
               float fw = ImGui::CalcTextSize("TRACE").x + pill_pad * 2;
               ImU32 bg = IM_COL32(20, 40, 20, 180);
               ImU32 tc = IM_COL32(80, 220, 80, 230);
               dl->AddRectFilled(ImVec2(pill_x, pill_y), ImVec2(pill_x + fw, pill_y + pill_h), bg, 4.0f);
               dl->AddRect(ImVec2(pill_x, pill_y), ImVec2(pill_x + fw, pill_y + pill_h), tc, 4.0f, 0, 1.0f);
               dl->AddText(ImVec2(pill_x + pill_pad, pill_y + 2), tc, "TRACE");
          }
     }

     /* ── Hover tooltip ── */
     if (s_sch_show_components && hover_comp >= 0)
     {
          const HwComponent *comp = &hw_components[hover_comp];
          ImGui::BeginTooltip();
          ImGui::Text("%s  —  %s", comp->ref, comp->value);
          ImGui::TextDisabled("pos: (%.3f, %.3f) normalized", comp->nx, comp->ny);
          const HwComponentSemantic *csem = hw_map_find_component(hover_comp);
          if (csem)
          {
               ImGui::TextDisabled("kind: %s  [%s]", hw_component_kind_name(csem->kind),
                                   hw_confidence_name(csem->confidence));
               if (csem->emulator_owner && csem->emulator_owner[0])
                    ImGui::TextDisabled("owner: %s", csem->emulator_owner);
          }
          ImGui::EndTooltip();
     }

     /* ── Legend + info bar ── */
     ImGui::Separator();
     if (s_sch_sel_net >= 0 && s_sch_sel_net < HW_NET_COUNT)
     {
          const HwNet *net = &hw_nets[s_sch_sel_net];
          const HwNetSemantic *sem = hw_map_find_net(s_sch_sel_net);
          if (sem)
               ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                                  "Net: %s  (%s)  [%s]  F=zoom",
                                  net->name, hw_signal_kind_name(sem->kind),
                                  hw_confidence_name(sem->confidence));
          else
               ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                                  "Net: %s  #%d  F=zoom", net->name, s_sch_sel_net);
     }
     else if (s_sch_sel_comp >= 0 && s_sch_sel_comp < HW_COMPONENT_COUNT)
     {
          const HwComponent *c = &hw_components[s_sch_sel_comp];
          ImGui::Text("Selecionado: %s  (%s)", c->ref, c->value);
     }
     else
     {
          ImGui::TextDisabled("Scroll=zoom  Arraste=pan  Clique wire/comp=selecionar  |  DMG-CPU-06  (CC-BY 4.0, Gekkio)");
     }
     ImGui::SameLine();
     ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20);
     /* Mini legend */
     if (s_sch_show_kinds)
     {
          ImGui::TextColored(ImVec4(0.31f, 0.55f, 1.0f, 1.0f), "Addr");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.31f, 0.86f, 0.43f, 1.0f), "Data");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "RD");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(1.0f, 0.51f, 0.20f, 1.0f), "WR");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.86f, 0.78f, 0.24f, 1.0f), "CS");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.78f, 0.55f, 1.0f, 1.0f), "CLK");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.24f, 0.82f, 0.71f, 1.0f), "LCD");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(1.0f, 0.63f, 0.24f, 1.0f), "Audio");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.24f, 0.78f, 0.90f, 1.0f), "Serial");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.78f, 0.90f, 0.31f, 1.0f), "Joy");
     }
     else if (s_sch_show_activity)
     {
          ImGui::TextColored(ImVec4(0.3f, 0.6f, 1.0f, 1.0f), "Addr");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.4f, 0.7f, 0.5f, 1.0f), "WRAM");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Audio");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.6f, 1.0f), "LCD");
          ImGui::SameLine();
          ImGui::TextColored(ImVec4(0.8f, 0.4f, 1.0f, 1.0f), "IRQ");
     }
     else
     {
          ImGui::TextDisabled("Activity hidden");
     }

     ImGui::EndChild(); /* ##sch_canvas_child */

     /* ── Net timeline: last N events for the selected net ── */
     if (gb && s_sch_sel_net >= 0 && s_sch_sel_net < HW_NET_COUNT)
     {
          const HwNet *sel_net = &hw_nets[s_sch_sel_net];
          struct gb_hw_trace *tl_tr = &gb->debug.hw_trace;

          /* Collect up to 64 events that touched this net, newest first */
          static const int TL_CAP = 64;
          struct TlEntry
          {
               const gb_hw_trace_event *ev;
               bool touched_net;
          };
          TlEntry tl_buf[TL_CAP];
          int tl_n = 0;

          /* Walk ring buffer newest→oldest */
          uint32_t cap = GB_HW_TRACE_CAP;
          uint32_t cnt = tl_tr->count < cap ? tl_tr->count : cap;
          for (uint32_t k = 0; k < cnt && tl_n < TL_CAP; k++)
          {
               uint32_t idx = (tl_tr->head + cap - 1 - k) % cap;
               const gb_hw_trace_event *ev = &tl_tr->events[idx];
               /* Include event if it references this net via address bits */
               bool relevant = false;
               switch (ev->type)
               {
               case GB_HW_EVT_CPU_READ:
               case GB_HW_EVT_CPU_WRITE:
               case GB_HW_EVT_CPU_FETCH:
               case GB_HW_EVT_DMA_READ:
               case GB_HW_EVT_DMA_WRITE:
                    relevant = true;
                    break;
               case GB_HW_EVT_PPU_MODE:
               case GB_HW_EVT_PPU_VBLANK:
               case GB_HW_EVT_PPU_HBLANK:
               case GB_HW_EVT_TIMER_OVF:
               case GB_HW_EVT_APU_WRITE:
               case GB_HW_EVT_JOYPAD:
               case GB_HW_EVT_SERIAL_START:
               case GB_HW_EVT_SERIAL_DONE:
               case GB_HW_EVT_MBC_SWITCH:
               case GB_HW_EVT_IRQ_REQUEST:
               case GB_HW_EVT_IRQ_ACK:
                    relevant = true;
                    break;
               default:
                    break;
               }
               if (relevant)
                    tl_buf[tl_n++] = {ev, true};
          }

          /* Timeline header */
          ImGui::Separator();
          ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f),
                             "Timeline: net %s  (últimos %d eventos do buffer)",
                             sel_net->name, tl_n);

          /* Horizontal mini-timeline strip */
          float tl_h = 36.0f;
          ImVec2 tl_pos = ImGui::GetCursorScreenPos();
          ImVec2 tl_size = ImVec2(ImGui::GetContentRegionAvail().x, tl_h);
          ImDrawList *tdl = ImGui::GetWindowDrawList();
          tdl->AddRectFilled(tl_pos, ImVec2(tl_pos.x + tl_size.x, tl_pos.y + tl_size.y),
                             IM_COL32(14, 16, 22, 220));

          static int s_tl_sel = -1; /* selected event index in tl_buf */

          if (tl_n > 0)
          {
               /* Find timestamp range */
               int32_t ts_min = tl_buf[tl_n - 1].ev->timestamp;
               int32_t ts_max = tl_buf[0].ev->timestamp;
               int32_t ts_span = ts_max - ts_min;
               if (ts_span <= 0)
                    ts_span = 1;

               float bar_w = tl_size.x / (float)tl_n; /* equal-width bars when timestamps close */
               bool use_time_x = ts_span > tl_n * 4;  /* spread by time when events are sparse */

               for (int ei = 0; ei < tl_n; ei++)
               {
                    const gb_hw_trace_event *ev = tl_buf[ei].ev;
                    float t = use_time_x
                                  ? (float)(ev->timestamp - ts_min) / (float)ts_span
                                  : (float)(tl_n - 1 - ei) / (float)(tl_n > 1 ? tl_n - 1 : 1);
                    float bx = tl_pos.x + t * (tl_size.x - bar_w);
                    float by = tl_pos.y + 2.0f;

                    /* Colour by event type */
                    ImU32 bcol;
                    switch (ev->type)
                    {
                    case GB_HW_EVT_CPU_FETCH:
                         bcol = IM_COL32(255, 220, 60, 220);
                         break;
                    case GB_HW_EVT_CPU_READ:
                         bcol = IM_COL32(60, 200, 255, 220);
                         break;
                    case GB_HW_EVT_CPU_WRITE:
                         bcol = IM_COL32(255, 120, 60, 220);
                         break;
                    case GB_HW_EVT_DMA_READ:
                    case GB_HW_EVT_DMA_WRITE:
                         bcol = IM_COL32(200, 80, 255, 220);
                         break;
                    case GB_HW_EVT_IRQ_REQUEST:
                    case GB_HW_EVT_IRQ_ACK:
                         bcol = IM_COL32(255, 60, 60, 220);
                         break;
                    case GB_HW_EVT_PPU_MODE:
                    case GB_HW_EVT_PPU_VBLANK:
                    case GB_HW_EVT_PPU_HBLANK:
                         bcol = IM_COL32(60, 210, 180, 220);
                         break;
                    case GB_HW_EVT_APU_WRITE:
                         bcol = IM_COL32(255, 160, 60, 220);
                         break;
                    case GB_HW_EVT_TIMER_OVF:
                         bcol = IM_COL32(200, 140, 255, 220);
                         break;
                    case GB_HW_EVT_SERIAL_START:
                    case GB_HW_EVT_SERIAL_DONE:
                         bcol = IM_COL32(60, 200, 230, 220);
                         break;
                    case GB_HW_EVT_MBC_SWITCH:
                         bcol = IM_COL32(220, 200, 60, 220);
                         break;
                    case GB_HW_EVT_JOYPAD:
                         bcol = IM_COL32(200, 230, 80, 220);
                         break;
                    default:
                         bcol = IM_COL32(120, 120, 130, 180);
                         break;
                    }

                    bool sel = (s_tl_sel == ei);
                    float bar_h = tl_h - 4.0f;
                    ImVec2 b0 = ImVec2(bx, by);
                    ImVec2 b1 = ImVec2(bx + (bar_w > 2.0f ? bar_w - 1.0f : 1.5f), by + bar_h);
                    tdl->AddRectFilled(b0, b1, bcol, 1.5f);
                    if (sel)
                         tdl->AddRect(b0, b1, IM_COL32(255, 255, 255, 200), 1.5f, 0, 1.5f);

                    /* Hit-test for click */
                    ImVec2 mpos = ImGui::GetIO().MousePos;
                    if (mpos.x >= b0.x && mpos.x <= b1.x &&
                        mpos.y >= b0.y && mpos.y <= b1.y)
                    {
                         if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                              s_tl_sel = sel ? -1 : ei;
                         /* Tooltip */
                         ImGui::BeginTooltip();
                         static const char *evt_names[] = {
                             "NONE", "FETCH", "READ", "WRITE", "IRQ_REQ", "IRQ_ACK",
                             "ALU", "WRITEBACK", "DMA_RD", "DMA_WR", "PPU_MODE",
                             "APU_SMP", "VBLANK", "HBLANK", "OAM_DMA", "TIMER_OVF",
                             "APU_WR", "JOYPAD", "SER_START", "SER_DONE", "MBC_SW"};
                         int etype = (int)ev->type;
                         const char *ename = (etype >= 0 && etype < 21) ? evt_names[etype] : "?";
                         ImGui::Text("%s  ts=%d  seq=%llu",
                                     ename, ev->timestamp, (unsigned long long)ev->seq);
                         if (ev->addr)
                              ImGui::TextDisabled("addr=0x%04X  data=0x%02X", ev->addr, ev->data);
                         if (ev->pc)
                              ImGui::TextDisabled("PC=0x%04X  op=0x%02X", ev->pc, ev->opcode);
                         ImGui::EndTooltip();
                    }
               }
          }
          else
          {
               tdl->AddText(ImVec2(tl_pos.x + 8, tl_pos.y + 10),
                            IM_COL32(100, 105, 120, 200),
                            "Nenhum evento no buffer — habilite o hw trace");
          }

          ImGui::Dummy(tl_size); /* advance cursor past the manual-drawn strip */

          /* Inspector for selected event */
          if (s_tl_sel >= 0 && s_tl_sel < tl_n)
          {
               const gb_hw_trace_event *ev = tl_buf[s_tl_sel].ev;
               ImGui::Indent(8.0f);
               ImGui::TextColored(ImVec4(0.9f, 0.9f, 1.0f, 1.0f),
                                  "seq=%llu  ts=%d  PC=0x%04X  op=0x%02X",
                                  (unsigned long long)ev->seq,
                                  ev->timestamp, ev->pc, ev->opcode);
               ImGui::TextDisabled("addr=0x%04X  data=0x%02X  extra=0x%02X  write=%d",
                                   ev->addr, ev->data, ev->extra, (int)ev->write);
               ImGui::Unindent(8.0f);
          }
          ImGui::Separator();
     }

     /* ── HW Trace + Audit panel (scrollable, fills remaining space) ── */
     if (!gb)
          return;

     ImGui::BeginChild("##sch_trace_child", ImVec2(0, 0), ImGuiChildFlags_None,
                       ImGuiWindowFlags_None);

     struct gb_hw_trace *tr = &gb->debug.hw_trace;

     /* ── Metadata per event type (must match gb_hw_trace_event_type order) ── */
     struct EvtMeta
     {
          const char *name;
          ImU32 color;
          uint8_t subsystem;
     };
     /* subsystem bits: 0=CPU 1=PPU 2=DMA 3=Timer 4=APU 5=Input */
     static const EvtMeta s_evt_meta[] = {
         {"NONE", IM_COL32(100, 100, 100, 200), 0x3F},      /* GB_HW_EVT_NONE       */
         {"FETCH", IM_COL32(120, 180, 255, 220), 1 << 0},   /* GB_HW_EVT_CPU_FETCH  */
         {"READ", IM_COL32(80, 220, 110, 220), 1 << 0},     /* GB_HW_EVT_CPU_READ   */
         {"WRITE", IM_COL32(255, 130, 60, 220), 1 << 0},    /* GB_HW_EVT_CPU_WRITE  */
         {"IRQ_REQ", IM_COL32(220, 100, 255, 220), 1 << 0}, /* GB_HW_EVT_IRQ_REQUEST*/
         {"IRQ_ACK", IM_COL32(255, 80, 80, 220), 1 << 0},   /* GB_HW_EVT_IRQ_ACK    */
         {"DMA_RD", IM_COL32(160, 200, 255, 200), 1 << 2},  /* GB_HW_EVT_DMA_READ   */
         {"DMA_WR", IM_COL32(255, 200, 100, 200), 1 << 2},  /* GB_HW_EVT_DMA_WRITE  */
         {"PPU_MODE", IM_COL32(60, 210, 180, 200), 1 << 1}, /* GB_HW_EVT_PPU_MODE   */
         {"APU_SMP", IM_COL32(255, 160, 60, 200), 1 << 4},  /* GB_HW_EVT_APU_SAMPLE */
         {"VBLANK", IM_COL32(50, 255, 180, 230), 1 << 1},   /* GB_HW_EVT_PPU_VBLANK */
         {"HBLANK", IM_COL32(30, 190, 130, 200), 1 << 1},   /* GB_HW_EVT_PPU_HBLANK */
         {"OAM_DMA", IM_COL32(200, 230, 255, 210), 1 << 2}, /* GB_HW_EVT_OAM_DMA    */
         {"TMR_OVF", IM_COL32(255, 220, 50, 230), 1 << 3},  /* GB_HW_EVT_TIMER_OVF  */
         {"APU_WR", IM_COL32(255, 140, 0, 210), 1 << 4},    /* GB_HW_EVT_APU_WRITE  */
         {"JOYPAD", IM_COL32(80, 255, 100, 220), 1 << 5},   /* GB_HW_EVT_JOYPAD     */
     };
     static const int s_evt_meta_count = (int)(sizeof(s_evt_meta) / sizeof(s_evt_meta[0]));

     /* ── Controls row ── */
     ImGui::Separator();
     bool trace_on = tr->enabled;
     if (ImGui::Checkbox("HW Trace", &trace_on))
          tr->enabled = trace_on;
     ImGui::SameLine();
     uint32_t n_total = tr->count < GB_HW_TRACE_CAP ? tr->count : (uint32_t)GB_HW_TRACE_CAP;
     ImGui::TextDisabled("%u events", n_total);
     ImGui::SameLine();
     if (ImGui::SmallButton("Clear"))
     {
          tr->head = 0;
          tr->count = 0;
     }

     /* ── Subsystem filter toggles ── */
     static uint8_t s_filter_mask = 0x3F; /* all on */
     static bool s_filter_open = false;
     ImGui::SameLine();
     if (ImGui::SmallButton(s_filter_open ? "Filter [-]" : "Filter [+]"))
          s_filter_open = !s_filter_open;

     if (s_filter_open)
     {
          ImGui::Indent(8.0f);
          struct
          {
               const char *label;
               uint8_t bit;
               ImU32 col;
          } subsys[] = {
              {"CPU", 1 << 0, IM_COL32(120, 180, 255, 255)},
              {"PPU", 1 << 1, IM_COL32(50, 230, 180, 255)},
              {"DMA", 1 << 2, IM_COL32(200, 220, 255, 255)},
              {"Timer", 1 << 3, IM_COL32(255, 220, 50, 255)},
              {"APU", 1 << 4, IM_COL32(255, 150, 40, 255)},
              {"Input", 1 << 5, IM_COL32(80, 255, 100, 255)},
          };
          for (int si = 0; si < 6; si++)
          {
               bool on = (s_filter_mask & subsys[si].bit) != 0;
               ImGui::PushStyleColor(ImGuiCol_CheckMark, subsys[si].col);
               if (ImGui::Checkbox(subsys[si].label, &on))
               {
                    if (on)
                         s_filter_mask |= subsys[si].bit;
                    else
                         s_filter_mask &= ~subsys[si].bit;
               }
               ImGui::PopStyleColor();
               if (si < 5)
                    ImGui::SameLine();
          }
          ImGui::Unindent(8.0f);
     }

     if (n_total == 0)
     {
          ImGui::TextDisabled("(no events — enable HW Trace above)");
          ImGui::EndChild();
          return;
     }

     /* ── Selected event index (ring-buffer position from newest=0) ── */
     static int s_selected_ev = -1;
     /* Reset selection if it goes out of range after Clear */
     if (s_selected_ev >= (int)n_total)
          s_selected_ev = -1;

     /* ── Event list (scrollable, newest first) ── */
     float list_h = s_selected_ev >= 0 ? 140.0f : 180.0f;
     ImGui::BeginChild("##hw_trace_list", ImVec2(0, list_h), true,
                       ImGuiWindowFlags_HorizontalScrollbar);
     ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));

     for (uint32_t i = 0; i < n_total; i++)
     {
          uint32_t idx = (tr->head + GB_HW_TRACE_CAP - 1 - i) & (GB_HW_TRACE_CAP - 1);
          const gb_hw_trace_event *ev = &tr->events[idx];
          int t = (int)ev->type;
          if (t < 0 || t >= s_evt_meta_count)
               t = 0;

          /* Apply subsystem filter */
          if (!(s_evt_meta[t].subsystem & s_filter_mask))
               continue;

          ImVec4 cv = ImGui::ColorConvertU32ToFloat4(s_evt_meta[t].color);
          bool selected = (s_selected_ev == (int)i);

          /* Selectable row */
          ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(cv.x * 0.35f, cv.y * 0.35f, cv.z * 0.35f, 0.6f));
          ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(cv.x * 0.5f, cv.y * 0.5f, cv.z * 0.5f, 0.7f));
          ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(cv.x * 0.7f, cv.y * 0.7f, cv.z * 0.7f, 0.8f));

          char row_id[32];
          snprintf(row_id, sizeof(row_id), "##ev%u", i);

          /* Type badge */
          ImGui::TextColored(cv, "%-8s", s_evt_meta[t].name);
          ImGui::SameLine();

          /* Per-type compact summary */
          switch ((gb_hw_trace_event_type)t)
          {
          case GB_HW_EVT_CPU_FETCH:
               ImGui::TextDisabled("PC:%04X  op:%02X", ev->pc, ev->opcode);
               break;
          case GB_HW_EVT_CPU_READ:
               ImGui::TextDisabled("PC:%04X  [%04X]→%02X", ev->pc, ev->addr, ev->data);
               break;
          case GB_HW_EVT_CPU_WRITE:
               ImGui::TextDisabled("PC:%04X  [%04X]←%02X", ev->pc, ev->addr, ev->data);
               break;
          case GB_HW_EVT_IRQ_REQUEST:
               ImGui::TextDisabled("IF:%02X  IE:%02X", ev->data, ev->extra);
               break;
          case GB_HW_EVT_IRQ_ACK:
               ImGui::TextDisabled("vec:%04X  IF:%02X", ev->addr, ev->data);
               break;
          case GB_HW_EVT_DMA_READ:
               ImGui::TextDisabled("[%04X]→%02X", ev->addr, ev->data);
               break;
          case GB_HW_EVT_DMA_WRITE:
               ImGui::TextDisabled("OAM[%02X]←%02X", ev->extra, ev->data);
               break;
          case GB_HW_EVT_PPU_MODE:
               ImGui::TextDisabled("LY:%d  mode:%d", ev->addr & 0xFF, ev->data & 3);
               break;
          case GB_HW_EVT_APU_SAMPLE:
               ImGui::TextDisabled("L:%d R:%d", (int8_t)ev->data, (int8_t)ev->extra);
               break;
          case GB_HW_EVT_PPU_VBLANK:
               ImGui::TextDisabled("LY:%d", ev->data);
               break;
          case GB_HW_EVT_PPU_HBLANK:
               ImGui::TextDisabled("LY:%d", ev->data);
               break;
          case GB_HW_EVT_OAM_DMA:
               ImGui::TextDisabled("[FE%02X]←%02X  pos:%d", ev->extra, ev->data, ev->extra);
               break;
          case GB_HW_EVT_TIMER_OVF:
               ImGui::TextDisabled("TMA:%02X", ev->data);
               break;
          case GB_HW_EVT_APU_WRITE:
               ImGui::TextDisabled("[%04X]←%02X", ev->addr, ev->data);
               break;
          case GB_HW_EVT_JOYPAD:
               ImGui::TextDisabled("state:%02X  %s", ev->data, ev->write ? "pressed" : "released");
               break;
          default:
               ImGui::TextDisabled("T:%d", ev->timestamp);
               break;
          }

          /* Invisible selectable covering the whole row */
          ImGui::SameLine(0, 0);
          float row_w = ImGui::GetContentRegionAvail().x;
          if (ImGui::Selectable(row_id, selected,
                                ImGuiSelectableFlags_SpanAllColumns |
                                    ImGuiSelectableFlags_AllowOverlap,
                                ImVec2(row_w, 0)))
               s_selected_ev = selected ? -1 : (int)i;

          ImGui::PopStyleColor(3);
     }

     ImGui::PopStyleVar();
     ImGui::EndChild();

     /* ── Detail panel for selected event ── */
     if (s_selected_ev >= 0 && s_selected_ev < (int)n_total)
     {
          uint32_t idx = (tr->head + GB_HW_TRACE_CAP - 1 - (uint32_t)s_selected_ev) & (GB_HW_TRACE_CAP - 1);
          const gb_hw_trace_event *ev = &tr->events[idx];
          int t = (int)ev->type;
          if (t < 0 || t >= s_evt_meta_count)
               t = 0;
          ImVec4 cv = ImGui::ColorConvertU32ToFloat4(s_evt_meta[t].color);

          ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(cv.x * 0.12f, cv.y * 0.12f, cv.z * 0.12f, 0.5f));
          ImGui::BeginChild("##hw_trace_detail", ImVec2(0, 100), true);

          ImGui::TextColored(cv, "%s", s_evt_meta[t].name);
          ImGui::SameLine();
          ImGui::TextDisabled("seq:#%llu  T:%d", (unsigned long long)ev->seq, ev->timestamp);

          ImGui::Separator();

          switch ((gb_hw_trace_event_type)t)
          {
          case GB_HW_EVT_CPU_FETCH:
               ImGui::Text("PC   = %04X", ev->pc);
               ImGui::SameLine(120);
               ImGui::Text("opcode = %02X", ev->opcode);
               break;
          case GB_HW_EVT_CPU_READ:
               ImGui::Text("PC   = %04X", ev->pc);
               ImGui::SameLine(120);
               ImGui::Text("addr = %04X", ev->addr);
               ImGui::Text("data = %02X", ev->data);
               break;
          case GB_HW_EVT_CPU_WRITE:
               ImGui::Text("PC   = %04X", ev->pc);
               ImGui::SameLine(120);
               ImGui::Text("addr = %04X", ev->addr);
               ImGui::Text("data = %02X", ev->data);
               break;
          case GB_HW_EVT_IRQ_REQUEST:
               ImGui::Text("IF = %02X", ev->data);
               ImGui::SameLine(120);
               ImGui::Text("IE = %02X", ev->extra);
               {
                    const char *bits[] = {"VBlank", "STAT", "Timer", "Serial", "Joypad"};
                    ImGui::Text("Pending:");
                    for (int b = 0; b < 5; b++)
                         if (ev->data & (1 << b))
                         {
                              ImGui::SameLine();
                              ImGui::TextColored(cv, "%s", bits[b]);
                         }
               }
               break;
          case GB_HW_EVT_IRQ_ACK:
               ImGui::Text("vector = %04X", ev->addr);
               ImGui::SameLine(120);
               ImGui::Text("IF after = %02X", ev->data);
               break;
          case GB_HW_EVT_DMA_READ:
               ImGui::Text("src    = %04X", ev->addr);
               ImGui::SameLine(120);
               ImGui::Text("data = %02X", ev->data);
               break;
          case GB_HW_EVT_DMA_WRITE:
               ImGui::Text("OAM[%02X] = %02X", ev->extra, ev->data);
               break;
          case GB_HW_EVT_PPU_MODE:
               ImGui::Text("LY   = %d", ev->addr & 0xFF);
               ImGui::SameLine(120);
               ImGui::Text("mode = %d", ev->data & 3);
               break;
          case GB_HW_EVT_APU_SAMPLE:
               ImGui::Text("L = %d", (int8_t)ev->data);
               ImGui::SameLine(120);
               ImGui::Text("R = %d", (int8_t)ev->extra);
               break;
          case GB_HW_EVT_PPU_VBLANK:
               ImGui::Text("LY = %d  (VBlank start)", ev->data);
               break;
          case GB_HW_EVT_PPU_HBLANK:
               ImGui::Text("LY = %d", ev->data);
               break;
          case GB_HW_EVT_OAM_DMA:
               ImGui::Text("dst  = FE%02X", ev->extra);
               ImGui::SameLine(120);
               ImGui::Text("data = %02X", ev->data);
               ImGui::Text("pos  = %d / 159", (int)ev->extra);
               break;
          case GB_HW_EVT_TIMER_OVF:
               ImGui::Text("TMA reload = %02X", ev->data);
               break;
          case GB_HW_EVT_APU_WRITE:
               ImGui::Text("reg  = %04X", ev->addr);
               ImGui::SameLine(120);
               ImGui::Text("data = %02X", ev->data);
               break;
          case GB_HW_EVT_JOYPAD:
               ImGui::Text("state = %02X  (%s)", ev->data, ev->write ? "pressed" : "released");
               {
                    const char *btns[] = {"Right", "Left", "Up", "Down", "A", "B", "Select", "Start"};
                    ImGui::Text("Active (low=pressed):");
                    for (int b = 0; b < 8; b++)
                         if (!(ev->data & (1 << b)))
                         {
                              ImGui::SameLine();
                              ImGui::TextColored(cv, "%s", btns[b]);
                         }
               }
               break;
          default:
               ImGui::Text("timestamp = %d", ev->timestamp);
               break;
          }

          ImGui::EndChild();
          ImGui::PopStyleColor();
     }

     /* ── Fase G: Audit panel ── */
     ImGui::Separator();

     static bool s_audit_open = false;
     static HwAuditResult s_audit_result = {};
     static uint64_t s_audit_last_seq = 0;
     static bool s_audit_dirty = true; /* rerun on next open */
     static int s_audit_selected = -1;

     /* Invalidate whenever new trace events arrive */
     if (tr->next_seq > s_audit_last_seq + 1)
          s_audit_dirty = true;

     /* Colors per category */
     static const ImVec4 s_audit_cat_colors[] = {
         {1.0f, 0.4f, 0.4f, 1.0f}, /* UNMAPPED_NET  — red        */
         {1.0f, 0.8f, 0.2f, 1.0f}, /* NO_BUS_PROJ   — yellow     */
         {1.0f, 0.5f, 0.1f, 1.0f}, /* BAD_ADDR      — orange     */
         {0.9f, 0.2f, 0.9f, 1.0f}, /* BUS_CONFLICT  — magenta    */
         {0.4f, 0.8f, 1.0f, 1.0f}, /* SEQ_GAP       — cyan       */
     };

     /* Header row: category badge counts + toggle */
     int total_findings = s_audit_open ? s_audit_result.count : 0;
     ImGui::TextDisabled("Audit:");
     ImGui::SameLine();
     for (int c = 0; c < 5; c++)
     {
          if (s_audit_open)
          {
               ImGui::TextColored(s_audit_cat_colors[c], "%s:%d",
                                  HW_AUDIT_CAT_NAMES[c],
                                  s_audit_result.cat_count[c]);
          }
          else
          {
               ImGui::TextDisabled("%s", HW_AUDIT_CAT_NAMES[c]);
          }
          if (c < 4)
               ImGui::SameLine();
     }
     ImGui::SameLine();
     if (ImGui::SmallButton(s_audit_open ? "[-]" : "[Run]"))
     {
          s_audit_open = !s_audit_open;
          s_audit_dirty = true;
     }

     if (!s_audit_open)
     {
          ImGui::EndChild();
          return;
     }

     /* Run audit when dirty */
     if (s_audit_dirty)
     {
          uint64_t scan_from = 0; /* always full scan for correctness */
          hw_audit_trace(tr, &s_audit_result, &scan_from);
          s_audit_last_seq = tr->next_seq;
          s_audit_dirty = false;
          s_audit_selected = -1;
          total_findings = s_audit_result.count;
     }

     ImGui::TextDisabled("Scanned %d events — %d finding(s)",
                         s_audit_result.total_events_scanned, total_findings);

     if (total_findings == 0)
     {
          ImGui::TextColored({0.3f, 1.0f, 0.4f, 1.0f}, "No anomalies found.");
          ImGui::EndChild();
          return;
     }

     /* Finding list */
     float audit_list_h = s_audit_selected >= 0 ? 110.0f : 150.0f;
     ImGui::BeginChild("##audit_list", ImVec2(0, audit_list_h), true,
                       ImGuiWindowFlags_HorizontalScrollbar);
     ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 1));

     for (int fi = 0; fi < total_findings; fi++)
     {
          const HwAuditFinding *f = &s_audit_result.findings[fi];
          int cat = (int)f->category;
          ImVec4 cv = s_audit_cat_colors[cat];

          ImGui::TextColored(cv, "%-14s", HW_AUDIT_CAT_NAMES[cat]);
          ImGui::SameLine();
          ImGui::TextDisabled("seq:#%llu  %s",
                              (unsigned long long)f->seq, f->detail);

          /* Invisible selectable */
          ImGui::SameLine(0, 0);
          float rw = ImGui::GetContentRegionAvail().x;
          char sel_id[24];
          snprintf(sel_id, sizeof(sel_id), "##af%d", fi);
          bool sel = (s_audit_selected == fi);
          if (ImGui::Selectable(sel_id, sel,
                                ImGuiSelectableFlags_SpanAllColumns |
                                    ImGuiSelectableFlags_AllowOverlap,
                                ImVec2(rw, 0)))
               s_audit_selected = sel ? -1 : fi;
     }

     ImGui::PopStyleVar();
     ImGui::EndChild();

     /* Detail panel for selected finding */
     if (s_audit_selected >= 0 && s_audit_selected < total_findings)
     {
          const HwAuditFinding *f = &s_audit_result.findings[s_audit_selected];
          int cat = (int)f->category;
          ImVec4 cv = s_audit_cat_colors[cat];

          ImGui::PushStyleColor(ImGuiCol_ChildBg,
                                ImVec4(cv.x * 0.12f, cv.y * 0.12f, cv.z * 0.12f, 0.5f));
          ImGui::BeginChild("##audit_detail", ImVec2(0, 90), true);

          ImGui::TextColored(cv, "%s", HW_AUDIT_CAT_NAMES[cat]);
          ImGui::SameLine();
          ImGui::TextDisabled("seq:#%llu  T:%d",
                              (unsigned long long)f->seq,
                              f->timestamp);
          ImGui::Separator();
          ImGui::Text("Event type : %d", (int)f->type);
          ImGui::Text("Addr       : %04X", f->addr);
          if (f->net_id >= 0)
               ImGui::Text("Net id     : %d", f->net_id);
          ImGui::TextWrapped("Detail: %s", f->detail);

          ImGui::EndChild();
          ImGui::PopStyleColor();
     }

     ImGui::EndChild(); /* ##sch_trace_child */
}
