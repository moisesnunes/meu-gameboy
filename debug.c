#include <string.h>
#include <stdio.h>
#include "gb.h"
#include "debug.h"
#include "memory.h"
#include "disasm.h"

static FILE *s_trace_file = NULL;

bool gb_debug_trace_enabled(void)
{
    return s_trace_file != NULL;
}

bool gb_debug_trace_set_enabled(struct gb *gb, bool enabled, const char *path)
{
    (void)gb;

    if (!enabled)
    {
        if (s_trace_file)
        {
            fclose(s_trace_file);
            s_trace_file = NULL;
        }
        return true;
    }

    if (s_trace_file)
        return true;

    s_trace_file = fopen(path ? path : "trace.log", "w");
    return s_trace_file != NULL;
}

void gb_debug_init(struct gb *gb)
{
    struct gb_debug *dbg = &gb->debug;
    memset(dbg, 0, sizeof(*dbg));
    dbg->enabled = true;
    dbg->state   = GB_DEBUG_RUNNING;
}

/*
 * gb_debug_before_instr — chamado no início de cada instrução em gb_cpu_run_cycles.
 *
 * Verifica se o PC atual bate com algum breakpoint. Se bater, pausa.
 * Em modo STEPPING, pausa após a instrução já ter sido permitida rodar
 * (o caller verifica o estado antes de chamar esta função).
 */
