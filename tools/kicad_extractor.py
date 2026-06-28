#!/usr/bin/env python3
"""
kicad_extractor.py -- extract geometry from KiCad 6+ .kicad_sch files
                      for the GB hardware schematic viewer.

Usage:
    python3 tools/kicad_extractor.py \
        /tmp/DMG-CPU-06.kicad_sch \
        --output-c hw_schematic/hw_schematic_data.c \
        --output-h hw_schematic/hw_schematic_data.h \
        --json data/dmg_components.json
"""

import sys
import re
import json
import hashlib
import argparse
from datetime import datetime, timezone
from pathlib import Path


# ---------------------------------------------------------------------------
# S-expression tokenizer / parser
# ---------------------------------------------------------------------------

def tokenize(text):
    """Yield tokens: '(' | ')' | string-literal | atom"""
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in ' \t\r\n':
            i += 1
        elif c == '(':
            yield '('
            i += 1
        elif c == ')':
            yield ')'
            i += 1
        elif c == '"':
            j = i + 1
            buf = []
            while j < n:
                if text[j] == '\\':
                    buf.append(text[j+1] if j+1 < n else '\\')
                    j += 2
                elif text[j] == '"':
                    j += 1
                    break
                else:
                    buf.append(text[j])
                    j += 1
            yield ('str', ''.join(buf))
            i = j
        else:
            j = i
            while j < n and text[j] not in ' \t\r\n()\"':
                j += 1
            yield ('atom', text[i:j])
            i = j


def parse(tokens_iter):
    tok = next(tokens_iter)
    if tok == '(':
        result = []
        while True:
            nxt = peek_or_none(tokens_iter)
            if nxt == ')':
                next(tokens_iter)
                break
            result.append(parse(tokens_iter))
        return result
    elif isinstance(tok, tuple):
        return tok[1]
    else:
        return tok


class _Peekable:
    def __init__(self, it):
        self._it = iter(it)
        self._buf = []

    def __iter__(self):
        return self

    def __next__(self):
        if self._buf:
            return self._buf.pop()
        return next(self._it)

    def peek(self):
        val = next(self._it)
        self._buf.append(val)
        return val


def peek_or_none(pit):
    try:
        return pit.peek()
    except StopIteration:
        return None


def parse_sexp(text):
    pit = _Peekable(tokenize(text))
    results = []
    while True:
        p = peek_or_none(pit)
        if p is None:
            break
        results.append(parse(pit))
    return results[0] if len(results) == 1 else results


# ---------------------------------------------------------------------------
# KiCad schematic element extractors
# ---------------------------------------------------------------------------

def find_all(node, key):
    if not isinstance(node, list):
        return
    if node and node[0] == key:
        yield node
        return
    for child in node:
        yield from find_all(child, key)


def get_attr(node, key, default=None):
    for child in node:
        if isinstance(child, list) and child and child[0] == key:
            return child[1] if len(child) > 1 else default
    return default


def parse_xy(node):
    return float(node[1]), float(node[2])


def parse_at(node):
    x = float(node[1])
    y = float(node[2])
    angle = float(node[3]) if len(node) > 3 else 0.0
    return x, y, angle


# ---------------------------------------------------------------------------
# Schematic paper bounds (KiCad A4 landscape)
# ---------------------------------------------------------------------------

PAPER_W = 297.0
PAPER_H = 210.0


def normalize(x, y):
    return x / PAPER_W, y / PAPER_H


# ---------------------------------------------------------------------------
# Parse one schematic file
# ---------------------------------------------------------------------------

