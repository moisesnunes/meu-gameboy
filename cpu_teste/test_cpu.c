/*
 * test_cpu.c
 *
 * Unit tests for the Game Boy CPU (cpu.c).
 *
 * Strategy
 * --------
 *  - gb_mock_init() resets the full GB state + flat 64 KB memory.
 *  - gb_mock_write() places opcode bytes at the current PC.
 *  - step() calls gb_cpu_run_cycles(gb, 4), which executes exactly one
 *    instruction regardless of how many cycles it consumes (because the
 *    timestamp starts at 0 after gb_sync_rebase and is always >= 4 after
 *    any single instruction fetch+execute).
 *  - Assertions check registers, flags, PC, and SP after each instruction.
 *
 * Build
 * -----
 *  gcc -std=c11 -Wall -Wextra -g -I. \
 *      test_cpu.c gb_mock.c cpu.c -o test_cpu
 *
 * Run
 * ---
 *  ./test_cpu
 */

#include "gb.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

/* Declarations of test helpers that live in gb_mock.c */
extern void    gb_mock_init (struct gb *gb);
extern void    gb_mock_write(struct gb *gb, uint16_t addr,
                             const uint8_t *bytes, int len);
extern uint8_t gb_mock_peek (uint16_t addr);

/* ================================================================== */
/* Tiny test framework                                                   */
/* ================================================================== */

static int g_pass       = 0;
static int g_fail       = 0;
static const char *g_tc = "(none)";  /* current test-case name */

#define TC(name)  (g_tc = (name))

#define ASSERT_EQ(got, expected)                                          \
    do {                                                                  \
        unsigned long long _g = (unsigned long long)(got);               \
        unsigned long long _e = (unsigned long long)(expected);          \
        if (_g != _e) {                                                   \
            printf("  FAIL  %-42s  got=0x%llx  want=0x%llx  [%s:%d]\n", \
                   g_tc, _g, _e, __FILE__, __LINE__);                    \
            g_fail++;                                                     \
        } else {                                                          \
            g_pass++;                                                     \
        }                                                                 \
    } while (0)

#define ASSERT_TRUE(cond)                                                 \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("  FAIL  %-42s  assertion false: %s  [%s:%d]\n",      \
                   g_tc, #cond, __FILE__, __LINE__);                      \
            g_fail++;                                                     \
        } else {                                                          \
            g_pass++;                                                     \
        }                                                                 \
    } while (0)

/* ================================================================== */
/* Helpers                                                               */
/* ================================================================== */

static struct gb gb;

/* Execute exactly one instruction at the current PC. */
static void step(void)
{
    gb_cpu_run_cycles(&gb, 4);
}

/* Write bytes at the current PC, then step. */
static void run(const uint8_t *prog, int len)
{
    gb_mock_write(&gb, gb.cpu.pc, prog, len);
    step();
}

/* ================================================================== */
/* NOP                                                                   */
/* ================================================================== */

static void test_nop(void)
{
    printf("--- NOP ---\n");
    gb_mock_init(&gb);
    uint16_t pc0 = gb.cpu.pc;
    uint8_t prog[] = { 0x00 };
    run(prog, 1);

    TC("NOP advances PC by 1");
    ASSERT_EQ(gb.cpu.pc, pc0 + 1);
}

/* ================================================================== */
/* INC / DEC 8-bit registers                                            */
/* ================================================================== */

