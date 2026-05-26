/* hw_schematic_data.h -- generated from DMG-CPU-06.kicad_sch */
/* Source: https://github.com/Gekkio/gb-schematics (CC-BY 4.0) */
#pragma once
#include <stdint.h>

/* Coordinate space: [0,1] normalized from KiCad A4 paper (297x210mm)
 * nx = kicad_x / 297,  ny = kicad_y / 210 */

typedef struct {
    const char *ref;
    const char *value;
    float nx, ny;          /* center, normalized */
    float nw, nh;          /* estimated size, normalized */
    int   signal_group;    /* -1 = no group */
} HwComponent;

typedef struct {
    float   nx1, ny1, nx2, ny2;
    uint8_t is_bus;        /* 1 = bus line, 0 = wire */
    int16_t net_id;        /* index into hw_nets[], -1 = unknown */
} HwWire;

typedef struct {
    const char *name;      /* net name e.g. "A0", "D3", "PHI" */
    int8_t  anim_group;    /* animation group index, -1 = none */
} HwNet;

typedef struct {
    const char *text;
    float nx, ny;
    float angle;
} HwLabel;

typedef struct {
    float nx, ny;
} HwJunction;

/* Animation groups — index into HwNet.anim_group */
/* 0=addr  1=data  2=wram_data  3=wram_addr  4=clock
 * 5=audio 6=lcd   7=irq        8=power      9=serial 10=bus_ctrl */
#define HW_ANIM_ADDR      0
#define HW_ANIM_DATA      1
#define HW_ANIM_WRAM_DATA 2
#define HW_ANIM_WRAM_ADDR 3
#define HW_ANIM_CLOCK     4
#define HW_ANIM_AUDIO     5
#define HW_ANIM_LCD       6
#define HW_ANIM_IRQ       7
#define HW_ANIM_POWER     8
#define HW_ANIM_SERIAL    9
#define HW_ANIM_BUS_CTRL  10
#define HW_ANIM_GROUP_COUNT 11

#define HW_COMPONENT_COUNT  15
#define HW_WIRE_COUNT       411
#define HW_NET_COUNT        98
#define HW_LABEL_COUNT      180
#define HW_JUNCTION_COUNT   19

extern const HwComponent hw_components[HW_COMPONENT_COUNT];
extern const HwWire      hw_wires[HW_WIRE_COUNT];
extern const HwNet       hw_nets[HW_NET_COUNT];
extern const HwLabel     hw_labels[HW_LABEL_COUNT];
extern const HwJunction  hw_junctions[HW_JUNCTION_COUNT];