void gb_debug_before_instr(struct gb *gb)
{
    struct gb_debug *dbg = &gb->debug;
    unsigned i;

    dbg->instruction_count++;

    /* ── CPU viz: capture per-instruction state ── */
    {
        /* ALU kind per opcode. CB-prefixed handled separately. */
        static const uint8_t alu_map[256] = {
            /* 0x00-0x0F */
            0,0,0,0, GB_VIZ_ALU_INC,GB_VIZ_ALU_DEC,0,0,
            0,GB_VIZ_ALU_ADD,0,0, GB_VIZ_ALU_INC,GB_VIZ_ALU_DEC,0,0,
            /* 0x10-0x1F */
            0,0,0,0, GB_VIZ_ALU_INC,GB_VIZ_ALU_DEC,0,0,
            0,GB_VIZ_ALU_ADD,0,0, GB_VIZ_ALU_INC,GB_VIZ_ALU_DEC,0,0,
            /* 0x20-0x2F */
            0,0,0,0, GB_VIZ_ALU_INC,GB_VIZ_ALU_DEC,0,0,
            0,GB_VIZ_ALU_ADD,0,0, GB_VIZ_ALU_INC,GB_VIZ_ALU_DEC,0,GB_VIZ_ALU_CP,
            /* 0x30-0x3F */
            0,0,0,0, GB_VIZ_ALU_INC,GB_VIZ_ALU_DEC,0,0,
            0,GB_VIZ_ALU_ADD,0,0, GB_VIZ_ALU_INC,GB_VIZ_ALU_DEC,0,GB_VIZ_ALU_CP,
            /* 0x40-0x7F: LD r,r / HALT */
            0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
            0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
            /* 0x80-0x87: ADD A,r */
            GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,
            GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,
            /* 0x88-0x8F: ADC A,r */
            GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,
            GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,GB_VIZ_ALU_ADD,
            /* 0x90-0x97: SUB r */
            GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,
            GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,
            /* 0x98-0x9F: SBC A,r */
            GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,
            GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,GB_VIZ_ALU_SUB,
            /* 0xA0-0xA7: AND r */
            GB_VIZ_ALU_AND,GB_VIZ_ALU_AND,GB_VIZ_ALU_AND,GB_VIZ_ALU_AND,
            GB_VIZ_ALU_AND,GB_VIZ_ALU_AND,GB_VIZ_ALU_AND,GB_VIZ_ALU_AND,
            /* 0xA8-0xAF: XOR r */
            GB_VIZ_ALU_XOR,GB_VIZ_ALU_XOR,GB_VIZ_ALU_XOR,GB_VIZ_ALU_XOR,
            GB_VIZ_ALU_XOR,GB_VIZ_ALU_XOR,GB_VIZ_ALU_XOR,GB_VIZ_ALU_XOR,
            /* 0xB0-0xB7: OR r */
            GB_VIZ_ALU_OR,GB_VIZ_ALU_OR,GB_VIZ_ALU_OR,GB_VIZ_ALU_OR,
            GB_VIZ_ALU_OR,GB_VIZ_ALU_OR,GB_VIZ_ALU_OR,GB_VIZ_ALU_OR,
            /* 0xB8-0xBF: CP r */
            GB_VIZ_ALU_CP,GB_VIZ_ALU_CP,GB_VIZ_ALU_CP,GB_VIZ_ALU_CP,
            GB_VIZ_ALU_CP,GB_VIZ_ALU_CP,GB_VIZ_ALU_CP,GB_VIZ_ALU_CP,
            /* 0xC0-0xFF: misc — mark immediate-operand ALU ops */
            0,0,0,0,0,0,GB_VIZ_ALU_ADD,0, 0,0,0,0,0,0,GB_VIZ_ALU_ADD,0,
            0,0,0,0,0,0,GB_VIZ_ALU_SUB,0, 0,0,0,0,0,0,GB_VIZ_ALU_SUB,0,
            0,0,0,0,0,0,GB_VIZ_ALU_AND,0, 0,0,0,0,0,0,GB_VIZ_ALU_XOR,0,
            0,0,0,0,0,0,GB_VIZ_ALU_OR, 0, 0,0,0,0,0,0,GB_VIZ_ALU_CP, 0,
        };

        /*
         * Source register per opcode: which register feeds the operation.
         * Indexed by opcode; dst follows by convention (usually A for ALU).
         */
        static const uint8_t src_map[256] = {
            /* 0x00-0x3F: misc loads/inc/dec */
            0,0,0,0, GB_VIZ_REG_B,GB_VIZ_REG_B,0,0,
            0,GB_VIZ_REG_BC,0,0, GB_VIZ_REG_C,GB_VIZ_REG_C,0,0,
            0,0,0,0, GB_VIZ_REG_D,GB_VIZ_REG_D,0,0,
            0,GB_VIZ_REG_DE,0,0, GB_VIZ_REG_E,GB_VIZ_REG_E,0,0,
            0,0,0,0, GB_VIZ_REG_H,GB_VIZ_REG_H,0,0,
            0,GB_VIZ_REG_HL,0,0, GB_VIZ_REG_L,GB_VIZ_REG_L,0,GB_VIZ_REG_A,
            0,0,0,0, GB_VIZ_REG_MEM,GB_VIZ_REG_MEM,0,0,
            0,GB_VIZ_REG_SP,0,0, GB_VIZ_REG_A,GB_VIZ_REG_A,0,GB_VIZ_REG_A,
            /* 0x40-0x47: LD B,r */
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            /* 0x48-0x4F: LD C,r */
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            /* 0x50-0x57: LD D,r */
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            /* 0x58-0x5F: LD E,r */
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            /* 0x60-0x67: LD H,r */
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            /* 0x68-0x6F: LD L,r */
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            /* 0x70-0x77: LD (HL),r */
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,0,GB_VIZ_REG_A,
            /* 0x78-0x7F: LD A,r */
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            /* 0x80-0xBF: ALU A,r — src is the r operand */
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            GB_VIZ_REG_B,GB_VIZ_REG_C,GB_VIZ_REG_D,GB_VIZ_REG_E,
            GB_VIZ_REG_H,GB_VIZ_REG_L,GB_VIZ_REG_MEM,GB_VIZ_REG_A,
            /* 0xC0-0xFF: misc */
            0,GB_VIZ_REG_BC,GB_VIZ_REG_IMM16,0,0,GB_VIZ_REG_BC,GB_VIZ_REG_IMM8,0,
            0,0,GB_VIZ_REG_IMM16,0,0,0,GB_VIZ_REG_IMM8,0,
            0,GB_VIZ_REG_DE,0,0,0,GB_VIZ_REG_DE,GB_VIZ_REG_IMM8,0,
            0,0,0,0,0,0,GB_VIZ_REG_IMM8,0,
            GB_VIZ_REG_A,GB_VIZ_REG_HL,GB_VIZ_REG_A,0,0,GB_VIZ_REG_HL,GB_VIZ_REG_IMM8,0,
            0,GB_VIZ_REG_SP,0,0,0,0,GB_VIZ_REG_IMM8,0,
            GB_VIZ_REG_A,GB_VIZ_REG_NONE,GB_VIZ_REG_A,0,0,GB_VIZ_REG_NONE,GB_VIZ_REG_IMM8,0,
            0,GB_VIZ_REG_HL,0,0,0,0,GB_VIZ_REG_IMM8,0,
        };

        /* Destination register per opcode */
        static const uint8_t dst_map[256] = {
            /* 0x00-0x3F */
            0,0,0,0, GB_VIZ_REG_B,GB_VIZ_REG_B,0,0,
            0,GB_VIZ_REG_HL,0,0, GB_VIZ_REG_C,GB_VIZ_REG_C,0,0,
            0,0,0,0, GB_VIZ_REG_D,GB_VIZ_REG_D,0,0,
            0,GB_VIZ_REG_HL,0,0, GB_VIZ_REG_E,GB_VIZ_REG_E,0,0,
            0,0,0,0, GB_VIZ_REG_H,GB_VIZ_REG_H,0,0,
            0,GB_VIZ_REG_HL,0,0, GB_VIZ_REG_L,GB_VIZ_REG_L,0,0,
            0,0,0,0, GB_VIZ_REG_MEM,GB_VIZ_REG_MEM,0,0,
            0,GB_VIZ_REG_SP,0,0, GB_VIZ_REG_A,GB_VIZ_REG_A,0,0,
            /* 0x40-0x47: LD B,r  → dst=B */
            GB_VIZ_REG_B,GB_VIZ_REG_B,GB_VIZ_REG_B,GB_VIZ_REG_B,
            GB_VIZ_REG_B,GB_VIZ_REG_B,GB_VIZ_REG_B,GB_VIZ_REG_B,
            /* 0x48-0x4F: LD C,r  → dst=C */
            GB_VIZ_REG_C,GB_VIZ_REG_C,GB_VIZ_REG_C,GB_VIZ_REG_C,
            GB_VIZ_REG_C,GB_VIZ_REG_C,GB_VIZ_REG_C,GB_VIZ_REG_C,
            /* 0x50-0x57: LD D,r  → dst=D */
            GB_VIZ_REG_D,GB_VIZ_REG_D,GB_VIZ_REG_D,GB_VIZ_REG_D,
            GB_VIZ_REG_D,GB_VIZ_REG_D,GB_VIZ_REG_D,GB_VIZ_REG_D,
            /* 0x58-0x5F: LD E,r  → dst=E */
            GB_VIZ_REG_E,GB_VIZ_REG_E,GB_VIZ_REG_E,GB_VIZ_REG_E,
            GB_VIZ_REG_E,GB_VIZ_REG_E,GB_VIZ_REG_E,GB_VIZ_REG_E,
            /* 0x60-0x67: LD H,r  → dst=H */
            GB_VIZ_REG_H,GB_VIZ_REG_H,GB_VIZ_REG_H,GB_VIZ_REG_H,
            GB_VIZ_REG_H,GB_VIZ_REG_H,GB_VIZ_REG_H,GB_VIZ_REG_H,
            /* 0x68-0x6F: LD L,r  → dst=L */
            GB_VIZ_REG_L,GB_VIZ_REG_L,GB_VIZ_REG_L,GB_VIZ_REG_L,
            GB_VIZ_REG_L,GB_VIZ_REG_L,GB_VIZ_REG_L,GB_VIZ_REG_L,
            /* 0x70-0x77: LD (HL),r → dst=MEM */
            GB_VIZ_REG_MEM,GB_VIZ_REG_MEM,GB_VIZ_REG_MEM,GB_VIZ_REG_MEM,
            GB_VIZ_REG_MEM,GB_VIZ_REG_MEM,GB_VIZ_REG_MEM,GB_VIZ_REG_MEM,
            /* 0x78-0x7F: LD A,r  → dst=A */
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            /* 0x80-0xBF: ALU A,r → dst=A (except CP which writes no reg) */
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,GB_VIZ_REG_A,
            0,0,GB_VIZ_REG_A,GB_VIZ_REG_A,
            /* 0xC0-0xFF */
            0,GB_VIZ_REG_BC,0,0,0,GB_VIZ_REG_MEM,GB_VIZ_REG_A,0,
            0,0,0,0,0,0,GB_VIZ_REG_A,0,
            0,GB_VIZ_REG_DE,0,0,0,GB_VIZ_REG_MEM,GB_VIZ_REG_A,0,
            0,0,0,0,0,0,GB_VIZ_REG_A,0,
            GB_VIZ_REG_MEM,GB_VIZ_REG_HL,GB_VIZ_REG_MEM,0,0,GB_VIZ_REG_MEM,GB_VIZ_REG_A,0,
            0,GB_VIZ_REG_HL,0,0,0,0,GB_VIZ_REG_A,0,
            GB_VIZ_REG_MEM,GB_VIZ_REG_NONE,GB_VIZ_REG_MEM,0,0,GB_VIZ_REG_MEM,GB_VIZ_REG_A,0,
            0,GB_VIZ_REG_SP,0,0,0,0,GB_VIZ_REG_A,0,
        };

        struct gb_cpu_viz *cv = &dbg->cpu_viz;
        uint16_t pc = gb->cpu.pc;
        uint8_t op  = gb_memory_peekb(gb, pc);

        /* ── Read-back from PREVIOUS instruction (already executed) ── */
        {
            uint8_t flags_now = (uint8_t)(
                (gb->cpu.f_z ? 8u : 0u) |
                (gb->cpu.f_n ? 4u : 0u) |
                (gb->cpu.f_h ? 2u : 0u) |
                (gb->cpu.f_c ? 1u : 0u));
            cv->alu_result   = gb->cpu.a;
            cv->flags_after  = flags_now;
            cv->flags_changed = cv->flags_before ^ flags_now;
            if (cv->timestamp_last > 0)
                cv->m_cycles = (uint8_t)((gb->timestamp - cv->timestamp_last) >> 2);
        }

        /* ── Capture state for CURRENT instruction ── */
        cv->stage         = GB_VIZ_STAGE_FETCH;
        cv->opcode        = op;
        cv->addr_bus      = pc;
        cv->activity_fade = 1.0f;
        cv->alu_a         = gb->cpu.a;
        cv->flags_before  = (uint8_t)(
            (gb->cpu.f_z ? 8u : 0u) |
            (gb->cpu.f_n ? 4u : 0u) |
            (gb->cpu.f_h ? 2u : 0u) |
            (gb->cpu.f_c ? 1u : 0u));
        cv->timestamp_last = gb->timestamp;
        gb_disasm(gb, pc, cv->mnemonic, sizeof(cv->mnemonic));

        /* Populate alu_op, src, dst */
        if (op == 0xCB)
        {
            uint8_t op2 = gb_memory_peekb(gb, (uint16_t)(pc + 1));
            cv->alu_op = (op2 < 0x40) ? GB_VIZ_ALU_SHIFT : GB_VIZ_ALU_BIT;
            /* CB src: lower 3 bits select register B/C/D/E/H/L/(HL)/A */
            static const uint8_t cb_reg[8] = {
                GB_VIZ_REG_B, GB_VIZ_REG_C, GB_VIZ_REG_D, GB_VIZ_REG_E,
                GB_VIZ_REG_H, GB_VIZ_REG_L, GB_VIZ_REG_MEM, GB_VIZ_REG_A
            };
            cv->src = cb_reg[op2 & 7];
            cv->dst = (op2 >= 0x80) ? cb_reg[op2 & 7] : GB_VIZ_REG_NONE;
        }
        else
        {
            cv->alu_op = alu_map[op];
            cv->src    = src_map[op];
            cv->dst    = dst_map[op];
        }

        /* Capture alu_b: the source register's current value */
        switch (cv->src)
        {
        case GB_VIZ_REG_A:   cv->alu_b = gb->cpu.a; break;
        case GB_VIZ_REG_B:   cv->alu_b = gb->cpu.b; break;
        case GB_VIZ_REG_C:   cv->alu_b = gb->cpu.c; break;
        case GB_VIZ_REG_D:   cv->alu_b = gb->cpu.d; break;
        case GB_VIZ_REG_E:   cv->alu_b = gb->cpu.e; break;
        case GB_VIZ_REG_H:   cv->alu_b = gb->cpu.h; break;
        case GB_VIZ_REG_L:   cv->alu_b = gb->cpu.l; break;
        default:             cv->alu_b = cv->data_bus; break; /* IMM/MEM: use last bus value */
        }
    }

    if (s_trace_file)
    {
        char text[64];
        gb_disasm(gb, gb->cpu.pc, text, sizeof(text));
        fprintf(s_trace_file,
                "%08llu PC=%04x SP=%04x AF=%02x%c%c%c%c BC=%02x%02x DE=%02x%02x HL=%02x%02x  %s\n",
                (unsigned long long)dbg->instruction_count,
                gb->cpu.pc, gb->cpu.sp, gb->cpu.a,
                gb->cpu.f_z ? 'Z' : '-',
                gb->cpu.f_n ? 'N' : '-',
                gb->cpu.f_h ? 'H' : '-',
                gb->cpu.f_c ? 'C' : '-',
                gb->cpu.b, gb->cpu.c, gb->cpu.d, gb->cpu.e,
                gb->cpu.h, gb->cpu.l, text);
    }

    /* ── Profiling — sempre ativo, independente do modo debug ── */
    {
        uint16_t pc = gb->cpu.pc;

        dbg->exec_coverage[pc >> 3] |= (uint8_t)(1u << (pc & 7));

        if (dbg->exec_heatmap[pc] < 0xFFFFFFFFu)
            dbg->exec_heatmap[pc]++;

        uint8_t op  = gb_memory_peekb(gb, pc);
        int     idx = op;
        if (op == 0xCB)
        {
            uint8_t op2 = gb_memory_peekb(gb, (uint16_t)(pc + 1));
            idx = 256 + op2;
        }
        if (dbg->opcode_hits[idx] < 0xFFFFFFFFu)
            dbg->opcode_hits[idx]++;
    }

    /* Breakpoints e estados de debug só funcionam com debug habilitado */
    if (!dbg->enabled) return;

    for (i = 0; i < dbg->n_breakpoints; i++)
    {
        if (dbg->bp_enabled[i] && gb->cpu.pc == dbg->breakpoints[i])
        {
            dbg->state = GB_DEBUG_PAUSED;
            return;
        }
    }
}

