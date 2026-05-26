#include "debug_ui_menus.h"

#include "imgui.h"
#include <SDL3/SDL.h>

extern "C"
{
#include "gb.h"
#include "sdl.h"
}

struct input_bind_row
{
     const char *action;
     unsigned button;
};

static const input_bind_row k_input_binds[] = {
    {"D-Pad Cima", GB_INPUT_UP},
    {"D-Pad Baixo", GB_INPUT_DOWN},
    {"D-Pad Esq.", GB_INPUT_LEFT},
    {"D-Pad Dir.", GB_INPUT_RIGHT},
    {"Bot\xc3\xa3o A", GB_INPUT_A},
    {"Bot\xc3\xa3o B", GB_INPUT_B},
    {"Start", GB_INPUT_START},
    {"Select", GB_INPUT_SELECT},
};

struct gamepad_bind_row
{
     const char *action;
     const char *button;
};

static const gamepad_bind_row k_gamepad_binds[] = {
    {"D-Pad", "D-Pad"},
    {"A", "East (B no Xbox)"},
    {"B", "South (A no Xbox)"},
    {"Start", "Start"},
    {"Select", "Back"},
};

static const ImVec4 color_cyan = ImVec4(0.2f, 0.9f, 0.9f, 1.0f);
static const ImVec4 color_white = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);

static void draw_keyboard_mapping_table(void)
{
     ImGui::TextDisabled("Teclado - Mapeamento atual:");
     ImGui::Separator();

     if (ImGui::BeginTable("##kbinds", 2,
                           ImGuiTableFlags_BordersOuter |
                               ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_RowBg))
     {
          ImGui::TableSetupColumn("A\xc3\xa7\xc3\xa3o", ImGuiTableColumnFlags_WidthFixed, 120);
          ImGui::TableSetupColumn("Tecla", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableHeadersRow();

          for (const auto &b : k_input_binds)
          {
               SDL_Keycode key = gb_sdl_get_key_mapping(b.button);
               ImGui::TableNextRow();
               ImGui::TableSetColumnIndex(0);
               ImGui::TextColored(color_cyan, "%s", b.action);
               ImGui::TableSetColumnIndex(1);
               ImGui::TextColored(color_white, "%s", SDL_GetKeyName(key));
          }
          ImGui::EndTable();
     }
}

static void draw_keyboard_mapping_editor(void)
{
     if (!ImGui::BeginMenu("Editar teclado"))
          return;

     ImGui::TextDisabled("Informe SDL_Keycode decimal; salve em Prefer\xc3\xaancias.");
     for (const auto &b : k_input_binds)
     {
          int key = (int)gb_sdl_get_key_mapping(b.button);
          ImGui::PushID((int)b.button);
          if (ImGui::InputInt(b.action, &key))
               gb_sdl_set_key_mapping(b.button, (SDL_Keycode)key);
          ImGui::PopID();
     }
     if (ImGui::MenuItem("Restaurar padr\xc3\xa3o"))
          gb_sdl_reset_key_mapping();
     ImGui::EndMenu();
}

static void draw_gamepad_mapping_table(void)
{
     ImGui::TextDisabled("Gamepad - Bot\xc3\xb5"
                         "es:");
     ImGui::Separator();

     if (ImGui::BeginTable("##gbinds", 2,
                           ImGuiTableFlags_BordersOuter |
                               ImGuiTableFlags_BordersInnerV |
                               ImGuiTableFlags_RowBg))
     {
          ImGui::TableSetupColumn("A\xc3\xa7\xc3\xa3o", ImGuiTableColumnFlags_WidthFixed, 120);
          ImGui::TableSetupColumn("Bot\xc3\xa3o", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableHeadersRow();

          for (const auto &b : k_gamepad_binds)
          {
               ImGui::TableNextRow();
               ImGui::TableSetColumnIndex(0);
               ImGui::TextColored(color_cyan, "%s", b.action);
               ImGui::TableSetColumnIndex(1);
               ImGui::TextColored(color_white, "%s", b.button);
          }
          ImGui::EndTable();
     }
}

void draw_input_menu(void)
{
     if (!ImGui::BeginMenu("Entrada"))
          return;

     draw_keyboard_mapping_table();
     draw_keyboard_mapping_editor();

     ImGui::Separator();
     draw_gamepad_mapping_table();

     ImGui::EndMenu();
}