def parse_schematic(path):
    text = Path(path).read_text(encoding='utf-8')
    root = parse_sexp(text)

    lib_idx = None
    for i, child in enumerate(root):
        if isinstance(child, list) and child and child[0] == 'lib_symbols':
            lib_idx = i
            break
    drawing = [c for i, c in enumerate(root) if i != lib_idx and i != 0]

    components = []
    wires = []
    buses = []
    labels = []
    junctions = []
    no_connects = []
    bus_entries = []
    sheets = []

    for elem in drawing:
        if not isinstance(elem, list) or not elem:
            continue
        kind = elem[0]

        if kind == 'wire':
            pts_node = next(find_all(elem, 'pts'), None)
            if pts_node:
                xys = [c for c in pts_node if isinstance(c, list) and c and c[0] == 'xy']
                if len(xys) >= 2:
                    x1, y1 = parse_xy(xys[0])
                    x2, y2 = parse_xy(xys[1])
                    nx1, ny1 = normalize(x1, y1)
                    nx2, ny2 = normalize(x2, y2)
                    wires.append({'x1': x1, 'y1': y1, 'x2': x2, 'y2': y2,
                                  'nx1': nx1, 'ny1': ny1, 'nx2': nx2, 'ny2': ny2})

        elif kind == 'bus':
            pts_node = next(find_all(elem, 'pts'), None)
            if pts_node:
                xys = [c for c in pts_node if isinstance(c, list) and c and c[0] == 'xy']
                if len(xys) >= 2:
                    x1, y1 = parse_xy(xys[0])
                    x2, y2 = parse_xy(xys[1])
                    nx1, ny1 = normalize(x1, y1)
                    nx2, ny2 = normalize(x2, y2)
                    buses.append({'x1': x1, 'y1': y1, 'x2': x2, 'y2': y2,
                                  'nx1': nx1, 'ny1': ny1, 'nx2': nx2, 'ny2': ny2,
                                  'is_bus': True})

        elif kind == 'bus_entry':
            at_node = next(find_all(elem, 'at'), None)
            size_node = next(find_all(elem, 'size'), None)
            if at_node and size_node:
                x, y, angle = parse_at(at_node)
                sw = float(size_node[1])
                sh = float(size_node[2])
                x2 = x + sw
                y2 = y + sh
                nx1, ny1 = normalize(x, y)
                nx2, ny2 = normalize(x2, y2)
                bus_entries.append({'x1': x, 'y1': y, 'x2': x2, 'y2': y2,
                                    'nx1': nx1, 'ny1': ny1, 'nx2': nx2, 'ny2': ny2})

        elif kind == 'label':
            text_val = elem[1] if len(elem) > 1 else ''
            at_node = next(find_all(elem, 'at'), None)
            if at_node:
                x, y, angle = parse_at(at_node)
                nx, ny = normalize(x, y)
                labels.append({'text': text_val, 'x': x, 'y': y, 'angle': angle,
                                'nx': nx, 'ny': ny})

        elif kind == 'junction':
            at_node = next(find_all(elem, 'at'), None)
            if at_node:
                x, y, _ = parse_at(at_node)
                nx, ny = normalize(x, y)
                junctions.append({'x': x, 'y': y, 'nx': nx, 'ny': ny})

        elif kind == 'no_connect':
            at_node = next(find_all(elem, 'at'), None)
            if at_node:
                x, y, _ = parse_at(at_node)
                nx, ny = normalize(x, y)
                no_connects.append({'x': x, 'y': y, 'nx': nx, 'ny': ny})

        elif kind == 'symbol' and len(elem) > 1 and isinstance(elem[1], list) and elem[1] and elem[1][0] == 'lib_id':
            lib_id = elem[1][1] if len(elem[1]) > 1 else ''
            at_node = next(find_all(elem, 'at'), None)
            if not at_node:
                continue
            x, y, angle = parse_at(at_node)
            nx, ny = normalize(x, y)

            ref = ''
            value = ''
            for prop in find_all(elem, 'property'):
                if len(prop) > 2:
                    pname = prop[1]
                    pval = prop[2]
                    if pname == 'Reference':
                        ref = pval
                    elif pname == 'Value':
                        value = pval

            if ref.startswith('#PWR') or ref.startswith('#FLG'):
                continue

            is_ic = not any(ref.startswith(p) for p in ('C', 'R', 'L', 'D', 'Q', 'X', 'Y'))
            w = 20.0 if is_ic else 5.0
            h = 20.0 if is_ic else 5.0
            nw, nh = normalize(w, h)

            components.append({
                'ref': ref, 'value': value, 'lib': lib_id,
                'x': x, 'y': y, 'angle': angle,
                'w': w, 'h': h,
                'nx': nx, 'ny': ny, 'nw': nw, 'nh': nh
            })

        elif kind == 'sheet':
            at_node = next(find_all(elem, 'at'), None)
            size_node = next(find_all(elem, 'size'), None)
            if not at_node or not size_node:
                continue
            x, y, _ = parse_at(at_node)
            w = float(size_node[1])
            h = float(size_node[2])
            nx, ny = normalize(x, y)
            nw, nh = normalize(w, h)
            name = ''
            for prop in find_all(elem, 'property'):
                if len(prop) > 2 and prop[1] == 'Sheetname':
                    name = prop[2]
                    break
            sheets.append({'name': name, 'x': x, 'y': y, 'w': w, 'h': h,
                           'nx': nx, 'ny': ny, 'nw': nw, 'nh': nh})

    return {
        'components': components,
        'wires': wires,
        'buses': buses,
        'labels': labels,
        'junctions': junctions,
        'no_connects': no_connects,
        'bus_entries': bus_entries,
        'sheets': sheets,
    }