void gb_debug_reset_profiler(struct gb *gb)
{
    struct gb_debug *dbg = &gb->debug;
    memset(dbg->opcode_hits,   0, sizeof(dbg->opcode_hits));
    memset(dbg->exec_heatmap,  0, sizeof(dbg->exec_heatmap));
    memset(dbg->exec_coverage, 0, sizeof(dbg->exec_coverage));
}

void gb_debug_add_breakpoint(struct gb *gb, uint16_t addr)
{
    struct gb_debug *dbg = &gb->debug;
    unsigned i;

    /* Não duplica */
    for (i = 0; i < dbg->n_breakpoints; i++)
    {
        if (dbg->breakpoints[i] == addr)
        {
            dbg->bp_enabled[i] = true;
            return;
        }
    }

    if (dbg->n_breakpoints >= GB_DEBUG_MAX_BREAKPOINTS) return;

    dbg->breakpoints[dbg->n_breakpoints] = addr;
    dbg->bp_enabled [dbg->n_breakpoints] = true;
    dbg->n_breakpoints++;
}

void gb_debug_remove_breakpoint(struct gb *gb, unsigned index)
{
    struct gb_debug *dbg = &gb->debug;
    unsigned i;

    if (index >= dbg->n_breakpoints) return;

    for (i = index; i < dbg->n_breakpoints - 1; i++)
    {
        dbg->breakpoints[i] = dbg->breakpoints[i + 1];
        dbg->bp_enabled [i] = dbg->bp_enabled [i + 1];
    }
    dbg->n_breakpoints--;
}