static void test_inc_dec_8(void)
{
    printf("--- INC/DEC 8-bit ---\n");
    uint8_t prog[1];

    /* --- INC B (0x04) --- */

    /* Normal increment */
    gb_mock_init(&gb);
    gb.cpu.b = 0x05;
    prog[0] = 0x04;
    run(prog, 1);
    TC("INC B normal: value");   ASSERT_EQ(gb.cpu.b,   0x06);
    TC("INC B normal: Z=0");     ASSERT_EQ(gb.cpu.f_z, false);
    TC("INC B normal: N=0");     ASSERT_EQ(gb.cpu.f_n, false);
    TC("INC B normal: H=0");     ASSERT_EQ(gb.cpu.f_h, false);

    /* Half-carry: 0x0F -> 0x10 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x0F;
    prog[0] = 0x04;
    run(prog, 1);
    TC("INC B 0x0F: value");   ASSERT_EQ(gb.cpu.b,   0x10);
    TC("INC B 0x0F: H=1");     ASSERT_EQ(gb.cpu.f_h, true);
    TC("INC B 0x0F: Z=0");     ASSERT_EQ(gb.cpu.f_z, false);

    /* Overflow: 0xFF -> 0x00, Z=1, carry unchanged */
    gb_mock_init(&gb);
    gb.cpu.b   = 0xFF;
    gb.cpu.f_c = true;   /* must be preserved */
    prog[0]    = 0x04;
    run(prog, 1);
    TC("INC B 0xFF: wraps to 0");  ASSERT_EQ(gb.cpu.b,   0x00);
    TC("INC B 0xFF: Z=1");         ASSERT_EQ(gb.cpu.f_z, true);
    TC("INC B 0xFF: H=1");         ASSERT_EQ(gb.cpu.f_h, true);
    TC("INC B 0xFF: C unchanged"); ASSERT_EQ(gb.cpu.f_c, true);

    /* INC A (0x3C) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x42;
    prog[0] = 0x3C;
    run(prog, 1);
    TC("INC A: value"); ASSERT_EQ(gb.cpu.a, 0x43);

    /* INC L (0x2C) */
    gb_mock_init(&gb);
    gb.cpu.l = 0x7F;
    prog[0] = 0x2C;
    run(prog, 1);
    TC("INC L: value"); ASSERT_EQ(gb.cpu.l, 0x80);
    TC("INC L: H=1");   ASSERT_EQ(gb.cpu.f_h, true);

    /* --- DEC B (0x05) --- */

    /* Normal decrement */
    gb_mock_init(&gb);
    gb.cpu.b = 0x05;
    prog[0] = 0x05;
    run(prog, 1);
    TC("DEC B normal: value");  ASSERT_EQ(gb.cpu.b,   0x04);
    TC("DEC B normal: N=1");    ASSERT_EQ(gb.cpu.f_n, true);
    TC("DEC B normal: Z=0");    ASSERT_EQ(gb.cpu.f_z, false);
    TC("DEC B normal: H=0");    ASSERT_EQ(gb.cpu.f_h, false);

    /* Zero result: 0x01 -> 0x00 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x01;
    prog[0] = 0x05;
    run(prog, 1);
    TC("DEC B 0x01: value"); ASSERT_EQ(gb.cpu.b,   0x00);
    TC("DEC B 0x01: Z=1");   ASSERT_EQ(gb.cpu.f_z, true);

    /* Half-borrow: 0x10 -> 0x0F */
    gb_mock_init(&gb);
    gb.cpu.b = 0x10;
    prog[0] = 0x05;
    run(prog, 1);
    TC("DEC B 0x10: value"); ASSERT_EQ(gb.cpu.b,   0x0F);
    TC("DEC B 0x10: H=1");   ASSERT_EQ(gb.cpu.f_h, true);

    /* Underflow: 0x00 -> 0xFF */
    gb_mock_init(&gb);
    gb.cpu.b = 0x00;
    prog[0] = 0x05;
    run(prog, 1);
    TC("DEC B 0x00: wraps to 0xFF"); ASSERT_EQ(gb.cpu.b,   0xFF);
    TC("DEC B 0x00: Z=0");           ASSERT_EQ(gb.cpu.f_z, false);
    TC("DEC B 0x00: H=1");           ASSERT_EQ(gb.cpu.f_h, true);

    /* DEC A (0x3D) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10;
    prog[0] = 0x3D;
    run(prog, 1);
    TC("DEC A: value"); ASSERT_EQ(gb.cpu.a, 0x0F);
    TC("DEC A: H=1");   ASSERT_EQ(gb.cpu.f_h, true);
}

/* ================================================================== */
/* ADD / ADC                                                             */
/* ================================================================== */

static void test_add_adc(void)
{
    printf("--- ADD / ADC ---\n");
    uint8_t prog[2];

    /* --- ADD A,B (0x80) --- */

    gb_mock_init(&gb);
    gb.cpu.a = 0x10; gb.cpu.b = 0x05;
    prog[0] = 0x80;
    run(prog, 1);
    TC("ADD A,B: result");  ASSERT_EQ(gb.cpu.a,   0x15);
    TC("ADD A,B: N=0");     ASSERT_EQ(gb.cpu.f_n, false);
    TC("ADD A,B: Z=0");     ASSERT_EQ(gb.cpu.f_z, false);
    TC("ADD A,B: H=0");     ASSERT_EQ(gb.cpu.f_h, false);
    TC("ADD A,B: C=0");     ASSERT_EQ(gb.cpu.f_c, false);

    /* Carry out */
    gb_mock_init(&gb);
    gb.cpu.a = 0xF0; gb.cpu.b = 0x20;
    prog[0] = 0x80;
    run(prog, 1);
    TC("ADD A,B carry: result"); ASSERT_EQ(gb.cpu.a,   0x10);
    TC("ADD A,B carry: C=1");    ASSERT_EQ(gb.cpu.f_c, true);
    TC("ADD A,B carry: Z=0");    ASSERT_EQ(gb.cpu.f_z, false);

    /* Half-carry */
    gb_mock_init(&gb);
    gb.cpu.a = 0x08; gb.cpu.b = 0x08;
    prog[0] = 0x80;
    run(prog, 1);
    TC("ADD A,B half-carry: result"); ASSERT_EQ(gb.cpu.a,   0x10);
    TC("ADD A,B half-carry: H=1");    ASSERT_EQ(gb.cpu.f_h, true);

    /* Result zero */
    gb_mock_init(&gb);
    gb.cpu.a = 0x80; gb.cpu.b = 0x80;
    prog[0] = 0x80;
    run(prog, 1);
    TC("ADD A,B zero: result"); ASSERT_EQ(gb.cpu.a,   0x00);
    TC("ADD A,B zero: Z=1");    ASSERT_EQ(gb.cpu.f_z, true);
    TC("ADD A,B zero: C=1");    ASSERT_EQ(gb.cpu.f_c, true);

    /* ADD A,A (0x87) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x21;
    prog[0] = 0x87;
    run(prog, 1);
    TC("ADD A,A: result"); ASSERT_EQ(gb.cpu.a, 0x42);

    /* ADD A,i8 (0xC6) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10;
    prog[0] = 0xC6; prog[1] = 0x05;
    run(prog, 2);
    TC("ADD A,i8: result"); ASSERT_EQ(gb.cpu.a, 0x15);

    /* --- ADC A,B (0x88) without carry --- */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10; gb.cpu.b = 0x05; gb.cpu.f_c = false;
    prog[0] = 0x88;
    run(prog, 1);
    TC("ADC A,B no-carry: result"); ASSERT_EQ(gb.cpu.a, 0x15);

    /* ADC A,B with carry = adds +1 extra */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10; gb.cpu.b = 0x05; gb.cpu.f_c = true;
    prog[0] = 0x88;
    run(prog, 1);
    TC("ADC A,B carry: result"); ASSERT_EQ(gb.cpu.a, 0x16);

    /* ADC A,i8 (0xCE): 0xFF + 0 + C=1 = 0x00, Z=1, C=1 */
    gb_mock_init(&gb);
    gb.cpu.a = 0xFF; gb.cpu.f_c = true;
    prog[0] = 0xCE; prog[1] = 0x00;
    run(prog, 2);
    TC("ADC A,i8 overflow: result"); ASSERT_EQ(gb.cpu.a,   0x00);
    TC("ADC A,i8 overflow: Z=1");    ASSERT_EQ(gb.cpu.f_z, true);
    TC("ADC A,i8 overflow: C=1");    ASSERT_EQ(gb.cpu.f_c, true);
}

/* ================================================================== */
/* SUB / SBC                                                             */
/* ================================================================== */

static void test_sub_sbc(void)
{
    printf("--- SUB / SBC ---\n");
    uint8_t prog[2];

    /* SUB A,B (0x90) normal */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10; gb.cpu.b = 0x05;
    prog[0] = 0x90;
    run(prog, 1);
    TC("SUB A,B: result"); ASSERT_EQ(gb.cpu.a,   0x0B);
    TC("SUB A,B: N=1");    ASSERT_EQ(gb.cpu.f_n, true);
    TC("SUB A,B: Z=0");    ASSERT_EQ(gb.cpu.f_z, false);
    TC("SUB A,B: C=0");    ASSERT_EQ(gb.cpu.f_c, false);

    /* SUB A,A (0x97): always produces zero */
    gb_mock_init(&gb);
    gb.cpu.a = 0x42;
    prog[0] = 0x97;
    run(prog, 1);
    TC("SUB A,A: result 0"); ASSERT_EQ(gb.cpu.a,   0x00);
    TC("SUB A,A: Z=1");      ASSERT_EQ(gb.cpu.f_z, true);
    TC("SUB A,A: C=0");      ASSERT_EQ(gb.cpu.f_c, false);

    /* SUB with borrow (carry) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x00; gb.cpu.b = 0x01;
    prog[0] = 0x90;
    run(prog, 1);
    TC("SUB A,B borrow: result"); ASSERT_EQ(gb.cpu.a,   0xFF);
    TC("SUB A,B borrow: C=1");    ASSERT_EQ(gb.cpu.f_c, true);

    /* SUB with half-borrow */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10; gb.cpu.b = 0x01;
    prog[0] = 0x90;
    run(prog, 1);
    TC("SUB A,B half-borrow: result"); ASSERT_EQ(gb.cpu.a,   0x0F);
    TC("SUB A,B half-borrow: H=1");    ASSERT_EQ(gb.cpu.f_h, true);

    /* SUB A,i8 (0xD6) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x20;
    prog[0] = 0xD6; prog[1] = 0x10;
    run(prog, 2);
    TC("SUB A,i8: result"); ASSERT_EQ(gb.cpu.a, 0x10);

    /* SBC A,B (0x98) without carry */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10; gb.cpu.b = 0x05; gb.cpu.f_c = false;
    prog[0] = 0x98;
    run(prog, 1);
    TC("SBC A,B no-carry: result"); ASSERT_EQ(gb.cpu.a, 0x0B);

    /* SBC A,B with carry: A - B - 1 */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10; gb.cpu.b = 0x05; gb.cpu.f_c = true;
    prog[0] = 0x98;
    run(prog, 1);
    TC("SBC A,B carry: result"); ASSERT_EQ(gb.cpu.a, 0x0A);

    /* SBC A,i8 (0xDE) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10; gb.cpu.f_c = false;
    prog[0] = 0xDE; prog[1] = 0x05;
    run(prog, 2);
    TC("SBC A,i8: result"); ASSERT_EQ(gb.cpu.a, 0x0B);
}

/* ================================================================== */
/* AND / OR / XOR / CP                                                   */
/* ================================================================== */

static void test_logic(void)
{
    printf("--- AND / OR / XOR / CP ---\n");
    uint8_t prog[2];

    /* AND A,B (0xA0) */
    gb_mock_init(&gb);
    gb.cpu.a = 0xFF; gb.cpu.b = 0x0F;
    prog[0] = 0xA0;
    run(prog, 1);
    TC("AND A,B: result"); ASSERT_EQ(gb.cpu.a,   0x0F);
    TC("AND A,B: H=1");    ASSERT_EQ(gb.cpu.f_h, true);
    TC("AND A,B: N=0");    ASSERT_EQ(gb.cpu.f_n, false);
    TC("AND A,B: C=0");    ASSERT_EQ(gb.cpu.f_c, false);
    TC("AND A,B: Z=0");    ASSERT_EQ(gb.cpu.f_z, false);

    /* AND A,i8: result zero (0xF0 & 0x0F) */
    gb_mock_init(&gb);
    gb.cpu.a = 0xF0;
    prog[0] = 0xE6; prog[1] = 0x0F;
    run(prog, 2);
    TC("AND A,i8 zero: result"); ASSERT_EQ(gb.cpu.a,   0x00);
    TC("AND A,i8 zero: Z=1");    ASSERT_EQ(gb.cpu.f_z, true);
    TC("AND A,i8 zero: H=1");    ASSERT_EQ(gb.cpu.f_h, true);

    /* OR A,B (0xB0) */
    gb_mock_init(&gb);
    gb.cpu.a = 0xF0; gb.cpu.b = 0x0F;
    prog[0] = 0xB0;
    run(prog, 1);
    TC("OR A,B: result"); ASSERT_EQ(gb.cpu.a,   0xFF);
    TC("OR A,B: Z=0");    ASSERT_EQ(gb.cpu.f_z, false);
    TC("OR A,B: H=0");    ASSERT_EQ(gb.cpu.f_h, false);
    TC("OR A,B: N=0");    ASSERT_EQ(gb.cpu.f_n, false);
    TC("OR A,B: C=0");    ASSERT_EQ(gb.cpu.f_c, false);

    /* OR A,i8: result zero */
    gb_mock_init(&gb);
    gb.cpu.a = 0x00;
    prog[0] = 0xF6; prog[1] = 0x00;
    run(prog, 2);
    TC("OR A,0: Z=1"); ASSERT_EQ(gb.cpu.f_z, true);

    /* XOR A,B (0xA8) */
    gb_mock_init(&gb);
    gb.cpu.a = 0xFF; gb.cpu.b = 0xF0;
    prog[0] = 0xA8;
    run(prog, 1);
    TC("XOR A,B: result"); ASSERT_EQ(gb.cpu.a, 0x0F);

    /* XOR A,A (0xAF): always produces zero */
    gb_mock_init(&gb);
    gb.cpu.a = 0xAB;
    prog[0] = 0xAF;
    run(prog, 1);
    TC("XOR A,A: result"); ASSERT_EQ(gb.cpu.a,   0x00);
    TC("XOR A,A: Z=1");    ASSERT_EQ(gb.cpu.f_z, true);
    TC("XOR A,A: H=0");    ASSERT_EQ(gb.cpu.f_h, false);
    TC("XOR A,A: N=0");    ASSERT_EQ(gb.cpu.f_n, false);
    TC("XOR A,A: C=0");    ASSERT_EQ(gb.cpu.f_c, false);

    /* XOR A,i8 (0xEE) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x55;
    prog[0] = 0xEE; prog[1] = 0xAA;
    run(prog, 2);
    TC("XOR A,i8: result"); ASSERT_EQ(gb.cpu.a, 0xFF);

    /* CP A,B (0xB8): flags like SUB but A unchanged */
    gb_mock_init(&gb);
    gb.cpu.a = 0x42; gb.cpu.b = 0x42;
    prog[0] = 0xB8;
    run(prog, 1);
    TC("CP A,B equal: A unchanged"); ASSERT_EQ(gb.cpu.a,   0x42);
    TC("CP A,B equal: Z=1");         ASSERT_EQ(gb.cpu.f_z, true);
    TC("CP A,B equal: N=1");         ASSERT_EQ(gb.cpu.f_n, true);
    TC("CP A,B equal: C=0");         ASSERT_EQ(gb.cpu.f_c, false);

    /* CP A,i8 (0xFE): A < imm -> borrow */
    gb_mock_init(&gb);
    gb.cpu.a = 0x10;
    prog[0] = 0xFE; prog[1] = 0x20;
    run(prog, 2);
    TC("CP A<imm: A unchanged"); ASSERT_EQ(gb.cpu.a,   0x10);
    TC("CP A<imm: C=1");         ASSERT_EQ(gb.cpu.f_c, true);
    TC("CP A<imm: Z=0");         ASSERT_EQ(gb.cpu.f_z, false);
}

/* ================================================================== */
/* Load instructions                                                     */
/* ================================================================== */

static void test_loads(void)
{
    printf("--- Loads ---\n");
    uint8_t prog[3];

    /* LD B,i8 (0x06) */
    gb_mock_init(&gb);
    prog[0] = 0x06; prog[1] = 0xAB;
    run(prog, 2);
    TC("LD B,i8"); ASSERT_EQ(gb.cpu.b, 0xAB);

    /* LD A,i8 (0x3E) */
    gb_mock_init(&gb);
    prog[0] = 0x3E; prog[1] = 0x55;
    run(prog, 2);
    TC("LD A,i8"); ASSERT_EQ(gb.cpu.a, 0x55);

    /* LD C,i8 (0x0E) */
    gb_mock_init(&gb);
    prog[0] = 0x0E; prog[1] = 0x12;
    run(prog, 2);
    TC("LD C,i8"); ASSERT_EQ(gb.cpu.c, 0x12);

    /* LD H,i8 (0x26) */
    gb_mock_init(&gb);
    prog[0] = 0x26; prog[1] = 0xBE;
    run(prog, 2);
    TC("LD H,i8"); ASSERT_EQ(gb.cpu.h, 0xBE);

    /* LD B,C (0x41): register transfer */
    gb_mock_init(&gb);
    gb.cpu.c = 0x77;
    prog[0] = 0x41;
    run(prog, 1);
    TC("LD B,C"); ASSERT_EQ(gb.cpu.b, 0x77);

    /* LD D,A (0x57) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x33;
    prog[0] = 0x57;
    run(prog, 1);
    TC("LD D,A"); ASSERT_EQ(gb.cpu.d, 0x33);

    /* LD A,E (0x7B) */
    gb_mock_init(&gb);
    gb.cpu.e = 0x99;
    prog[0] = 0x7B;
    run(prog, 1);
    TC("LD A,E"); ASSERT_EQ(gb.cpu.a, 0x99);

    /* LD H,L (0x65) */
    gb_mock_init(&gb);
    gb.cpu.l = 0xAA;
    prog[0] = 0x65;
    run(prog, 1);
    TC("LD H,L"); ASSERT_EQ(gb.cpu.h, 0xAA);

    /* LD BC,i16 (0x01) — little-endian immediate */
    gb_mock_init(&gb);
    prog[0] = 0x01; prog[1] = 0x34; prog[2] = 0x12; /* LD BC, 0x1234 */
    run(prog, 3);
    TC("LD BC,i16: B"); ASSERT_EQ(gb.cpu.b, 0x12);
    TC("LD BC,i16: C"); ASSERT_EQ(gb.cpu.c, 0x34);

    /* LD HL,i16 (0x21) */
    gb_mock_init(&gb);
    prog[0] = 0x21; prog[1] = 0xCD; prog[2] = 0xAB; /* LD HL, 0xABCD */
    run(prog, 3);
    TC("LD HL,i16: H"); ASSERT_EQ(gb.cpu.h, 0xAB);
    TC("LD HL,i16: L"); ASSERT_EQ(gb.cpu.l, 0xCD);

    /* LD SP,i16 (0x31) */
    gb_mock_init(&gb);
    prog[0] = 0x31; prog[1] = 0x00; prog[2] = 0x20; /* LD SP, 0x2000 */
    run(prog, 3);
    TC("LD SP,i16"); ASSERT_EQ(gb.cpu.sp, 0x2000);

    /* LD (HL),A (0x77) then LD A,(HL) (0x7E) */
    gb_mock_init(&gb);
    gb.cpu.a = 0xBE;
    gb.cpu.h = 0xC0; gb.cpu.l = 0x00; /* HL = 0xC000 */
    prog[0] = 0x77; /* LD (HL),A */
    run(prog, 1);
    gb.cpu.a = 0x00; /* clear A to prove the round-trip */
    prog[0] = 0x7E;  /* LD A,(HL) */
    run(prog, 1);
    TC("LD (HL),A / LD A,(HL) round-trip"); ASSERT_EQ(gb.cpu.a, 0xBE);

    /* LD (HL),i8 (0x36) */
    gb_mock_init(&gb);
    gb.cpu.h = 0xC1; gb.cpu.l = 0x00;
    prog[0] = 0x36; prog[1] = 0xDE;
    run(prog, 2);
    TC("LD (HL),i8: memory written"); ASSERT_EQ(gb_mock_peek(0xC100), 0xDE);
}

/* ================================================================== */
/* 16-bit arithmetic                                                     */
/* ================================================================== */

static void test_16bit(void)
{
    printf("--- 16-bit arithmetic ---\n");
    uint8_t prog[1];

    /* INC BC (0x03): low byte carry to high */
    gb_mock_init(&gb);
    gb.cpu.b = 0x00; gb.cpu.c = 0xFF;
    prog[0] = 0x03;
    run(prog, 1);
    TC("INC BC propagate: B"); ASSERT_EQ(gb.cpu.b, 0x01);
    TC("INC BC propagate: C"); ASSERT_EQ(gb.cpu.c, 0x00);

    /* INC BC: wraps at 0xFFFF */
    gb_mock_init(&gb);
    gb.cpu.b = 0xFF; gb.cpu.c = 0xFF;
    prog[0] = 0x03;
    run(prog, 1);
    TC("INC BC wrap: B"); ASSERT_EQ(gb.cpu.b, 0x00);
    TC("INC BC wrap: C"); ASSERT_EQ(gb.cpu.c, 0x00);

    /* DEC BC (0x0B) */
    gb_mock_init(&gb);
    gb.cpu.b = 0x01; gb.cpu.c = 0x00;
    prog[0] = 0x0B;
    run(prog, 1);
    TC("DEC BC borrow: B"); ASSERT_EQ(gb.cpu.b, 0x00);
    TC("DEC BC borrow: C"); ASSERT_EQ(gb.cpu.c, 0xFF);

    /* INC HL (0x23) */
    gb_mock_init(&gb);
    gb.cpu.h = 0x12; gb.cpu.l = 0xFF;
    prog[0] = 0x23;
    run(prog, 1);
    TC("INC HL: H"); ASSERT_EQ(gb.cpu.h, 0x13);
    TC("INC HL: L"); ASSERT_EQ(gb.cpu.l, 0x00);

    /* DEC DE (0x1B) */
    gb_mock_init(&gb);
    gb.cpu.d = 0x00; gb.cpu.e = 0x00;
    prog[0] = 0x1B;
    run(prog, 1);
    TC("DEC DE wrap: D"); ASSERT_EQ(gb.cpu.d, 0xFF);
    TC("DEC DE wrap: E"); ASSERT_EQ(gb.cpu.e, 0xFF);

    /* INC SP (0x33) / DEC SP (0x3B) */
    gb_mock_init(&gb);
    gb.cpu.sp = 0x1000;
    prog[0] = 0x33;
    run(prog, 1);
    TC("INC SP"); ASSERT_EQ(gb.cpu.sp, 0x1001);

    gb_mock_init(&gb);
    gb.cpu.sp = 0x1000;
    prog[0] = 0x3B;
    run(prog, 1);
    TC("DEC SP"); ASSERT_EQ(gb.cpu.sp, 0x0FFF);

    /* ADD HL,BC (0x09): no overflow */
    gb_mock_init(&gb);
    gb.cpu.h = 0x10; gb.cpu.l = 0x00;
    gb.cpu.b = 0x00; gb.cpu.c = 0x50;
    prog[0] = 0x09;
    run(prog, 1);
    TC("ADD HL,BC: H");  ASSERT_EQ(gb.cpu.h,   0x10);
    TC("ADD HL,BC: L");  ASSERT_EQ(gb.cpu.l,   0x50);
    TC("ADD HL,BC: C=0"); ASSERT_EQ(gb.cpu.f_c, false);
    TC("ADD HL,BC: N=0"); ASSERT_EQ(gb.cpu.f_n, false);

    /* ADD HL,BC: carry (0xFFFF + 1) */
    gb_mock_init(&gb);
    gb.cpu.h = 0xFF; gb.cpu.l = 0xFF;
    gb.cpu.b = 0x00; gb.cpu.c = 0x01;
    prog[0] = 0x09;
    run(prog, 1);
    TC("ADD HL,BC overflow: H");   ASSERT_EQ(gb.cpu.h,   0x00);
    TC("ADD HL,BC overflow: L");   ASSERT_EQ(gb.cpu.l,   0x00);
    TC("ADD HL,BC overflow: C=1"); ASSERT_EQ(gb.cpu.f_c, true);

    /* ADD HL,HL (0x29) */
    gb_mock_init(&gb);
    gb.cpu.h = 0x00; gb.cpu.l = 0x80;
    prog[0] = 0x29;
    run(prog, 1);
    TC("ADD HL,HL: H"); ASSERT_EQ(gb.cpu.h, 0x01);
    TC("ADD HL,HL: L"); ASSERT_EQ(gb.cpu.l, 0x00);

    /* 16-bit ops must NOT touch Z flag */
    gb_mock_init(&gb);
    gb.cpu.f_z = true;
    gb.cpu.h = 0x10; gb.cpu.l = 0x00;
    gb.cpu.b = 0x00; gb.cpu.c = 0x01;
    prog[0] = 0x09;
    run(prog, 1);
    TC("ADD HL,BC: Z flag unchanged"); ASSERT_EQ(gb.cpu.f_z, true);
}

/* ================================================================== */
/* Jump / branch                                                          */
/* ================================================================== */

static void test_jumps(void)
{
    printf("--- Jumps / Branches ---\n");
    uint8_t prog[3];

    /* JP i16 (0xC3) */
    gb_mock_init(&gb);
    prog[0] = 0xC3; prog[1] = 0x00; prog[2] = 0x02; /* JP 0x0200 */
    run(prog, 3);
    TC("JP i16"); ASSERT_EQ(gb.cpu.pc, 0x0200);

    /* JR si8 (0x18): forward offset */
    gb_mock_init(&gb);
    prog[0] = 0x18; prog[1] = 0x05;  /* JR +5; PC after fetch = 0x102; dest = 0x107 */
    run(prog, 2);
    TC("JR +5"); ASSERT_EQ(gb.cpu.pc, 0x107);

    /* JR si8: backward offset */
    gb_mock_init(&gb);
    prog[0] = 0x18; prog[1] = (uint8_t)(-4);  /* JR -4; 0x102 - 4 = 0x0FE */
    run(prog, 2);
    TC("JR -4"); ASSERT_EQ(gb.cpu.pc, 0x0FE);

    /* JR NZ (0x20): taken when Z=0 */
    gb_mock_init(&gb);
    gb.cpu.f_z = false;
    prog[0] = 0x20; prog[1] = 0x04;
    run(prog, 2);
    TC("JR NZ taken: PC"); ASSERT_EQ(gb.cpu.pc, 0x106);

    /* JR NZ: not taken when Z=1 */
    gb_mock_init(&gb);
    gb.cpu.f_z = true;
    prog[0] = 0x20; prog[1] = 0x04;
    run(prog, 2);
    TC("JR NZ not-taken: PC"); ASSERT_EQ(gb.cpu.pc, 0x102);

    /* JR Z (0x28): taken when Z=1 */
    gb_mock_init(&gb);
    gb.cpu.f_z = true;
    prog[0] = 0x28; prog[1] = 0x02;
    run(prog, 2);
    TC("JR Z taken: PC"); ASSERT_EQ(gb.cpu.pc, 0x104);

    /* JR C (0x38): taken when C=1 */
    gb_mock_init(&gb);
    gb.cpu.f_c = true;
    prog[0] = 0x38; prog[1] = 0x08;
    run(prog, 2);
    TC("JR C taken: PC"); ASSERT_EQ(gb.cpu.pc, 0x10A);

    /* JR NC (0x30): not taken when C=1 */
    gb_mock_init(&gb);
    gb.cpu.f_c = true;
    prog[0] = 0x30; prog[1] = 0x08;
    run(prog, 2);
    TC("JR NC not-taken: PC"); ASSERT_EQ(gb.cpu.pc, 0x102);

    /* JP HL (0xE9) */
    gb_mock_init(&gb);
    gb.cpu.h = 0x03; gb.cpu.l = 0x00;
    prog[0] = 0xE9;
    run(prog, 1);
    TC("JP HL"); ASSERT_EQ(gb.cpu.pc, 0x0300);

    /* JP NZ,i16 (0xC2): taken */
    gb_mock_init(&gb);
    gb.cpu.f_z = false;
    prog[0] = 0xC2; prog[1] = 0x00; prog[2] = 0x05;
    run(prog, 3);
    TC("JP NZ taken"); ASSERT_EQ(gb.cpu.pc, 0x0500);

    /* JP NZ,i16: not taken */
    gb_mock_init(&gb);
    gb.cpu.f_z = true;
    prog[0] = 0xC2; prog[1] = 0x00; prog[2] = 0x05;
    run(prog, 3);
    TC("JP NZ not-taken"); ASSERT_EQ(gb.cpu.pc, 0x103);

    /* JP Z,i16 (0xCA): taken */
    gb_mock_init(&gb);
    gb.cpu.f_z = true;
    prog[0] = 0xCA; prog[1] = 0x00; prog[2] = 0x06;
    run(prog, 3);
    TC("JP Z taken"); ASSERT_EQ(gb.cpu.pc, 0x0600);

    /* JP C,i16 (0xDA): not taken */
    gb_mock_init(&gb);
    gb.cpu.f_c = false;
    prog[0] = 0xDA; prog[1] = 0x00; prog[2] = 0x07;
    run(prog, 3);
    TC("JP C not-taken"); ASSERT_EQ(gb.cpu.pc, 0x103);
}

/* ================================================================== */
/* CALL / RET / PUSH / POP                                               */
/* ================================================================== */

static void test_call_ret_stack(void)
{
    printf("--- CALL / RET / PUSH / POP ---\n");
    uint8_t prog[3];

    /* CALL i16 (0xCD) */
    gb_mock_init(&gb);
    uint16_t sp0    = gb.cpu.sp;   /* 0xFFFE */
    uint16_t retaddr = 0x0103;     /* 0x100 + 3-byte CALL */
    prog[0] = 0xCD; prog[1] = 0x00; prog[2] = 0x04; /* CALL 0x0400 */
    run(prog, 3);
    TC("CALL: PC at target");   ASSERT_EQ(gb.cpu.pc, 0x0400);
    TC("CALL: SP decremented"); ASSERT_EQ(gb.cpu.sp, sp0 - 2);
    /* Stack contents: low byte of return addr at new SP, high byte above it */
    TC("CALL: retaddr lo on stack"); ASSERT_EQ(gb_mock_peek(gb.cpu.sp),     retaddr & 0xFF);
    TC("CALL: retaddr hi on stack"); ASSERT_EQ(gb_mock_peek(gb.cpu.sp + 1), retaddr >> 8);

    /* RET (0xC9): restore PC and SP */
    prog[0] = 0xC9;
    gb_mock_write(&gb, 0x0400, prog, 1);
    step();
    TC("RET: PC restored"); ASSERT_EQ(gb.cpu.pc, retaddr);
    TC("RET: SP restored"); ASSERT_EQ(gb.cpu.sp, sp0);

    /* CALL Z: taken (Z=1) */
    gb_mock_init(&gb);
    gb.cpu.f_z = true;
    prog[0] = 0xCC; prog[1] = 0x00; prog[2] = 0x05;
    run(prog, 3);
    TC("CALL Z taken: PC"); ASSERT_EQ(gb.cpu.pc, 0x0500);

    /* CALL Z: not taken (Z=0) */
    gb_mock_init(&gb);
    gb.cpu.f_z = false;
    prog[0] = 0xCC; prog[1] = 0x00; prog[2] = 0x05;
    run(prog, 3);
    TC("CALL Z not-taken: PC"); ASSERT_EQ(gb.cpu.pc, 0x103);

    /* CALL NZ: taken (Z=0) */
    gb_mock_init(&gb);
    gb.cpu.f_z = false;
    prog[0] = 0xC4; prog[1] = 0x00; prog[2] = 0x06;
    run(prog, 3);
    TC("CALL NZ taken: PC"); ASSERT_EQ(gb.cpu.pc, 0x0600);

    /* RET Z: taken (Z=1) */
    gb_mock_init(&gb);
    /* push a return address manually: 0xBEEF */
    gb.cpu.sp = 0xFFFC;
    gb_mock_peek(0);                              /* silence unused warning */
    uint8_t patch[2] = { 0xEF, 0xBE };           /* 0xBEEF little-endian */
    gb_mock_write(&gb, 0xFFFC, patch, 2);
    gb.cpu.f_z = true;
    prog[0] = 0xC8;                               /* RET Z */
    run(prog, 1);
    TC("RET Z taken: PC"); ASSERT_EQ(gb.cpu.pc, 0xBEEF);

    /* RET NZ: not taken (Z=1) */
    gb_mock_init(&gb);
    gb.cpu.f_z = true;
    prog[0] = 0xC0;                               /* RET NZ */
    run(prog, 1);
    TC("RET NZ not-taken: PC"); ASSERT_EQ(gb.cpu.pc, 0x101);

    /* PUSH BC (0xC5) / POP DE (0xD1) */
    gb_mock_init(&gb);
    gb.cpu.b = 0xAB; gb.cpu.c = 0xCD;
    prog[0] = 0xC5;
    run(prog, 1);
    prog[0] = 0xD1;
    run(prog, 1);
    TC("PUSH BC / POP DE: D"); ASSERT_EQ(gb.cpu.d, 0xAB);
    TC("PUSH BC / POP DE: E"); ASSERT_EQ(gb.cpu.e, 0xCD);
    TC("PUSH/POP SP balanced"); ASSERT_EQ(gb.cpu.sp, 0xFFFE);

    /* PUSH DE (0xD5) / POP HL (0xE1) */
    gb_mock_init(&gb);
    gb.cpu.d = 0x12; gb.cpu.e = 0x34;
    prog[0] = 0xD5;
    run(prog, 1);
    prog[0] = 0xE1;
    run(prog, 1);
    TC("PUSH DE / POP HL: H"); ASSERT_EQ(gb.cpu.h, 0x12);
    TC("PUSH DE / POP HL: L"); ASSERT_EQ(gb.cpu.l, 0x34);

    /* PUSH HL (0xE5) / POP AF (0xF1) */
    gb_mock_init(&gb);
    gb.cpu.h = 0x55; gb.cpu.l = 0xF0;
    prog[0] = 0xE5;
    run(prog, 1);
    prog[0] = 0xF1;
    run(prog, 1);
    TC("PUSH HL / POP AF: A"); ASSERT_EQ(gb.cpu.a, 0x55);

    /* PUSH AF (0xF5) / POP BC (0xC1) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x99; gb.cpu.f_z = true; gb.cpu.f_c = true;
    prog[0] = 0xF5;
    run(prog, 1);
    gb.cpu.a = 0; /* clear to verify pop */
    prog[0] = 0xC1;
    run(prog, 1);
    TC("PUSH AF / POP BC: B (=A)"); ASSERT_EQ(gb.cpu.b, 0x99);
}

/* ================================================================== */
/* Miscellaneous: SCF / CCF / CPL / HALT / DI / EI                      */
/* ================================================================== */

static void test_misc(void)
{
    printf("--- Misc (SCF/CCF/CPL/HALT/DI/EI) ---\n");
    uint8_t prog[1];

    /* SCF (0x37): set carry; clear N, H */
    gb_mock_init(&gb);
    gb.cpu.f_c = false; gb.cpu.f_n = true; gb.cpu.f_h = true;
    prog[0] = 0x37;
    run(prog, 1);
    TC("SCF: C=1"); ASSERT_EQ(gb.cpu.f_c, true);
    TC("SCF: N=0"); ASSERT_EQ(gb.cpu.f_n, false);
    TC("SCF: H=0"); ASSERT_EQ(gb.cpu.f_h, false);

    /* CCF (0x3F): toggle carry; clear N, H */
    gb_mock_init(&gb);
    gb.cpu.f_c = true; gb.cpu.f_n = true; gb.cpu.f_h = true;
    prog[0] = 0x3F;
    run(prog, 1);
    TC("CCF 1->0: C=0"); ASSERT_EQ(gb.cpu.f_c, false);
    TC("CCF: N=0");       ASSERT_EQ(gb.cpu.f_n, false);
    TC("CCF: H=0");       ASSERT_EQ(gb.cpu.f_h, false);

    gb_mock_init(&gb);
    gb.cpu.f_c = false;
    prog[0] = 0x3F;
    run(prog, 1);
    TC("CCF 0->1: C=1"); ASSERT_EQ(gb.cpu.f_c, true);

    /* CPL A (0x2F): bitwise NOT, sets N and H */
    gb_mock_init(&gb);
    gb.cpu.a = 0xAA;
    prog[0] = 0x2F;
    run(prog, 1);
    TC("CPL A: value"); ASSERT_EQ(gb.cpu.a,   0x55);
    TC("CPL A: N=1");   ASSERT_EQ(gb.cpu.f_n, true);
    TC("CPL A: H=1");   ASSERT_EQ(gb.cpu.f_h, true);

    gb_mock_init(&gb);
    gb.cpu.a = 0x00;
    prog[0] = 0x2F;
    run(prog, 1);
    TC("CPL 0x00: value"); ASSERT_EQ(gb.cpu.a, 0xFF);

    /* HALT (0x76): sets halted flag */
    gb_mock_init(&gb);
    prog[0] = 0x76;
    run(prog, 1);
    TC("HALT: halted=true"); ASSERT_EQ(gb.cpu.halted, true);

    /* DI (0xF3): clears both irq_enable bits immediately */
    gb_mock_init(&gb);
    gb.cpu.irq_enable      = true;
    gb.cpu.irq_enable_next = true;
    prog[0] = 0xF3;
    run(prog, 1);
    TC("DI: irq_enable=false");      ASSERT_EQ(gb.cpu.irq_enable,      false);
    TC("DI: irq_enable_next=false"); ASSERT_EQ(gb.cpu.irq_enable_next, false);

    /* EI (0xFB): schedules re-enable after the NEXT instruction */
    gb_mock_init(&gb);
    gb.cpu.irq_enable      = false;
    gb.cpu.irq_enable_next = false;
    prog[0] = 0xFB;
    run(prog, 1);
    TC("EI: irq_enable_next=true"); ASSERT_EQ(gb.cpu.irq_enable_next, true);
}

/* ================================================================== */
/* CB prefix: rotate / shift / swap                                      */
/* ================================================================== */

static void test_cb_rotate_shift(void)
{
    printf("--- CB: RLC/RRC/RL/RR/SLA/SRA/SRL/SWAP ---\n");
    uint8_t prog[2];

    /* --- RLC B (0xCB 0x00) --- */
    /* 0x85 = 1000_0101 → rotate left → 0000_1011, C=1 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x85;
    prog[0] = 0xCB; prog[1] = 0x00;
    run(prog, 2);
    TC("RLC B 0x85: value"); ASSERT_EQ(gb.cpu.b,   0x0B);
    TC("RLC B 0x85: C=1");   ASSERT_EQ(gb.cpu.f_c, true);
    TC("RLC B 0x85: Z=0");   ASSERT_EQ(gb.cpu.f_z, false);
    TC("RLC B 0x85: N=0");   ASSERT_EQ(gb.cpu.f_n, false);
    TC("RLC B 0x85: H=0");   ASSERT_EQ(gb.cpu.f_h, false);

    /* RLC B: 0x00 → 0x00, Z=1 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x00;
    prog[0] = 0xCB; prog[1] = 0x00;
    run(prog, 2);
    TC("RLC B 0: Z=1");   ASSERT_EQ(gb.cpu.f_z, true);
    TC("RLC B 0: C=0");   ASSERT_EQ(gb.cpu.f_c, false);

    /* --- RRC A (0xCB 0x0F) --- */
    /* 0x01 = 0000_0001 → rotate right → 1000_0000, C=1 */
    gb_mock_init(&gb);
    gb.cpu.a = 0x01;
    prog[0] = 0xCB; prog[1] = 0x0F;
    run(prog, 2);
    TC("RRC A 0x01: value"); ASSERT_EQ(gb.cpu.a,   0x80);
    TC("RRC A 0x01: C=1");   ASSERT_EQ(gb.cpu.f_c, true);

    /* RRC A: 0x00 → 0x00, Z=1 */
    gb_mock_init(&gb);
    gb.cpu.a = 0x00;
    prog[0] = 0xCB; prog[1] = 0x0F;
    run(prog, 2);
    TC("RRC A 0: Z=1"); ASSERT_EQ(gb.cpu.f_z, true);

    /* --- RL B (0xCB 0x10): rotate left through carry --- */
    /* 0x80, C=0 → 0x00, new C=1, Z=1 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x80; gb.cpu.f_c = false;
    prog[0] = 0xCB; prog[1] = 0x10;
    run(prog, 2);
    TC("RL B 0x80 C=0: value"); ASSERT_EQ(gb.cpu.b,   0x00);
    TC("RL B 0x80 C=0: C=1");   ASSERT_EQ(gb.cpu.f_c, true);
    TC("RL B 0x80 C=0: Z=1");   ASSERT_EQ(gb.cpu.f_z, true);

    /* 0x01, C=1 → 0x03, new C=0 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x01; gb.cpu.f_c = true;
    prog[0] = 0xCB; prog[1] = 0x10;
    run(prog, 2);
    TC("RL B 0x01 C=1: value"); ASSERT_EQ(gb.cpu.b,   0x03);
    TC("RL B 0x01 C=1: C=0");   ASSERT_EQ(gb.cpu.f_c, false);

    /* --- RR A (0xCB 0x1F): rotate right through carry --- */
    /* 0x01, C=1 → 0x80 (old C into bit 7), new C=1 (old bit 0) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x01; gb.cpu.f_c = true;
    prog[0] = 0xCB; prog[1] = 0x1F;
    run(prog, 2);
    TC("RR A 0x01 C=1: value"); ASSERT_EQ(gb.cpu.a,   0x80);
    TC("RR A 0x01 C=1: C=1");   ASSERT_EQ(gb.cpu.f_c, true);

    /* --- SLA B (0xCB 0x20): shift left, bit 0 = 0 --- */
    /* 0x81 = 1000_0001 → 0x02, C=1 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x81;
    prog[0] = 0xCB; prog[1] = 0x20;
    run(prog, 2);
    TC("SLA B 0x81: value"); ASSERT_EQ(gb.cpu.b,   0x02);
    TC("SLA B 0x81: C=1");   ASSERT_EQ(gb.cpu.f_c, true);

    /* SLA: 0x80 → 0x00, Z=1, C=1 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x80;
    prog[0] = 0xCB; prog[1] = 0x20;
    run(prog, 2);
    TC("SLA B 0x80: Z=1"); ASSERT_EQ(gb.cpu.f_z, true);
    TC("SLA B 0x80: C=1"); ASSERT_EQ(gb.cpu.f_c, true);

    /* --- SRA A (0xCB 0x2F): arithmetic right shift, sign-extends --- */
    /* 0x80 = 1000_0000 → 0xC0, C=0 */
    gb_mock_init(&gb);
    gb.cpu.a = 0x80;
    prog[0] = 0xCB; prog[1] = 0x2F;
    run(prog, 2);
    TC("SRA A 0x80: value"); ASSERT_EQ(gb.cpu.a,   0xC0);
    TC("SRA A 0x80: C=0");   ASSERT_EQ(gb.cpu.f_c, false);

    /* 0x81 = 1000_0001 → 0xC0, C=1 */
    gb_mock_init(&gb);
    gb.cpu.a = 0x81;
    prog[0] = 0xCB; prog[1] = 0x2F;
    run(prog, 2);
    TC("SRA A 0x81: value"); ASSERT_EQ(gb.cpu.a,   0xC0);
    TC("SRA A 0x81: C=1");   ASSERT_EQ(gb.cpu.f_c, true);

    /* --- SRL B (0xCB 0x38): logical right shift, bit 7 = 0 --- */
    /* 0x81 → 0x40, C=1 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x81;
    prog[0] = 0xCB; prog[1] = 0x38;
    run(prog, 2);
    TC("SRL B 0x81: value"); ASSERT_EQ(gb.cpu.b,   0x40);
    TC("SRL B 0x81: C=1");   ASSERT_EQ(gb.cpu.f_c, true);

    /* SRL: 0x01 → 0x00, Z=1 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x01;
    prog[0] = 0xCB; prog[1] = 0x38;
    run(prog, 2);
    TC("SRL B 0x01: Z=1"); ASSERT_EQ(gb.cpu.f_z, true);
    TC("SRL B 0x01: C=1"); ASSERT_EQ(gb.cpu.f_c, true);

    /* --- SWAP B (0xCB 0x30): swap nibbles --- */
    gb_mock_init(&gb);
    gb.cpu.b = 0xAB;
    prog[0] = 0xCB; prog[1] = 0x30;
    run(prog, 2);
    TC("SWAP B 0xAB: value"); ASSERT_EQ(gb.cpu.b,   0xBA);
    TC("SWAP B 0xAB: Z=0");   ASSERT_EQ(gb.cpu.f_z, false);
    TC("SWAP B: N=0");         ASSERT_EQ(gb.cpu.f_n, false);
    TC("SWAP B: H=0");         ASSERT_EQ(gb.cpu.f_h, false);
    TC("SWAP B: C=0");         ASSERT_EQ(gb.cpu.f_c, false);

    /* SWAP 0x00 → 0x00, Z=1 */
    gb_mock_init(&gb);
    gb.cpu.b = 0x00;
    prog[0] = 0xCB; prog[1] = 0x30;
    run(prog, 2);
    TC("SWAP B 0x00: Z=1"); ASSERT_EQ(gb.cpu.f_z, true);

    /* SWAP A (0xCB 0x37) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x5A;
    prog[0] = 0xCB; prog[1] = 0x37;
    run(prog, 2);
    TC("SWAP A 0x5A: value"); ASSERT_EQ(gb.cpu.a, 0xA5);
}

/* ================================================================== */
/* CB prefix: BIT / SET / RES                                            */
/* ================================================================== */

static void test_cb_bits(void)
{
    printf("--- CB: BIT / SET / RES ---\n");
    uint8_t prog[2];

    /* --- BIT b,r: sets Z = NOT(bit), always sets H, clears N --- */

    /* BIT 0,B (0xCB 0x40): bit set */
    gb_mock_init(&gb);
    gb.cpu.b = 0x01;
    prog[0] = 0xCB; prog[1] = 0x40;
    run(prog, 2);
    TC("BIT 0,B set: Z=0"); ASSERT_EQ(gb.cpu.f_z, false);
    TC("BIT 0,B set: H=1"); ASSERT_EQ(gb.cpu.f_h, true);
    TC("BIT 0,B set: N=0"); ASSERT_EQ(gb.cpu.f_n, false);

    /* BIT 0,B: bit clear */
    gb_mock_init(&gb);
    gb.cpu.b = 0xFE;
    prog[0] = 0xCB; prog[1] = 0x40;
    run(prog, 2);
    TC("BIT 0,B clear: Z=1"); ASSERT_EQ(gb.cpu.f_z, true);

    /* BIT 7,A (0xCB 0x7F): bit set */
    gb_mock_init(&gb);
    gb.cpu.a = 0x80;
    prog[0] = 0xCB; prog[1] = 0x7F;
    run(prog, 2);
    TC("BIT 7,A set: Z=0"); ASSERT_EQ(gb.cpu.f_z, false);

    /* BIT 7,A: bit clear */
    gb_mock_init(&gb);
    gb.cpu.a = 0x7F;
    prog[0] = 0xCB; prog[1] = 0x7F;
    run(prog, 2);
    TC("BIT 7,A clear: Z=1"); ASSERT_EQ(gb.cpu.f_z, true);

    /* BIT 3,C (0xCB 0x59): bit 3 set */
    gb_mock_init(&gb);
    gb.cpu.c = 0x08;
    prog[0] = 0xCB; prog[1] = 0x59;
    run(prog, 2);
    TC("BIT 3,C set: Z=0"); ASSERT_EQ(gb.cpu.f_z, false);

    /* BIT must NOT alter register value */
    gb_mock_init(&gb);
    gb.cpu.b = 0xAA;
    prog[0] = 0xCB; prog[1] = 0x40;
    run(prog, 2);
    TC("BIT 0,B: B unchanged"); ASSERT_EQ(gb.cpu.b, 0xAA);

    /* --- SET b,r: sets the given bit, no flag changes --- */

    /* SET 0,B (0xCB 0xC0) */
    gb_mock_init(&gb);
    gb.cpu.b = 0x00;
    prog[0] = 0xCB; prog[1] = 0xC0;
    run(prog, 2);
    TC("SET 0,B: bit 0 set"); ASSERT_EQ(gb.cpu.b, 0x01);

    /* SET 7,A (0xCB 0xFF) */
    gb_mock_init(&gb);
    gb.cpu.a = 0x00;
    prog[0] = 0xCB; prog[1] = 0xFF;
    run(prog, 2);
    TC("SET 7,A: bit 7 set"); ASSERT_EQ(gb.cpu.a, 0x80);

    /* SET is idempotent */
    gb_mock_init(&gb);
    gb.cpu.b = 0xFF;
    prog[0] = 0xCB; prog[1] = 0xC0;
    run(prog, 2);
    TC("SET 0,B idempotent"); ASSERT_EQ(gb.cpu.b, 0xFF);

    /* SET 4,D (0xCB 0xE2) */
    gb_mock_init(&gb);
    gb.cpu.d = 0x00;
    prog[0] = 0xCB; prog[1] = 0xE2;
    run(prog, 2);
    TC("SET 4,D"); ASSERT_EQ(gb.cpu.d, 0x10);

    /* --- RES b,r: clears the given bit --- */

    /* RES 0,B (0xCB 0x80) */
    gb_mock_init(&gb);
    gb.cpu.b = 0xFF;
    prog[0] = 0xCB; prog[1] = 0x80;
    run(prog, 2);
    TC("RES 0,B: bit 0 clear"); ASSERT_EQ(gb.cpu.b, 0xFE);

    /* RES 7,A (0xCB 0xBF) */
    gb_mock_init(&gb);
    gb.cpu.a = 0xFF;
    prog[0] = 0xCB; prog[1] = 0xBF;
    run(prog, 2);
    TC("RES 7,A: bit 7 clear"); ASSERT_EQ(gb.cpu.a, 0x7F);

    /* RES is idempotent */
    gb_mock_init(&gb);
    gb.cpu.b = 0x00;
    prog[0] = 0xCB; prog[1] = 0x80;
    run(prog, 2);
    TC("RES 0,B idempotent"); ASSERT_EQ(gb.cpu.b, 0x00);

    /* RES 3,E (0xCB 0x9B) */
    gb_mock_init(&gb);
    gb.cpu.e = 0xFF;
    prog[0] = 0xCB; prog[1] = 0x9B;
    run(prog, 2);
    TC("RES 3,E"); ASSERT_EQ(gb.cpu.e, 0xFF & ~0x08);
}

/* ================================================================== */
/* main                                                                   */
/* ================================================================== */

int main(void)
{
    printf("==========================================\n");
    printf("  Game Boy CPU — Opcode Unit Tests\n");
    printf("==========================================\n\n");

    test_nop();
    test_inc_dec_8();
    test_add_adc();
    test_sub_sbc();
    test_logic();
    test_loads();
    test_16bit();
    test_jumps();
    test_call_ret_stack();
    test_misc();
    test_cb_rotate_shift();
    test_cb_bits();

    printf("\n==========================================\n");
    if (g_fail == 0) {
        printf("  ALL %d TESTS PASSED\n", g_pass);
    } else {
        printf("  %d passed, %d FAILED\n", g_pass, g_fail);
    }
    printf("==========================================\n");

    return g_fail ? 1 : 0;
}