# ---------------------------------------------------------------------------
# Net tracing via union-find on wire endpoints
# ---------------------------------------------------------------------------

EPSILON = 0.001  # mm — two endpoints are the same if closer than this


def pt_key(x, y, eps=EPSILON):
    """Snap a coordinate to a grid to handle floating point noise."""
    return (round(x / eps), round(y / eps))


def build_nets(wires, buses, labels):
    """
    Flood-fill net connectivity:
    - Each wire endpoint is a node.
    - Two nodes are in the same net if they share an endpoint.
    - Labels attach a net name to a node.
    Returns: list of net dicts {name, wire_indices, bus_indices}
             and per-wire net_id (index into nets list, -1 if unlabeled).
    """
    # Union-Find
    parent = {}

    def find(k):
        if k not in parent:
            parent[k] = k
        while parent[k] != k:
            parent[k] = parent[parent[k]]
            k = parent[k]
        return k

    def union(a, b):
        a, b = find(a), find(b)
        if a != b:
            parent[b] = a

    # Register all wire endpoints and union them
    all_wires = [(w, False) for w in wires] + [(b, True) for b in buses]
    for w, _ in all_wires:
        k1 = pt_key(w['x1'], w['y1'])
        k2 = pt_key(w['x2'], w['y2'])
        union(k1, k2)

    # Map each root -> set of label names touching it
    root_labels = {}
    for lbl in labels:
        k = pt_key(lbl['x'], lbl['y'])
        r = find(k)
        root_labels.setdefault(r, set()).add(lbl['text'])

    # Assign net names: use the label text, or generate anonymous name
    root_net = {}
    anon = [0]

    def get_net(root):
        if root not in root_net:
            names = root_labels.get(root, set())
            # Prefer the shortest name if multiple
            if names:
                name = sorted(names, key=len)[0]
            else:
                name = f'_net{anon[0]}'
                anon[0] += 1
            root_net[root] = name
        return root_net[root]

    # Build net objects
    net_index = {}  # name -> index
    nets = []       # list of {name, wire_indices, bus_indices, anim_group}

    def net_for(name):
        if name not in net_index:
            net_index[name] = len(nets)
            nets.append({'name': name, 'wire_indices': [], 'bus_indices': [],
                         'anim_group': net_anim_group(name)})
        return net_index[name]

    # Assign each wire to a net
    wire_net_ids = []
    for i, w in enumerate(wires):
        k1 = pt_key(w['x1'], w['y1'])
        r = find(k1)
        name = get_net(r)
        nid = net_for(name)
        nets[nid]['wire_indices'].append(i)
        wire_net_ids.append(nid)

    bus_net_ids = []
    for i, b in enumerate(buses):
        k1 = pt_key(b['x1'], b['y1'])
        r = find(k1)
        name = get_net(r)
        nid = net_for(name)
        nets[nid]['bus_indices'].append(i)
        bus_net_ids.append(nid)

    return nets, wire_net_ids, bus_net_ids


# ---------------------------------------------------------------------------
# Animation group: which emulator signal drives this net?
#
#  0 = address bus A0..A15       -> fade_cpu_rom (cart access)
#  1 = data bus D0..D7           -> fade_cpu_rom (generic bus)
#  2 = WRAM data bus MD0..MD7    -> fade_cpu_wram
#  3 = WRAM addr bus MA0..MA12   -> fade_cpu_wram
#  4 = PHI clock                 -> always-on pulse
#  5 = APU / audio SO1 SO2       -> fade_apu
#  6 = LCD pixel LD0 LD1 CP CPG  -> fade_ppu_vram
#  7 = IRQ / RES                 -> fade_irq_cpu
#  8 = power VCC VDD VEE VIN     -> always-on dim
# -1 = unlabeled / other
# ---------------------------------------------------------------------------