bool gb_debug_has_breakpoint(struct gb *gb, uint16_t addr)
{
    struct gb_debug *dbg = &gb->debug;
    unsigned i;

    for (i = 0; i < dbg->n_breakpoints; i++)
    {
        if (dbg->bp_enabled[i] && dbg->breakpoints[i] == addr)
            return true;
    }
    return false;
}

/* ────────────────────────────────────────────────────────────────── */
/* Watchpoints                                                        */
/* ────────────────────────────────────────────────────────────────── */

void gb_debug_add_watchpoint(struct gb *gb, uint16_t addr, gb_watchpoint_type type)
{
    struct gb_debug *dbg = &gb->debug;
    unsigned i;

    /* Não duplica o mesmo endereço */
    for (i = 0; i < dbg->n_watchpoints; i++)
    {
        if (dbg->watchpoints[i].addr == addr)
        {
            dbg->watchpoints[i].type = type;
            dbg->watchpoints[i].enabled = true;
            return;
        }
    }

    if (dbg->n_watchpoints >= GB_DEBUG_MAX_WATCHPOINTS) return;

    dbg->watchpoints[dbg->n_watchpoints].addr = addr;
    dbg->watchpoints[dbg->n_watchpoints].type = type;
    dbg->watchpoints[dbg->n_watchpoints].enabled = true;
    dbg->n_watchpoints++;
}

