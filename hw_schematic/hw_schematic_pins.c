/* hw_schematic_pins.c — DMG CPU (U1) pin table.
 *
 * Pin layout follows DMG-CPU-06.kicad_sch (Gekkio, CC BY 4.0).
 * The DMG CPU is an 80-pin QFP ASIC.
 *
 *   Pins  1-20  — right side (top to bottom)
 *   Pins 21-40  — bottom side (right to left)
 *   Pins 41-60  — left side (bottom to top)
 *   Pins 61-80  — top side (left to right)
 *
 * net_id values are indices into hw_nets[] (hw_schematic_data.c).
 * -1 means the pin has no connected net in the schematic (N/C or power tie).
 *
 * pos is the fractional position along each side [0..1] for UI rendering.
 * It is evenly spaced: pin i on a 20-pin side → pos = (i-0.5)/20.
 */
#include "hw_schematic_pins.h"
#include <stddef.h>

/* Convenience macro: evenly-spaced position, 20 pins per side, index 0-based */
#define POS(i) ((float)((i) + 0.5f) / 20.0f)

/* side constants */
#define RIGHT 1
#define BOTTOM 3
#define LEFT 0
#define TOP 2

/* net_id values — cross-reference hw_schematic_map.c for signal names */
/* Address bus */
#define N_A0 8
#define N_A1 20
#define N_A2 29
#define N_A3 44
#define N_A4 4
#define N_A5 24
#define N_A6 86
#define N_A7 26
#define N_A8 73
#define N_A9 66
#define N_A10 30
#define N_A11 83
#define N_A12 22
#define N_A13 19
#define N_A14 47
#define N_A15 85
/* Data bus */
#define N_D0 74
#define N_D1 62
#define N_D2 54
#define N_D3 27
#define N_D4 72
#define N_D5 10
#define N_D6 67
#define N_D7 2
/* Bus control */
#define N_nRD 82
#define N_nWR 80
#define N_nCS 76
#define N_nMCS 33
#define N_nMRD 15
#define N_nMWR 46
/* WRAM address bus */
#define N_MA0 81
#define N_MA1 56
#define N_MA2 0
#define N_MA3 90
#define N_MA4 88
#define N_MA5 37
#define N_MA6 75
#define N_MA7 57
#define N_MA8 14
#define N_MA9 91
#define N_MA10 39
#define N_MA11 28
#define N_MA12 89
/* WRAM data bus */
#define N_MD0 63
#define N_MD1 53
#define N_MD2 55
#define N_MD3 5
#define N_MD4 64
#define N_MD5 61
#define N_MD6 49
#define N_MD7 58
/* Misc */
#define N_PHI 70
#define N_nRES 65
#define N_LD0 38
#define N_LD1 71
#define N_CP 50
#define N_CPG 16
#define N_CPL 31
#define N_FR 60
#define N_S 87
#define N_ST 35
#define N_SO1 9
#define N_SO2 42
#define N_VEE 6
#define N_SCK 84
#define N_SIN 21
#define N_SOUT 12
#define N_P10 17
#define N_P11 51
#define N_P12 36
#define N_P13 45
#define N_P14 59
#define N_P15 3
#define N_VCC 13
#define N_VDD 7
#define N_VIN 23
#define N_GND 18
#define N_NC -1

/*
 * Full 80-pin table.
 * Layout matches Gekkio's DMG-CPU-06 schematic pin numbering.
 */