def net_anim_group(name):
    # Strip KiCad negation markers
    n = name.lstrip('~{').rstrip('}').upper()

    if re.match(r'^A\d+$', n):          return 0   # address bus
    if re.match(r'^D\d+$', n):          return 1   # data bus
    if re.match(r'^MD\d+$', n):         return 2   # WRAM data
    if re.match(r'^MA\d+$', n):         return 3   # WRAM addr
    if n == 'PHI':                       return 4   # clock
    if n in ('SO1', 'SO2', 'SPKOUT'):   return 5   # audio
    if n in ('LD0', 'LD1', 'CP', 'CPG', 'CPL', 'FR', 'S', 'ST',
             'VEE', 'V1', 'V2', 'V3', 'V4', 'V5'):
                                         return 6   # LCD signals + bias voltages
    if re.match(r'^P1[0-5](_\w+)?$', n): return 11  # joypad P10..P15
    if n in ('RES', 'RST'):             return 7   # reset/IRQ
    if n in ('VCC', 'VDD', 'VIN', 'DVDD', 'GND'):  return 8   # power
    if n in ('SCK', 'SIN', 'SOUT', 'SER'):
                                         return 9   # serial link
    if n in ('WR', 'RD', 'CS', 'MCS', 'MRD', 'MWR'):
                                         return 10  # bus control
    return -1


ANIM_GROUP_NAMES = [
    "addr_bus",    # 0
    "data_bus",    # 1
    "wram_data",   # 2
    "wram_addr",   # 3
    "clock",       # 4
    "audio",       # 5
    "lcd",         # 6
    "irq",         # 7
    "power",       # 8
    "serial",      # 9
    "bus_ctrl",    # 10
    "joypad",      # 11
]


# ---------------------------------------------------------------------------
# Signal group mapping (component ref -> display group)
# ---------------------------------------------------------------------------

# DMG main board component groups (used when prefix == 'HW')
SIGNAL_GROUPS_DMG = {
    'U1': 0,    # CPU (SM83 SoC)
    'U2': 1,    # WRAM bank 1
    'U3': 2,    # WRAM bank 2
    'P1': 3,    # Cartridge connector
    'X1': 4,    # Crystal/clock
    'J1': 5,    # Audio/headphone jack
    'J2': 5,    # Link connector
    'VR1': 6,   # Volume pot
    'SW1': 7,   # Power switch
    'BT1': 8,   # Battery
}

# DMG LCD board component groups (used when prefix == 'LCD')
SIGNAL_GROUPS_LCD = {
    'U1': 6,    # IR3E02 — LCD bias voltage generator
    'U2': 6,    # LH5076 — LCD row driver
    'U3': 6,    # LH5077 — LCD column driver
    'VR1': 6,   # contrast potentiometer
    'J2': 6,    # LCD ribbon connector (to row/col drivers)
    'J1': 11,   # main board connector (carries joypad + speaker)
    'A1': 6,    # ribbon cable to LCD panel
    'SW1': 11,  # SW1..SW8 — button switches (joypad)
    'SW2': 11,
    'SW3': 11,
    'SW4': 11,
    'SW5': 11,
    'SW6': 11,
    'SW7': 11,
    'SW8': 11,
    'DA1': 5,   # DAN215 diodes — speaker bridge
    'DA2': 5,
    'DA3': 11,  # DA3/DA4 — joypad diode matrix (button side)
    'DA4': 11,
    'LS1': 5,   # speaker
    'J3': 11,   # joypad sub-board connectors
    'J4': 11,
    'J5': 11,
}

def get_signal_group(ref, prefix='HW'):
    groups = SIGNAL_GROUPS_LCD if prefix.upper() == 'LCD' else SIGNAL_GROUPS_DMG
    for key, group in groups.items():
        if ref == key or ref.startswith(key):
            return group
    return -1


# ---------------------------------------------------------------------------
# C code generator
# ---------------------------------------------------------------------------