void gb_debug_remove_watchpoint(struct gb *gb, unsigned index)
{
    struct gb_debug *dbg = &gb->debug;
    unsigned i;

    if (index >= dbg->n_watchpoints) return;

    for (i = index; i < dbg->n_watchpoints - 1; i++)
    {
        dbg->watchpoints[i] = dbg->watchpoints[i + 1];
    }
    dbg->n_watchpoints--;
}

void gb_debug_check_watchpoint(struct gb *gb, uint16_t addr, gb_watchpoint_type type)
{
    struct gb_debug *dbg = &gb->debug;
    unsigned i;

    if (!dbg->enabled || dbg->state != GB_DEBUG_RUNNING) return;

    for (i = 0; i < dbg->n_watchpoints; i++)
    {
        struct gb_watchpoint *wp = &dbg->watchpoints[i];
        if (wp->enabled && wp->addr == addr && (wp->type & type))
        {
            dbg->state = GB_DEBUG_PAUSED;
            return;
        }
    }
}

/* ────────────────────────────────────────────────────────────────── */
/* Step over / step out                                               */
/* ────────────────────────────────────────────────────────────────── */

uint16_t gb_debug_get_next_instr_addr(struct gb *gb)
{
    uint16_t pc = gb->cpu.pc;
    return (uint16_t)(pc + gb_disasm_len(gb, pc));
}

