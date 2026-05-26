#include "gba_debug_ui.h"

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl3.h"

#include <SDL3/SDL_opengl.h>
#include <stdint.h>
#include <stdio.h>

extern "C" {
#include "gba/gba.h"
#include "gba/gba_disasm.h"
#include "gba/gba_memory.h"
}

static SDL_Window *s_window = nullptr;
static SDL_GLContext s_gl_context = nullptr;
static GLuint s_game_tex = 0;
static bool s_show_cpu = true;
static bool s_show_disasm = true;
static bool s_show_memory = true;
static bool s_show_gpu = true;
static bool s_show_screen = true;
static bool s_fast_forward = false;
static uint32_t s_mem_addr = 0x03000000;
static char s_bp_input[16] = "08000000";

static const char *debug_state_name(enum gba_debug_state state)
{
    switch (state) {
    case GBA_DEBUG_RUNNING: return "Running";
    case GBA_DEBUG_PAUSED: return "Paused";
    case GBA_DEBUG_STEPPING: return "Stepping";
    default: return "?";
    }
}

static const char *cpu_mode_name(uint32_t cpsr)
{
    switch (cpsr & GBA_CPSR_M) {
    case GBA_MODE_USR: return "USR";
    case GBA_MODE_FIQ: return "FIQ";
    case GBA_MODE_IRQ: return "IRQ";
    case GBA_MODE_SVC: return "SVC";
    case GBA_MODE_ABT: return "ABT";
    case GBA_MODE_UND: return "UND";
    case GBA_MODE_SYS: return "SYS";
    default: return "???";
    }
}

static void draw_game_output()
{
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
    ImGui::Begin("GBA Screen", &s_show_screen);
    float avail_w = ImGui::GetContentRegionAvail().x;
    float avail_h = ImGui::GetContentRegionAvail().y;
    float scale = avail_w / (float)GBA_LCD_W;
    float scale_h = avail_h / (float)GBA_LCD_H;
    if (scale_h < scale) scale = scale_h;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 5.0f) scale = 5.0f;
    ImGui::Image((ImTextureID)(intptr_t)s_game_tex,
                 ImVec2(GBA_LCD_W * scale, GBA_LCD_H * scale));
    ImGui::End();
}

static void draw_cpu_panel(struct gba *gba)
{
    struct gba_cpu *cpu = &gba->cpu;
    uint32_t pc = gba_cpu_current_pc(cpu);

    ImGui::Text("State: %s", debug_state_name(gba->debug.state));
    ImGui::SameLine();
    ImGui::Text("Mode: %s %s", cpu_mode_name(cpu->cpsr),
                (cpu->cpsr & GBA_CPSR_T) ? "THUMB" : "ARM");
    ImGui::Text("Instructions: %llu", (unsigned long long)gba->debug.instruction_count);
    ImGui::Text("Cycles: %llu", (unsigned long long)gba->debug.cycle_count);
    ImGui::Separator();

    if (ImGui::Button(gba->debug.state == GBA_DEBUG_RUNNING ? "Pause" : "Run"))
        gba->debug.state = (gba->debug.state == GBA_DEBUG_RUNNING) ? GBA_DEBUG_PAUSED
                                                                   : GBA_DEBUG_RUNNING;
    ImGui::SameLine();
    if (ImGui::Button("Step"))
        gba->debug.state = GBA_DEBUG_STEPPING;
    ImGui::SameLine();
    if (ImGui::Button("Break PC"))
        gba_debug_toggle_breakpoint(gba, pc);

    ImGui::Separator();
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int r = row * 4 + col;
            ImGui::Text("r%-2d %08x", r, cpu->r[r]);
            if (col != 3) ImGui::SameLine();
        }
    }
    ImGui::Text("PC  %08x", pc);
    ImGui::Text("CPSR %08x  [%c%c%c%c %c%c]",
                cpu->cpsr,
                (cpu->cpsr & GBA_CPSR_N) ? 'N' : '-',
                (cpu->cpsr & GBA_CPSR_Z) ? 'Z' : '-',
                (cpu->cpsr & GBA_CPSR_C) ? 'C' : '-',
                (cpu->cpsr & GBA_CPSR_V) ? 'V' : '-',
                (cpu->cpsr & GBA_CPSR_I) ? 'I' : '-',
                (cpu->cpsr & GBA_CPSR_F) ? 'F' : '-');

    ImGui::Separator();
    ImGui::SetNextItemWidth(110);
    ImGui::InputText("Breakpoint", s_bp_input, sizeof(s_bp_input),
                     ImGuiInputTextFlags_CharsHexadecimal |
                     ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if (ImGui::Button("Add")) {
        unsigned addr = 0;
        if (sscanf(s_bp_input, "%x", &addr) == 1)
            gba_debug_add_breakpoint(gba, addr);
    }
    for (unsigned i = 0; i < gba->debug.n_breakpoints; i++) {
        ImGui::PushID((int)i);
        ImGui::Checkbox("", &gba->debug.bp_enabled[i]);
        ImGui::SameLine();
        ImGui::Text("%08x", gba->debug.breakpoints[i]);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove"))
            gba_debug_remove_breakpoint(gba, i--);
        ImGui::PopID();
    }
}