const HwU1Pin hw_u1_pins[HW_U1_PIN_COUNT] = {
    /* Right side: pins 1-20 (top → bottom) */
    {1, "VCC", N_VCC, RIGHT, POS(0)},
    {2, "PHI", N_PHI, RIGHT, POS(1)},
    {3, "nRES", N_nRES, RIGHT, POS(2)},
    {4, "nRD", N_nRD, RIGHT, POS(3)},
    {5, "nWR", N_nWR, RIGHT, POS(4)},
    {6, "nCS", N_nCS, RIGHT, POS(5)},
    {7, "A15", N_A15, RIGHT, POS(6)},
    {8, "A14", N_A14, RIGHT, POS(7)},
    {9, "A13", N_A13, RIGHT, POS(8)},
    {10, "A12", N_A12, RIGHT, POS(9)},
    {11, "A11", N_A11, RIGHT, POS(10)},
    {12, "A10", N_A10, RIGHT, POS(11)},
    {13, "A9", N_A9, RIGHT, POS(12)},
    {14, "A8", N_A8, RIGHT, POS(13)},
    {15, "A7", N_A7, RIGHT, POS(14)},
    {16, "A6", N_A6, RIGHT, POS(15)},
    {17, "A5", N_A5, RIGHT, POS(16)},
    {18, "A4", N_A4, RIGHT, POS(17)},
    {19, "A3", N_A3, RIGHT, POS(18)},
    {20, "A2", N_A2, RIGHT, POS(19)},

    /* Bottom side: pins 21-40 (right → left) */
    {21, "A1", N_A1, BOTTOM, POS(0)},
    {22, "A0", N_A0, BOTTOM, POS(1)},
    {23, "D0", N_D0, BOTTOM, POS(2)},
    {24, "D1", N_D1, BOTTOM, POS(3)},
    {25, "D2", N_D2, BOTTOM, POS(4)},
    {26, "D3", N_D3, BOTTOM, POS(5)},
    {27, "D4", N_D4, BOTTOM, POS(6)},
    {28, "D5", N_D5, BOTTOM, POS(7)},
    {29, "D6", N_D6, BOTTOM, POS(8)},
    {30, "D7", N_D7, BOTTOM, POS(9)},
    {31, "GND", N_GND, BOTTOM, POS(10)},
    {32, "VDD", N_VDD, BOTTOM, POS(11)},
    {33, "P10", N_P10, BOTTOM, POS(12)},
    {34, "P11", N_P11, BOTTOM, POS(13)},
    {35, "P12", N_P12, BOTTOM, POS(14)},
    {36, "P13", N_P13, BOTTOM, POS(15)},
    {37, "P14", N_P14, BOTTOM, POS(16)},
    {38, "P15", N_P15, BOTTOM, POS(17)},
    {39, "SCK", N_SCK, BOTTOM, POS(18)},
    {40, "SIN", N_SIN, BOTTOM, POS(19)},

    /* Left side: pins 41-60 (bottom → top) */
    {41, "SOUT", N_SOUT, LEFT, POS(0)},
    {42, "SO1", N_SO1, LEFT, POS(1)},
    {43, "SO2", N_SO2, LEFT, POS(2)},
    {44, "VEE", N_VEE, LEFT, POS(3)},
    {45, "LD0", N_LD0, LEFT, POS(4)},
    {46, "LD1", N_LD1, LEFT, POS(5)},
    {47, "CP", N_CP, LEFT, POS(6)},
    {48, "CPG", N_CPG, LEFT, POS(7)},
    {49, "CPL", N_CPL, LEFT, POS(8)},
    {50, "FR", N_FR, LEFT, POS(9)},
    {51, "S", N_S, LEFT, POS(10)},
    {52, "ST", N_ST, LEFT, POS(11)},
    {53, "MA0", N_MA0, LEFT, POS(12)},
    {54, "MA1", N_MA1, LEFT, POS(13)},
    {55, "MA2", N_MA2, LEFT, POS(14)},
    {56, "MA3", N_MA3, LEFT, POS(15)},
    {57, "MA4", N_MA4, LEFT, POS(16)},
    {58, "MA5", N_MA5, LEFT, POS(17)},
    {59, "MA6", N_MA6, LEFT, POS(18)},
    {60, "MA7", N_MA7, LEFT, POS(19)},

    /* Top side: pins 61-80 (left → right) */
    {61, "MA8", N_MA8, TOP, POS(0)},
    {62, "MA9", N_MA9, TOP, POS(1)},
    {63, "MA10", N_MA10, TOP, POS(2)},
    {64, "MA11", N_MA11, TOP, POS(3)},
    {65, "MA12", N_MA12, TOP, POS(4)},
    {66, "MD0", N_MD0, TOP, POS(5)},
    {67, "MD1", N_MD1, TOP, POS(6)},
    {68, "MD2", N_MD2, TOP, POS(7)},
    {69, "MD3", N_MD3, TOP, POS(8)},
    {70, "MD4", N_MD4, TOP, POS(9)},
    {71, "MD5", N_MD5, TOP, POS(10)},
    {72, "MD6", N_MD6, TOP, POS(11)},
    {73, "MD7", N_MD7, TOP, POS(12)},
    {74, "nMCS", N_nMCS, TOP, POS(13)},
    {75, "nMRD", N_nMRD, TOP, POS(14)},
    {76, "nMWR", N_nMWR, TOP, POS(15)},
    {77, "VIN", N_VIN, TOP, POS(16)},
    {78, "GND", N_GND, TOP, POS(17)},
    {79, "VCC", N_VCC, TOP, POS(18)},
    {80, "NC", N_NC, TOP, POS(19)},
};

const HwU1Pin *hw_u1_pin_find(int pin_number)
{
     for (int i = 0; i < HW_U1_PIN_COUNT; i++)
          if (hw_u1_pins[i].pin == (uint8_t)pin_number)
               return &hw_u1_pins[i];
     return NULL;
}

const HwU1Pin *hw_u1_pin_find_net(int net_id)
{
     if (net_id < 0)
          return NULL;
     for (int i = 0; i < HW_U1_PIN_COUNT; i++)
          if (hw_u1_pins[i].net_id == (int16_t)net_id)
               return &hw_u1_pins[i];
     return NULL;
}