void gb_debug_step_over(struct gb *gb)
{
    struct gb_debug *dbg = &gb->debug;
    uint8_t op = gb_memory_peekb(gb, gb->cpu.pc);

    /* Se é um CALL (CD = CALL u16, C4 = CALL NZ, ..., etc), seta um breakpoint
     * no endereço após a instrução CALL */
    if (op == 0xCD || op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC)
    {
        dbg->step_return_addr = gb_debug_get_next_instr_addr(gb);
        dbg->state = GB_DEBUG_RUNNING;
        gb_debug_add_breakpoint(gb, dbg->step_return_addr);
    }
    else
    {
        /* Não é um CALL, comporta-se como step normal */
        dbg->state = GB_DEBUG_STEPPING;
    }
}

void gb_debug_step_out(struct gb *gb)
{
    struct gb_debug *dbg = &gb->debug;

    /* Temos que guardar a profundidade de calls até agora.
     * Por simplicidade, aqui apenas continuamos até encontrar um RET.
     * Em um debugger real, rastreariamos a pilha de calls. */
    dbg->step_depth = 0;
    dbg->state = GB_DEBUG_RUNNING;
    /* Continuará rodando até encontrar um RET na mesma profundidade (depth 0) */
}

/* ── Hardware trace helpers ── */

static void hw_trace_push(struct gb *gb, gb_hw_trace_event *ev)
{
    struct gb_hw_trace *t = &gb->debug.hw_trace;
    ev->seq = t->next_seq++;
    t->events[t->head] = *ev;
    t->head = (t->head + 1) & (GB_HW_TRACE_CAP - 1);
    if (t->count < GB_HW_TRACE_CAP)
        t->count++;
}