static void draw_disasm_panel(struct gba *gba)
{
    struct gba_cpu *cpu = &gba->cpu;
    bool thumb = (cpu->cpsr & GBA_CPSR_T) != 0;
    uint32_t pc = gba_cpu_current_pc(cpu);
    uint32_t start = pc - (thumb ? 8u : 16u);

    for (int i = 0; i < 16; i++) {
        uint32_t addr = start + (uint32_t)i * (thumb ? 2u : 4u);
        char text[96];
        gba_disasm(gba, addr, thumb, text, sizeof(text));
        bool is_pc = addr == pc;
        bool has_bp = gba_debug_has_breakpoint(gba, addr);

        if (ImGui::Selectable(has_bp ? "B" : " ", false, 0, ImVec2(18, 0)))
            gba_debug_toggle_breakpoint(gba, addr);
        ImGui::SameLine();
        if (is_pc)
            ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.0f), "%08x  %s", addr, text);
        else
            ImGui::Text("%08x  %s", addr, text);
    }
}

static void draw_memory_panel(struct gba *gba)
{
    ImGui::SetNextItemWidth(120);
    ImGui::InputScalar("Address", ImGuiDataType_U32, &s_mem_addr, nullptr, nullptr, "%08X",
                       ImGuiInputTextFlags_CharsHexadecimal |
                       ImGuiInputTextFlags_CharsUppercase);
    s_mem_addr &= ~0xfu;
    ImGui::Separator();
    for (int row = 0; row < 16; row++) {
        uint32_t addr = s_mem_addr + (uint32_t)row * 16u;
        ImGui::Text("%08x:", addr);
        ImGui::SameLine();
        for (int i = 0; i < 16; i++) {
            ImGui::Text("%02x", gba_memory_peek8(gba, addr + (uint32_t)i));
            if (i != 15) ImGui::SameLine();
        }
    }
}

static void draw_gpu_panel(struct gba *gba)
{
    struct gba_gpu *gpu = &gba->gpu;
    ImGui::Text("DISPCNT mode=%u frame=%u forced_blank=%u obj_1d=%u",
                gpu->bg_mode, gpu->frame_select, gpu->forced_blank, gpu->obj_1d);
    ImGui::Text("VCOUNT=%u  vblank=%u hblank=%u", gpu->vcount, gpu->vblank, gpu->hblank);
    ImGui::Text("BG enable: %d %d %d %d  OBJ=%d",
                gpu->bg_en[0], gpu->bg_en[1], gpu->bg_en[2], gpu->bg_en[3], gpu->obj_en);
    ImGui::Separator();
    for (int i = 0; i < 4; i++) {
        struct gba_bg_layer *bg = &gpu->bg[i];
        ImGui::Text("BG%d prio=%u char=%u map=%u size=%u scroll=%u,%u",
                    i, bg->priority, bg->tile_base, bg->map_base, bg->size,
                    bg->hofs, bg->vofs);
    }
}