def escape_c_str(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')


def file_sha256(path):
    h = hashlib.sha256(Path(path).read_bytes())
    return h.hexdigest()[:16]


def emit_c(data, nets, wire_net_ids, bus_net_ids, output_c, output_h, source_files, prefix='HW'):
    """
    prefix: symbol prefix for this dataset, e.g. 'HW' (DMG) or 'LCD'.
    Generates <PREFIX>_COMPONENT_COUNT, <prefix>_components[], etc.
    The shared typedefs (HwComponent, HwWire, ...) and HW_ANIM_* defines are
    emitted only when prefix == 'HW', so secondary datasets just include the
    primary hw_schematic_data.h for the shared types.
    """
    src_comment = ', '.join(str(Path(s).name) for s in source_files)
    components = data['components']
    wires = data['wires']
    buses = data['buses']
    labels = data['labels']
    junctions = data['junctions']

    total_wires = len(wires) + len(buses)
    total_nets  = len(nets)
    is_primary  = (prefix.upper() == 'HW')
    UP          = prefix.upper()
    lo          = prefix.lower()
    h_basename  = Path(output_h).name

    gen_time = datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    src_hashes = '  '.join(
        f'{Path(s).name}:{file_sha256(s)}' for s in source_files
    )

    with open(output_h, 'w') as f:
        f.write(f"""\
/* {h_basename} -- AUTO-GENERATED by tools/kicad_extractor.py -- DO NOT EDIT */
/* Source: https://github.com/Gekkio/gb-schematics (CC-BY 4.0) */
/* Attribution: Joonas Javanainen (Gekkio), CC BY 4.0 */
/* Generated : {gen_time} */
/* Source    : {src_comment} */
/* SHA-256   : {src_hashes} */
/* Counts    : components={len(components)} wires={total_wires} nets={total_nets} labels={len(labels)} junctions={len(junctions)} */
#pragma once
#include <stdint.h>
""")
        if is_primary:
            f.write("""
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
 * 5=audio 6=lcd   7=irq        8=power      9=serial 10=bus_ctrl 11=joypad */
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
#define HW_ANIM_JOYPAD    11
#define HW_ANIM_GROUP_COUNT 12
""")
        else:
            f.write('#include "hw_schematic_data.h"\n')

        f.write(f"""
#define {UP}_COMPONENT_COUNT  {len(components)}
#define {UP}_WIRE_COUNT       {total_wires}
#define {UP}_NET_COUNT        {total_nets}
#define {UP}_LABEL_COUNT      {len(labels)}
#define {UP}_JUNCTION_COUNT   {len(junctions)}

extern const HwComponent {lo}_components[{UP}_COMPONENT_COUNT];
extern const HwWire      {lo}_wires[{UP}_WIRE_COUNT];
extern const HwNet       {lo}_nets[{UP}_NET_COUNT];
extern const HwLabel     {lo}_labels[{UP}_LABEL_COUNT];
extern const HwJunction  {lo}_junctions[{UP}_JUNCTION_COUNT];
""")

    c_basename = Path(output_c).name
    with open(output_c, 'w') as f:
        f.write(f"""\
/* {c_basename} -- AUTO-GENERATED by tools/kicad_extractor.py -- DO NOT EDIT */
/* Source: https://github.com/Gekkio/gb-schematics (CC-BY 4.0) */
/* Attribution: Joonas Javanainen (Gekkio), CC BY 4.0 */
/* Generated : {gen_time} */
/* Source    : {src_comment} */
#include "{h_basename}"

const HwComponent {lo}_components[{UP}_COMPONENT_COUNT] = {{
""")
        for c in components:
            sg = get_signal_group(c['ref'], prefix)
            f.write(f'    {{ "{escape_c_str(c["ref"])}", "{escape_c_str(c["value"])}", '
                    f'{c["nx"]:.6f}f, {c["ny"]:.6f}f, '
                    f'{c["nw"]:.6f}f, {c["nh"]:.6f}f, {sg} }},\n')
        f.write('};\n\n')

        f.write(f'const HwWire {lo}_wires[{UP}_WIRE_COUNT] = {{\n')
        for i, w in enumerate(wires):
            nid = wire_net_ids[i]
            f.write(f'    {{ {w["nx1"]:.6f}f, {w["ny1"]:.6f}f, '
                    f'{w["nx2"]:.6f}f, {w["ny2"]:.6f}f, 0, {nid} }},\n')
        for i, b in enumerate(buses):
            nid = bus_net_ids[i]
            f.write(f'    {{ {b["nx1"]:.6f}f, {b["ny1"]:.6f}f, '
                    f'{b["nx2"]:.6f}f, {b["ny2"]:.6f}f, 1, {nid} }},\n')
        f.write('};\n\n')

        f.write(f'const HwNet {lo}_nets[{UP}_NET_COUNT] = {{\n')
        for net in nets:
            f.write(f'    {{ "{escape_c_str(net["name"])}", {net["anim_group"]} }},\n')
        f.write('};\n\n')

        f.write(f'const HwLabel {lo}_labels[{UP}_LABEL_COUNT] = {{\n')
        for lbl in labels:
            f.write(f'    {{ "{escape_c_str(lbl["text"])}", '
                    f'{lbl["nx"]:.6f}f, {lbl["ny"]:.6f}f, {lbl["angle"]:.1f}f }},\n')
        f.write('};\n\n')

        f.write(f'const HwJunction {lo}_junctions[{UP}_JUNCTION_COUNT] = {{\n')
        for j in junctions:
            f.write(f'    {{ {j["nx"]:.6f}f, {j["ny"]:.6f}f }},\n')
        f.write('};\n')


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description='Extract KiCad schematic for GB HW viewer')
    ap.add_argument('schematics', nargs='+', help='.kicad_sch files (main first)')
    ap.add_argument('--output-c', default='hw_schematic/hw_schematic_data.c')
    ap.add_argument('--output-h', default='hw_schematic/hw_schematic_data.h')
    ap.add_argument('--json', default='data/dmg_components.json')
    ap.add_argument('--prefix', default='HW',
                    help='Symbol prefix for dataset (e.g. HW for DMG, LCD for lcd board). '
                         'Secondary datasets (non-HW) include hw_schematic_data.h for shared types.')
    args = ap.parse_args()

    print(f'Parsing {len(args.schematics)} schematic(s)...')
    schematics = []
    for path in args.schematics:
        print(f'  {path}')
        sch = parse_schematic(path)
        schematics.append(sch)
        print(f'    components={len(sch["components"])} wires={len(sch["wires"])} '
              f'buses={len(sch["buses"])} labels={len(sch["labels"])} '
              f'junctions={len(sch["junctions"])}')

    # Use only the main schematic (sub-sheets have different coordinate spaces)
    data = schematics[0]
    emitted_sources = [args.schematics[0]]

    print(f'\nTotal (main sheet only):')
    print(f'  components : {len(data["components"])}')
    print(f'  wires      : {len(data["wires"])}')
    print(f'  buses      : {len(data["buses"])}')
    print(f'  labels     : {len(data["labels"])}')
    print(f'  junctions  : {len(data["junctions"])}')

    # Net tracing
    print('\nTracing nets...')
    nets, wire_net_ids, bus_net_ids = build_nets(
        data['wires'], data['buses'], data['labels'])
    print(f'  nets found : {len(nets)}')

    # Print named nets with anim groups
    named = [(n['name'], n['anim_group']) for n in nets if not n['name'].startswith('_net')]
    named.sort()
    print(f'  named nets : {len(named)}')
    for name, ag in named:
        gname = ANIM_GROUP_NAMES[ag] if 0 <= ag < len(ANIM_GROUP_NAMES) else 'none'
        print(f'    {name:20} anim={gname}')

    # Write JSON
    export = dict(data)
    export['nets'] = nets
    Path(args.json).parent.mkdir(exist_ok=True)
    with open(args.json, 'w') as f:
        json.dump(export, f, indent=2)
    print(f'\nJSON written to {args.json}')

    # Write C
    Path(args.output_c).parent.mkdir(exist_ok=True)
    emit_c(data, nets, wire_net_ids, bus_net_ids,
           args.output_c, args.output_h, emitted_sources, prefix=args.prefix)
    print(f'C data written to {args.output_c} / {args.output_h}')

    print('\nComponents:')
    for c in data['components']:
        sg = get_signal_group(c['ref'], args.prefix)
        print(f'  {c["ref"]:8} {c["value"]:25} nx={c["nx"]:.4f} ny={c["ny"]:.4f} group={sg}')


if __name__ == '__main__':
    main()