void gb_debug_hw_trace_cpu_fetch(struct gb *gb, uint16_t pc, uint8_t opcode)
{
    if (!gb->debug.hw_trace.enabled)
        return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_CPU_FETCH;
    ev.pc        = pc;
    ev.opcode    = opcode;
    ev.addr      = pc;
    ev.data      = opcode;
    ev.write     = false;
    /* CPU register snapshot for die viewer seed */
    const struct gb_cpu_viz *cv = &gb->debug.cpu_viz;
    ev.snap_a       = gb->cpu.a;
    ev.snap_b       = gb->cpu.b;
    ev.snap_c       = gb->cpu.c;
    ev.snap_d       = gb->cpu.d;
    ev.snap_e       = gb->cpu.e;
    ev.snap_h       = gb->cpu.h;
    ev.snap_l       = gb->cpu.l;
    ev.snap_sp      = gb->cpu.sp;
    ev.snap_flags   = (uint8_t)((gb->cpu.f_z ? 8u : 0u) | (gb->cpu.f_n ? 4u : 0u) |
                                (gb->cpu.f_h ? 2u : 0u) | (gb->cpu.f_c ? 1u : 0u));
    ev.snap_ir      = opcode;
    ev.snap_dbus    = cv->data_bus;
    ev.snap_alu_op  = cv->alu_op;
    ev.snap_src     = cv->src;
    ev.snap_dst     = cv->dst;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_cpu_read(struct gb *gb, uint16_t addr, uint8_t data)
{
    if (!gb->debug.hw_trace.enabled)
        return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_CPU_READ;
    ev.pc        = gb->cpu.pc;
    ev.addr      = addr;
    ev.data      = data;
    ev.write     = false;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_cpu_write(struct gb *gb, uint16_t addr, uint8_t data)
{
    if (!gb->debug.hw_trace.enabled)
        return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_CPU_WRITE;
    ev.pc        = gb->cpu.pc;
    ev.addr      = addr;
    ev.data      = data;
    ev.write     = true;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_irq(struct gb *gb, bool ack, uint8_t if_reg, uint8_t ie_reg)
{
    if (!gb->debug.hw_trace.enabled)
        return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = ack ? GB_HW_EVT_IRQ_ACK : GB_HW_EVT_IRQ_REQUEST;
    ev.pc        = gb->cpu.pc;
    ev.data      = if_reg & ie_reg;
    ev.extra     = ie_reg;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_alu(struct gb *gb, uint8_t alu_op, uint8_t result,
                           uint8_t flags_before, uint8_t flags_after)
{
    if (!gb->debug.hw_trace.enabled)
        return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_CPU_ALU;
    ev.pc        = gb->cpu.pc;
    ev.data      = result;
    ev.extra     = alu_op;
    ev.addr      = (uint16_t)(((uint16_t)flags_after << 8) | flags_before);
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_writeback(struct gb *gb, uint8_t dst_reg, uint8_t val8, uint16_t val16)
{
    if (!gb->debug.hw_trace.enabled)
        return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_CPU_WRITEBACK;
    ev.pc        = gb->cpu.pc;
    ev.data      = val8;
    ev.extra     = dst_reg;
    ev.addr      = val16;
    hw_trace_push(gb, &ev);
}

/* ── Fase E helpers ── */

void gb_debug_hw_trace_ppu_vblank(struct gb *gb, uint8_t ly)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_PPU_VBLANK;
    ev.data      = ly;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_ppu_hblank(struct gb *gb, uint8_t ly)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_PPU_HBLANK;
    ev.data      = ly;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_oam_dma(struct gb *gb, uint8_t pos, uint8_t data)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_OAM_DMA;
    ev.addr      = (uint16_t)(0xFE00 + pos);
    ev.data      = data;
    ev.extra     = pos;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_timer_ovf(struct gb *gb, uint8_t tma)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_TIMER_OVF;
    ev.data      = tma;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_apu_write(struct gb *gb, uint16_t addr, uint8_t data)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_APU_WRITE;
    ev.addr      = addr;
    ev.data      = data;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_joypad(struct gb *gb, uint8_t state, bool pressed)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_JOYPAD;
    ev.data      = state;
    ev.write     = pressed;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_serial_start(struct gb *gb, uint8_t sc)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_SERIAL_START;
    ev.extra     = sc;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_serial_done(struct gb *gb, uint8_t sb_received)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_SERIAL_DONE;
    ev.data      = sb_received;
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_mbc_switch(struct gb *gb, uint16_t write_addr,
                                   uint16_t new_rom_bank, uint8_t new_ram_bank)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_MBC_SWITCH;
    ev.addr      = write_addr;
    ev.data      = (uint8_t)(new_rom_bank & 0xff);
    ev.extra     = (uint8_t)((new_rom_bank >> 8) & 0x01) | (uint8_t)(new_ram_bank << 1);
    hw_trace_push(gb, &ev);
}

void gb_debug_hw_trace_ppu_mode(struct gb *gb, uint8_t new_mode, uint8_t ly)
{
    if (!gb->debug.hw_trace.enabled) return;
    gb_hw_trace_event ev = {0};
    ev.timestamp = gb->timestamp;
    ev.type      = GB_HW_EVT_PPU_MODE;
    ev.data      = new_mode;
    ev.extra     = ly;
    hw_trace_push(gb, &ev);
}