bool gba_debug_ui_init(struct gba *gba, SDL_Window *window)
{
    s_window = window;
    s_gl_context = SDL_GL_CreateContext(window);
    if (!s_gl_context) {
        fprintf(stderr, "gba_debug_ui: SDL_GL_CreateContext: %s\n", SDL_GetError());
        s_window = nullptr;
        return false;
    }

    SDL_GL_MakeCurrent(window, s_gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(window, s_gl_context);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    glGenTextures(1, &s_game_tex);
    glBindTexture(GL_TEXTURE_2D, s_game_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GBA_LCD_W, GBA_LCD_H, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture(GL_TEXTURE_2D, 0);

    gba->debug.enabled = true;
    if (gba->debug.state == GBA_DEBUG_RUNNING)
        gba->debug.state = GBA_DEBUG_PAUSED;
    return true;
}

void gba_debug_ui_process_event(struct gba *gba, SDL_Event *event)
{
    if (!s_window)
        return;
    ImGui_ImplSDL3_ProcessEvent(event);
    if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat) {
        if (event->key.key == SDLK_F5)
            gba->debug.state = (gba->debug.state == GBA_DEBUG_RUNNING) ? GBA_DEBUG_PAUSED
                                                                       : GBA_DEBUG_RUNNING;
        if (event->key.key == SDLK_F10)
            gba->debug.state = GBA_DEBUG_STEPPING;
        if (event->key.key == SDLK_TAB)
            s_fast_forward = true;
    } else if (event->type == SDL_EVENT_KEY_UP && event->key.key == SDLK_TAB) {
        s_fast_forward = false;
    }
}

void gba_debug_ui_render(struct gba *gba, const uint32_t *pixels)
{
    if (!s_window || !s_gl_context)
        return;

    SDL_GL_MakeCurrent(s_window, s_gl_context);
    glBindTexture(GL_TEXTURE_2D, s_game_tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GBA_LCD_W, GBA_LCD_H,
                    GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Screen", nullptr, &s_show_screen);
            ImGui::MenuItem("CPU", nullptr, &s_show_cpu);
            ImGui::MenuItem("Disassembly", nullptr, &s_show_disasm);
            ImGui::MenuItem("Memory", nullptr, &s_show_memory);
            ImGui::MenuItem("GPU", nullptr, &s_show_gpu);
            ImGui::EndMenu();
        }
        ImGui::Text("PC %08x  %s", gba_cpu_current_pc(&gba->cpu),
                    debug_state_name(gba->debug.state));
        ImGui::EndMainMenuBar();
    }

    if (s_show_screen)
        draw_game_output();
    if (s_show_cpu) {
        ImGui::SetNextWindowSize(ImVec2(500, 430), ImGuiCond_FirstUseEver);
        ImGui::Begin("GBA CPU / Control", &s_show_cpu);
        draw_cpu_panel(gba);
        ImGui::End();
    }
    if (s_show_disasm) {
        ImGui::SetNextWindowSize(ImVec2(520, 420), ImGuiCond_FirstUseEver);
        ImGui::Begin("GBA Disassembly", &s_show_disasm);
        draw_disasm_panel(gba);
        ImGui::End();
    }
    if (s_show_memory) {
        ImGui::SetNextWindowSize(ImVec2(650, 360), ImGuiCond_FirstUseEver);
        ImGui::Begin("GBA Memory", &s_show_memory);
        draw_memory_panel(gba);
        ImGui::End();
    }
    if (s_show_gpu) {
        ImGui::SetNextWindowSize(ImVec2(470, 260), ImGuiCond_FirstUseEver);
        ImGui::Begin("GBA GPU", &s_show_gpu);
        draw_gpu_panel(gba);
        ImGui::End();
    }

    ImGui::Render();
    int w, h;
    SDL_GetWindowSizeInPixels(s_window, &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.06f, 0.07f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(s_window);
}

void gba_debug_ui_destroy(void)
{
    if (!s_window)
        return;
    if (s_game_tex)
        glDeleteTextures(1, &s_game_tex);
    s_game_tex = 0;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(s_gl_context);
    s_gl_context = nullptr;
    s_window = nullptr;
}

float gba_debug_ui_speed_multiplier(void)
{
    return s_fast_forward ? 4.0f : 1.0f;
}
