#include "gba_debug_ui_panels.h"

#include "imgui.h"

#include <string.h>
#include <stdio.h>

extern "C" {
#include "gba/gba.h"
#include "gba/gba_cpu.h"
#include "gba/gba_debug.h"
#include "gba/gba_disasm.h"
#include "gba/gba_memory.h"
}

/* ── Shared state ── */

uint32_t g_gba_mem_addr = 0x03000000;

/* ── Cores ── */

static ImVec4 col_green  = ImVec4(0.2f, 0.9f, 0.2f, 1.0f);
static ImVec4 col_red    = ImVec4(0.9f, 0.2f, 0.2f, 1.0f);
static ImVec4 col_yellow = ImVec4(0.9f, 0.9f, 0.1f, 1.0f);
static ImVec4 col_gray   = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
static ImVec4 col_cyan   = ImVec4(0.2f, 0.9f, 0.9f, 1.0f);
static ImVec4 col_orange = ImVec4(1.0f, 0.55f,0.1f, 1.0f);

/* ── Init / Shutdown ── */

void gba_debug_ui_panels_init(void)  {}
void gba_debug_ui_panels_shutdown(void) {}

/* ── Helpers ── */

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

/* ── CPU / Controle ── */

void gba_draw_panel_cpu_control(struct gba *gba)
{
    struct gba_cpu *cpu = &gba->cpu;
    uint32_t pc  = gba_cpu_current_pc(cpu);
    bool thumb   = (cpu->cpsr & GBA_CPSR_T) != 0;

    /* Botões de controle */
    if (ImGui::Button(gba->debug.state == GBA_DEBUG_RUNNING ? "  Pausar  " : "  Rodar   "))
        gba->debug.state = (gba->debug.state == GBA_DEBUG_RUNNING) ? GBA_DEBUG_PAUSED
                                                                    : GBA_DEBUG_RUNNING;
    ImGui::SameLine();
    if (ImGui::Button("  Step  "))
        gba->debug.state = GBA_DEBUG_STEPPING;
    ImGui::SameLine();
    if (ImGui::Button("  Reset  "))
        gba_reset(gba);

    ImGui::Separator();

    /* Estado */
    ImGui::TextColored(col_cyan, "PC"); ImGui::SameLine();
    ImGui::Text("%08x", pc);            ImGui::SameLine();
    ImGui::TextColored(col_gray, "|"); ImGui::SameLine();
    ImGui::Text("%s  %s", cpu_mode_name(cpu->cpsr), thumb ? "THUMB" : "ARM");
    ImGui::SameLine(); ImGui::TextColored(col_gray, "|"); ImGui::SameLine();
    ImGui::Text("ins: %llu", (unsigned long long)gba->debug.instruction_count);

    ImGui::Separator();

    /* Flags CPSR */
    auto flag = [&](const char *name, uint32_t bit, ImVec4 col_on) {
        bool on = (cpu->cpsr & bit) != 0;
        ImGui::TextColored(on ? col_on : col_gray, "%s", name);
        ImGui::SameLine();
    };
    ImGui::Text("CPSR:"); ImGui::SameLine();
    flag("N", GBA_CPSR_N, col_orange);
    flag("Z", GBA_CPSR_Z, col_green);
    flag("C", GBA_CPSR_C, col_cyan);
    flag("V", GBA_CPSR_V, col_red);
    flag("I", GBA_CPSR_I, col_yellow);
    flag("F", GBA_CPSR_F, col_yellow);
    ImGui::NewLine();

    ImGui::Separator();

    /* Registradores r0–r15 */
    if (ImGui::BeginTable("gba_regs", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Reg", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("Val", ImGuiTableColumnFlags_WidthStretch);
        for (int i = 0; i < 16; i++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            /* Alias para registradores especiais */
            const char *alias[] = {
                "r0","r1","r2","r3","r4","r5","r6","r7",
                "r8","r9","r10","r11","r12","SP","LR","PC"
            };
            bool is_pc = (i == 15);
            ImGui::TextColored(is_pc ? col_cyan : col_gray, "%s", alias[i]);
            ImGui::TableNextColumn();
            if (is_pc)
                ImGui::TextColored(col_cyan, "%08x", pc);
            else
                ImGui::Text("%08x", cpu->r[i]);
        }
        ImGui::TableNextRow(); ImGui::TableNextColumn();
        ImGui::TextColored(col_gray, "CPSR");
        ImGui::TableNextColumn();
        ImGui::Text("%08x", cpu->cpsr);
        ImGui::EndTable();
    }

    ImGui::Separator();

    /* Breakpoints */
    static char s_bp_input[16] = "08000000";
    ImGui::Text("Breakpoints");
    ImGui::SetNextItemWidth(130);
    ImGui::InputText("##bp_addr", s_bp_input, sizeof(s_bp_input),
                     ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
    ImGui::SameLine();
    if (ImGui::Button("Adicionar")) {
        unsigned addr = 0;
        if (sscanf(s_bp_input, "%x", &addr) == 1)
            gba_debug_add_breakpoint(gba, addr);
    }
    ImGui::SameLine();
    if (ImGui::Button("Limpar todos")) {
        while (gba->debug.n_breakpoints > 0)
            gba_debug_remove_breakpoint(gba, 0);
    }

    for (unsigned i = 0; i < gba->debug.n_breakpoints; i++) {
        ImGui::PushID((int)i);
        ImGui::Checkbox("##bp_en", &gba->debug.bp_enabled[i]);
        ImGui::SameLine();
        bool is_cur = gba->debug.breakpoints[i] == pc;
        ImGui::TextColored(is_cur ? col_green : col_gray, "%08x", gba->debug.breakpoints[i]);
        ImGui::SameLine();
        if (ImGui::SmallButton("X"))
            gba_debug_remove_breakpoint(gba, i--);
        ImGui::PopID();
    }
}

/* ── Disassembly ── */

void gba_draw_panel_disasm(struct gba *gba)
{
    struct gba_cpu *cpu = &gba->cpu;
    bool thumb   = (cpu->cpsr & GBA_CPSR_T) != 0;
    uint32_t pc  = gba_cpu_current_pc(cpu);
    uint32_t step = thumb ? 2u : 4u;

    /* Mostra 8 instruções antes e 16 depois do PC */
    uint32_t start = pc - step * 8u;

    for (int i = 0; i < 28; i++) {
        uint32_t addr = start + (uint32_t)i * step;
        char text[128];
        gba_disasm(gba, addr, thumb, text, sizeof(text));

        bool is_pc = (addr == pc);
        bool has_bp = gba_debug_has_breakpoint(gba, addr);

        /* Marcador de breakpoint clicável */
        ImGui::PushID((int)i);
        ImVec4 bp_col = has_bp ? col_red : col_gray;
        ImGui::TextColored(bp_col, has_bp ? "\xe2\x97\x8f" : " ");
        if (ImGui::IsItemClicked())
            gba_debug_toggle_breakpoint(gba, addr);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Clique para toggle breakpoint");
        ImGui::SameLine();
        ImGui::PopID();

        if (is_pc)
            ImGui::TextColored(col_green, "%08x  %s", addr, text);
        else
            ImGui::TextUnformatted(""), ImGui::SameLine(),
            ImGui::Text("%08x  %s", addr, text);
    }
}

/* ── Memory viewer ── */

void gba_draw_panel_memory(struct gba *gba)
{
    /* Seletor de região */
    struct { const char *name; uint32_t addr; } regions[] = {
        {"BIOS  (0000)",  0x00000000},
        {"EWRAM (0200)",  0x02000000},
        {"IWRAM (0300)",  0x03000000},
        {"IO    (0400)",  0x04000000},
        {"PAL   (0500)",  0x05000000},
        {"VRAM  (0600)",  0x06000000},
        {"OAM   (0700)",  0x07000000},
        {"ROM   (0800)",  0x08000000},
    };
    static int s_region = 2;
    ImGui::Text("Região:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180);
    if (ImGui::BeginCombo("##mem_region", regions[s_region].name)) {
        for (int i = 0; i < (int)(sizeof(regions)/sizeof(regions[0])); i++) {
            if (ImGui::Selectable(regions[i].name, s_region == i)) {
                s_region = i;
                g_gba_mem_addr = regions[i].addr;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110);
    ImGui::InputScalar("##mem_addr", ImGuiDataType_U32, &g_gba_mem_addr,
                       nullptr, nullptr, "%08X",
                       ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase);
    g_gba_mem_addr &= ~0xFu;

    ImGui::Separator();

    /* Header de colunas */
    ImGui::TextColored(col_gray, "Address  ");
    ImGui::SameLine();
    for (int i = 0; i < 16; i++) {
        ImGui::TextColored(col_gray, "%02X", i);
        if (i != 15) ImGui::SameLine();
    }

    ImGui::Separator();

    /* Dump — 24 linhas de 16 bytes */
    for (int row = 0; row < 24; row++) {
        uint32_t base = g_gba_mem_addr + (uint32_t)row * 16u;
        ImGui::TextColored(col_cyan, "%08x", base);
        ImGui::SameLine();
        for (int col = 0; col < 16; col++) {
            uint8_t val = gba_memory_peek8(gba, base + (uint32_t)col);
            ImGui::Text("%02x", val);
            if (col != 15) ImGui::SameLine();
        }
    }

    /* Botões de navegação */
    ImGui::Separator();
    if (ImGui::Button("- 256"))  g_gba_mem_addr -= 256;
    ImGui::SameLine();
    if (ImGui::Button("+ 256"))  g_gba_mem_addr += 256;
    ImGui::SameLine();
    if (ImGui::Button("- 4KB"))  g_gba_mem_addr -= 0x1000;
    ImGui::SameLine();
    if (ImGui::Button("+ 4KB"))  g_gba_mem_addr += 0x1000;
}

/* ── GPU / PPU ── */

void gba_draw_panel_gpu(struct gba *gba)
{
    struct gba_gpu *gpu = &gba->gpu;

    /* DISPCNT */
    ImGui::Text("DISPCNT");
    ImGui::Separator();
    ImGui::Text("Modo BG:      %u", gpu->bg_mode);
    ImGui::SameLine(); ImGui::TextColored(col_gray, "|"); ImGui::SameLine();
    ImGui::Text("Frame select: %u", (unsigned)gpu->frame_select);
    ImGui::Text("Forced blank: %s", gpu->forced_blank ? "SIM" : "nao");
    ImGui::SameLine(); ImGui::TextColored(col_gray, "|"); ImGui::SameLine();
    ImGui::Text("OBJ 1D map:   %s", gpu->obj_1d ? "SIM" : "nao");

    /* Enable flags */
    ImGui::Text("BG enable: ");
    for (int i = 0; i < 4; i++) {
        ImGui::SameLine();
        ImGui::TextColored(gpu->bg_en[i] ? col_green : col_gray, "BG%d", i);
    }
    ImGui::SameLine();
    ImGui::TextColored(gpu->obj_en ? col_green : col_gray, " OBJ");

    ImGui::Separator();

    /* VCOUNT / timing */
    ImGui::Text("VCOUNT: %3u", gpu->vcount);
    ImGui::SameLine(); ImGui::TextColored(col_gray, "|"); ImGui::SameLine();
    ImGui::TextColored(gpu->vblank ? col_green : col_gray, "VBlank");
    ImGui::SameLine(); ImGui::TextColored(col_gray, "|"); ImGui::SameLine();
    ImGui::TextColored(gpu->hblank ? col_cyan   : col_gray, "HBlank");

    ImGui::Separator();

    /* Camadas BG */
    if (ImGui::BeginTable("gba_bg", 7,
            ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("BG");
        ImGui::TableSetupColumn("Prio");
        ImGui::TableSetupColumn("Char");
        ImGui::TableSetupColumn("Map");
        ImGui::TableSetupColumn("Size");
        ImGui::TableSetupColumn("HScr");
        ImGui::TableSetupColumn("VScr");
        ImGui::TableHeadersRow();
        for (int i = 0; i < 4; i++) {
            struct gba_bg_layer *bg = &gpu->bg[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(gpu->bg_en[i] ? col_green : col_gray, "BG%d", i);
            ImGui::TableNextColumn(); ImGui::Text("%u", bg->priority);
            ImGui::TableNextColumn(); ImGui::Text("%u", bg->tile_base);
            ImGui::TableNextColumn(); ImGui::Text("%u", bg->map_base);
            ImGui::TableNextColumn(); ImGui::Text("%u", bg->size);
            ImGui::TableNextColumn(); ImGui::Text("%u", bg->hofs);
            ImGui::TableNextColumn(); ImGui::Text("%u", bg->vofs);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    /* Blend */
    ImGui::Text("BLDCNT: %04x  EVA:%u  EVB:%u  BLDY:%u",
                gpu->bldcnt, gpu->bldalpha_eva, gpu->bldalpha_evb, gpu->bldy);

    /* Window */
    if (gpu->win0_en || gpu->win1_en || gpu->winobj_en) {
        ImGui::Separator();
        ImGui::Text("Janelas:"); ImGui::SameLine();
        ImGui::TextColored(gpu->win0_en   ? col_green : col_gray, " WIN0");
        ImGui::SameLine();
        ImGui::TextColored(gpu->win1_en   ? col_green : col_gray, " WIN1");
        ImGui::SameLine();
        ImGui::TextColored(gpu->winobj_en ? col_green : col_gray, " WINOBJ");
        ImGui::Text("WIN0: x=%u-%u  y=%u-%u",
                    gpu->win0_x1, gpu->win0_x2, gpu->win0_y1, gpu->win0_y2);
        ImGui::Text("WIN1: x=%u-%u  y=%u-%u",
                    gpu->win1_x1, gpu->win1_x2, gpu->win1_y1, gpu->win1_y2);
    }
}

/* ── OAM (Sprites) ── */

void gba_draw_panel_oam(struct gba *gba)
{
    ImGui::Text("128 entradas OAM (GBA)");
    ImGui::Separator();

    if (ImGui::BeginTable("gba_oam", 8,
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersOuter |
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit,
            ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("X");
        ImGui::TableSetupColumn("Y");
        ImGui::TableSetupColumn("Mode");
        ImGui::TableSetupColumn("Tile");
        ImGui::TableSetupColumn("Prio");
        ImGui::TableSetupColumn("Pal");
        ImGui::TableSetupColumn("Flags");
        ImGui::TableHeadersRow();

        /* Lê OAM diretamente da memória */
        for (int i = 0; i < 128; i++) {
            uint32_t base = 0x07000000 + (uint32_t)i * 8u;
            uint16_t attr0 = gba_memory_peek8(gba, base)     | ((uint16_t)gba_memory_peek8(gba, base+1) << 8);
            uint16_t attr1 = gba_memory_peek8(gba, base+2)   | ((uint16_t)gba_memory_peek8(gba, base+3) << 8);
            uint16_t attr2 = gba_memory_peek8(gba, base+4)   | ((uint16_t)gba_memory_peek8(gba, base+5) << 8);

            uint8_t obj_mode = (attr0 >> 8) & 3;
            bool disabled = (obj_mode == 2);

            int16_t x = (int16_t)(attr1 & 0x1FF);
            if (x >= 240) x -= 512;
            int16_t y = (int8_t)(attr0 & 0xFF);

            uint16_t tile  = attr2 & 0x3FF;
            uint8_t  prio  = (attr2 >> 10) & 3;
            uint8_t  pal   = (attr2 >> 12) & 0xF;
            bool     hflip = (attr1 >> 12) & 1;
            bool     vflip = (attr1 >> 13) & 1;
            bool     c256  = (attr0 >> 13) & 1;

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(disabled ? col_gray : col_green, "%d", i);
            ImGui::TableNextColumn(); ImGui::Text("%d", x);
            ImGui::TableNextColumn(); ImGui::Text("%d", y);
            ImGui::TableNextColumn();
            const char *modes[] = {"Norm","Aff","Dis","AffDbl"};
            ImGui::Text("%s", modes[obj_mode & 3]);
            ImGui::TableNextColumn(); ImGui::Text("%u", tile);
            ImGui::TableNextColumn(); ImGui::Text("%u", prio);
            ImGui::TableNextColumn(); ImGui::Text("%u", pal);
            ImGui::TableNextColumn();
            ImGui::Text("%s%s%s",
                        c256  ? "256 " : "16 ",
                        hflip ? "H"    : "-",
                        vflip ? "V"    : "-");
        }
        ImGui::EndTable();
    }
}

/* ── APU ── */

void gba_draw_panel_apu(struct gba *gba)
{
    struct gba_apu *apu = &gba->apu;
    bool master = (apu->soundcnt_x & 0x80) != 0;

    ImGui::TextColored(master ? col_green : col_red,
                       "APU Master: %s", master ? "ON" : "OFF");
    ImGui::Text("SOUNDCNT_L: %04x  H: %04x  X: %02x",
                apu->soundcnt_l, apu->soundcnt_h, apu->soundcnt_x);
    ImGui::Text("SOUNDBIAS: %04x", apu->soundbias);

    ImGui::Separator();

    auto ch_row = [&](const char *name, bool en, bool dac, bool muted) {
        ImGui::TextColored(en && !muted ? col_green : col_gray, "%-14s", name);
        ImGui::SameLine();
        ImGui::TextColored(dac  ? col_cyan  : col_gray, "DAC:%s", dac  ? "ON " : "OFF");
        ImGui::SameLine();
        ImGui::TextColored(muted ? col_yellow : col_gray, "%s", muted ? "MUDO" : "    ");
    };

    ch_row("CH1 Pulse+Sweep", apu->ch1.enabled, apu->ch1.dac_on, apu->mute_ch1);
    ch_row("CH2 Pulse",       apu->ch2.enabled, apu->ch2.dac_on, apu->mute_ch2);
    ch_row("CH3 Wave",        apu->ch3.enabled, apu->ch3.dac_on, apu->mute_ch3);
    ch_row("CH4 Noise",       apu->ch4.enabled, apu->ch4.dac_on, apu->mute_ch4);

    ImGui::Separator();

    /* FIFO */
    ImGui::Text("FIFO A: timer=%u  L=%s R=%s  len=%d  sample=%d",
                apu->fifo_a.timer_sel,
                apu->fifo_a.l_en ? "ON" : "off",
                apu->fifo_a.r_en ? "ON" : "off",
                apu->fifo_a.len,
                (int)apu->fifo_a.sample);
    ImGui::Text("FIFO B: timer=%u  L=%s R=%s  len=%d  sample=%d",
                apu->fifo_b.timer_sel,
                apu->fifo_b.l_en ? "ON" : "off",
                apu->fifo_b.r_en ? "ON" : "off",
                apu->fifo_b.len,
                (int)apu->fifo_b.sample);

    ImGui::Separator();

    /* Mute toggles */
    ImGui::Text("Mute:");
    ImGui::SameLine(); ImGui::Checkbox("CH1##m", &apu->mute_ch1);
    ImGui::SameLine(); ImGui::Checkbox("CH2##m", &apu->mute_ch2);
    ImGui::SameLine(); ImGui::Checkbox("CH3##m", &apu->mute_ch3);
    ImGui::SameLine(); ImGui::Checkbox("CH4##m", &apu->mute_ch4);
    ImGui::SameLine(); ImGui::Checkbox("FIFO A##m", &apu->mute_fifo_a);
    ImGui::SameLine(); ImGui::Checkbox("FIFO B##m", &apu->mute_fifo_b);

    ImGui::Text("Sample Rate: %d Hz", GBA_APU_SAMPLE_RATE);
}

/* ── Profiler ── */

void gba_draw_panel_profiler(struct gba *gba)
{
    ImGui::Text("Instruções: %llu", (unsigned long long)gba->debug.instruction_count);
    ImGui::Text("Ciclos:     %llu", (unsigned long long)gba->debug.cycle_count);

    if (gba->debug.cycle_count > 0) {
        double cpi = (double)gba->debug.cycle_count / (double)gba->debug.instruction_count;
        ImGui::Text("CPI médio:  %.3f", cpi);
    }

    ImGui::Separator();
    ImGui::Text("CPU: %.2f MHz (%.2f%%)",
                (double)GBA_CPU_FREQ_HZ / 1e6,
                100.0);

    ImGui::Separator();

    /* Event trace */
    ImGui::Text("Últimos eventos de branch/IRQ:");
    if (ImGui::BeginTable("gba_trace", 4,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Tipo");
        ImGui::TableSetupColumn("PC");
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Detalhe");
        ImGui::TableHeadersRow();

        for (int i = 0; i < GBA_EVENT_TRACE_SIZE; i++) {
            int idx = (gba->event_trace_head - 1 - i + GBA_EVENT_TRACE_SIZE) % GBA_EVENT_TRACE_SIZE;
            struct gba_event_trace_entry *e = &gba->event_trace[idx];
            if (!e->kind) continue;

            const char *kind = "?";
            switch (e->kind) {
            case GBA_EVENT_BRANCH:    kind = "Branch"; break;
            case GBA_EVENT_SWI:       kind = "SWI";    break;
            case GBA_EVENT_IRQ_EDGE:  kind = "IRQ?";   break;
            case GBA_EVENT_IRQ_ENTER: kind = "IRQ!";   break;
            case GBA_EVENT_DMA:       kind = "DMA";    break;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", kind);
            ImGui::TableNextColumn(); ImGui::Text("%08x", e->pc);
            ImGui::TableNextColumn(); ImGui::Text("%08x", e->target);
            ImGui::TableNextColumn(); ImGui::Text("%u", e->detail);
        }
        ImGui::EndTable();
    }
}
