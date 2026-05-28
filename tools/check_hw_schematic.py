#!/usr/bin/env python3
"""
check_hw_schematic.py — Fase A+B validation for the HW Schematic panel.

Fase A — validates hw_schematic_data.h / hw_schematic_data.c (generated):
  1. Expected counts in plausible range for DMG-CPU-06.
  2. A4 aspect ratio preserved (297x210mm).
  3. All wire/bus net_id values in [0, HW_NET_COUNT) or -1.
  4. All component coordinates in [0,1].
  5. Address bus A0..A15 with correct anim_group.
  6. Data bus D0..D7 with correct anim_group.
  7. Key control nets (RD, WR, CS, PHI, RES).
  8. Key components (U1=CPU, U2/U3=WRAM, P1=cart, X1=crystal).
  9. All labels in [0,1] with non-empty text.
 10. Labels-to-net coverage.
 11. WRAM bus MA0..MA12 / MD0..MD7.
 12. Generation metadata in header.
 13. A4 aspect ratio documented.

Fase B — validates hw_schematic_map.c (hand-written semantic mapping):
 14. All net_ids in hw_net_map[] are in [0, HW_NET_COUNT).
 15. No duplicate net_ids in hw_net_map[].
 16. All component_ids in hw_component_map[] are in [0, HW_COMPONENT_COUNT).
 17. No duplicate component_ids in hw_component_map[].
 18. A0..A15 all mapped with kind=ADDR and matching bit.
 19. D0..D7 all mapped with kind=DATA and matching bit.
 20. MA0..MA12 all mapped with kind=WRAM_ADDR.
 21. MD0..MD7 all mapped with kind=WRAM_DATA.
 22. Key control nets mapped (~{RD}, ~{WR}, ~{CS}, PHI, ~{RES}).
 23. Key components mapped (U1=CPU, U2/U3=WRAM, P1=cart, X1=clock, J2=lcd).
 24. Coverage: fraction of named nets that have a semantic mapping.

Usage:
    python3 tools/check_hw_schematic.py          (uses default paths)
    python3 tools/check_hw_schematic.py --h hw_schematic/hw_schematic_data.h \
                                        --c hw_schematic/hw_schematic_data.c \
                                        --map hw_schematic/hw_schematic_map.c
"""

import re
import sys
import argparse
from pathlib import Path

PASS_TAG = "  PASS  "
FAIL_TAG = "  FAIL  "
INFO_TAG = "  INFO  "
WARN_TAG = "  WARN  "

failures = 0


def ok(msg):
    print(f"{PASS_TAG}{msg}")


def fail(msg):
    global failures
    failures += 1
    print(f"{FAIL_TAG}{msg}")


def info(msg):
    print(f"{INFO_TAG}{msg}")


def warn(msg):
    print(f"{WARN_TAG}{msg}")


def check(cond, pass_msg, fail_msg=None):
    if cond:
        ok(pass_msg)
    else:
        fail(fail_msg or pass_msg)


# ---------------------------------------------------------------------------
# Parse the generated .h header to extract #define values
# ---------------------------------------------------------------------------

def parse_defines(h_text):
    defines = {}
    for m in re.finditer(r'#define\s+(\w+)\s+(-?\d+)', h_text):
        defines[m.group(1)] = int(m.group(2))
    return defines


# ---------------------------------------------------------------------------
# Parse the generated .c file to extract arrays
# ---------------------------------------------------------------------------

def parse_components(c_text):
    """Extract list of (ref, value, nx, ny, nw, nh, signal_group)."""
    pattern = re.compile(
        r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*'
        r'([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*'
        r'([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*(-?\d+)\s*\}'
    )
    return [
        {'ref': m.group(1), 'value': m.group(2),
         'nx': float(m.group(3)), 'ny': float(m.group(4)),
         'nw': float(m.group(5)), 'nh': float(m.group(6)),
         'sg': int(m.group(7))}
        for m in pattern.finditer(c_text)
    ]


def parse_wires(c_text):
    """Extract list of (nx1, ny1, nx2, ny2, is_bus, net_id)."""
    pattern = re.compile(
        r'\{\s*([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*'
        r'([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*'
        r'(\d+)\s*,\s*(-?\d+)\s*\}'
    )
    return [
        {'nx1': float(m.group(1)), 'ny1': float(m.group(2)),
         'nx2': float(m.group(3)), 'ny2': float(m.group(4)),
         'is_bus': int(m.group(5)), 'net_id': int(m.group(6))}
        for m in pattern.finditer(c_text)
    ]


def parse_nets(c_text):
    """Extract list of (name, anim_group) from hw_nets array."""
    # Find the hw_nets block
    block_match = re.search(
        r'const HwNet hw_nets\[.*?\]\s*=\s*\{(.*?)\};',
        c_text, re.DOTALL
    )
    if not block_match:
        return []
    block = block_match.group(1)
    pattern = re.compile(r'\{\s*"([^"]*)"\s*,\s*(-?\d+)\s*\}')
    return [{'name': m.group(1), 'anim_group': int(m.group(2))}
            for m in pattern.finditer(block)]


def parse_labels(c_text):
    """Extract list of (text, nx, ny, angle)."""
    block_match = re.search(
        r'const HwLabel hw_labels\[.*?\]\s*=\s*\{(.*?)\};',
        c_text, re.DOTALL
    )
    if not block_match:
        return []
    block = block_match.group(1)
    pattern = re.compile(
        r'\{\s*"([^"]*)"\s*,\s*([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*([\d.-]+)f\s*\}'
    )
    return [{'text': m.group(1),
             'nx': float(m.group(2)), 'ny': float(m.group(3)),
             'angle': float(m.group(4))}
            for m in pattern.finditer(block)]


def parse_junctions(c_text):
    block_match = re.search(
        r'const HwJunction hw_junctions\[.*?\]\s*=\s*\{(.*?)\};',
        c_text, re.DOTALL
    )
    if not block_match:
        return []
    block = block_match.group(1)
    pattern = re.compile(r'\{\s*([\d.]+)f\s*,\s*([\d.]+)f\s*\}')
    return [{'nx': float(m.group(1)), 'ny': float(m.group(2))}
            for m in pattern.finditer(block)]


# ---------------------------------------------------------------------------
# Individual checks
# ---------------------------------------------------------------------------

def test_metadata(h_text):
    print("[1] Generation metadata")
    check('AUTO-GENERATED' in h_text or 'auto-generated' in h_text.lower() or
          'kicad_extractor' in h_text or 'kicad_sch' in h_text,
          "header contains generation comment (AUTO-GENERATED / kicad_extractor)")
    check('CC-BY' in h_text or 'CC BY' in h_text,
          "header contains attribution / license (CC BY)")
    check('Gekkio' in h_text or 'Javanainen' in h_text or 'gb-schematics' in h_text,
          "header credits source (Gekkio / gb-schematics)")
    check('Generated' in h_text or 'generated' in h_text,
          "header has Generated timestamp field")
    check('Source' in h_text and 'kicad_sch' in h_text,
          "header has Source field referencing .kicad_sch")
    check('Counts' in h_text or 'components=' in h_text,
          "header has Counts metadata line")
    check('297' in h_text and '210' in h_text,
          "header references A4 dimensions (297x210mm)")


def test_counts(defines):
    print("[2] Counts (plausible range for DMG-CPU-06)")
    n_comp  = defines.get('HW_COMPONENT_COUNT', -1)
    n_wire  = defines.get('HW_WIRE_COUNT', -1)
    n_net   = defines.get('HW_NET_COUNT', -1)
    n_label = defines.get('HW_LABEL_COUNT', -1)
    n_junct = defines.get('HW_JUNCTION_COUNT', -1)

    check(5 <= n_comp <= 50,
          f"HW_COMPONENT_COUNT={n_comp} (expected 5-50)",
          f"HW_COMPONENT_COUNT={n_comp} out of expected range 5-50")
    check(100 <= n_wire <= 2000,
          f"HW_WIRE_COUNT={n_wire} (expected 100-2000)",
          f"HW_WIRE_COUNT={n_wire} out of expected range 100-2000")
    check(20 <= n_net <= 300,
          f"HW_NET_COUNT={n_net} (expected 20-300)",
          f"HW_NET_COUNT={n_net} out of expected range 20-300")
    check(50 <= n_label <= 500,
          f"HW_LABEL_COUNT={n_label} (expected 50-500)",
          f"HW_LABEL_COUNT={n_label} out of expected range 50-500")
    check(0 <= n_junct <= 200,
          f"HW_JUNCTION_COUNT={n_junct} (expected 0-200)",
          f"HW_JUNCTION_COUNT={n_junct} out of expected range 0-200")

    return n_comp, n_wire, n_net, n_label, n_junct


def test_anim_defines(h_text):
    print("[3] Animation group defines")
    expected = [
        'HW_ANIM_ADDR', 'HW_ANIM_DATA', 'HW_ANIM_WRAM_DATA', 'HW_ANIM_WRAM_ADDR',
        'HW_ANIM_CLOCK', 'HW_ANIM_AUDIO', 'HW_ANIM_LCD', 'HW_ANIM_IRQ',
        'HW_ANIM_POWER', 'HW_ANIM_SERIAL', 'HW_ANIM_BUS_CTRL', 'HW_ANIM_GROUP_COUNT',
    ]
    missing = [d for d in expected if d not in h_text]
    check(not missing,
          f"all {len(expected)} HW_ANIM_* defines present",
          f"missing defines: {missing}")


def test_components(components, n_comp_expected):
    print("[4] Components")

    check(len(components) == n_comp_expected,
          f"parsed {len(components)} components == HW_COMPONENT_COUNT={n_comp_expected}",
          f"parsed {len(components)} components != HW_COMPONENT_COUNT={n_comp_expected}")

    out_of_range = [(c['ref'], c['nx'], c['ny'])
                    for c in components
                    if not (0.0 <= c['nx'] <= 1.0 and 0.0 <= c['ny'] <= 1.0)]
    check(not out_of_range,
          f"all {len(components)} components have nx,ny in [0,1]",
          f"{len(out_of_range)} components have coordinates outside [0,1]: {out_of_range[:3]}")

    # Key components by ref prefix
    refs = {c['ref'] for c in components}
    ref_vals = {c['ref']: c['value'] for c in components}

    key_refs = {
        'U1': 'CPU (DMG SoC)',
        'P1': 'cartridge connector',
        'X1': 'crystal oscillator',
    }
    for ref, role in key_refs.items():
        found = any(r == ref or r.startswith(ref) for r in refs)
        check(found,
              f"component {ref} ({role}) present — value={ref_vals.get(ref, '?')}",
              f"component {ref} ({role}) NOT found in schematic")

    wram_refs = [r for r in refs if r.startswith('U') and r != 'U1']
    check(len(wram_refs) >= 1,
          f"WRAM chip(s) found: {sorted(wram_refs)}",
          "no WRAM chip (U2/U3/…) found")

    info(f"all component refs: {sorted(refs)}")


def test_wires(wires, n_wire_expected, n_net):
    print("[5] Wires and net_id range")

    check(len(wires) == n_wire_expected,
          f"parsed {len(wires)} wires == HW_WIRE_COUNT={n_wire_expected}",
          f"parsed {len(wires)} wires != HW_WIRE_COUNT={n_wire_expected}")

    bad_range = [w for w in wires if w['net_id'] != -1 and not (0 <= w['net_id'] < n_net)]
    check(not bad_range,
          f"all wire net_ids in [-1, {n_net})",
          f"{len(bad_range)} wires have net_id out of range: {bad_range[:3]}")

    out_of_range = [w for w in wires
                    if not (0.0 <= w['nx1'] <= 1.1 and 0.0 <= w['ny1'] <= 1.1 and
                            0.0 <= w['nx2'] <= 1.1 and 0.0 <= w['ny2'] <= 1.1)]
    check(not out_of_range,
          f"all wire endpoints within [0,1.1] (A4 bounds + margin)",
          f"{len(out_of_range)} wires have endpoints outside bounds")

    buses = [w for w in wires if w['is_bus']]
    info(f"{len(buses)} bus segments, {len(wires) - len(buses)} wire segments")


def test_nets(nets, n_net_expected):
    print("[6] Nets")

    check(len(nets) == n_net_expected,
          f"parsed {len(nets)} nets == HW_NET_COUNT={n_net_expected}",
          f"parsed {len(nets)} nets != HW_NET_COUNT={n_net_expected}")

    empty_names = [i for i, n in enumerate(nets) if not n['name']]
    check(not empty_names,
          "no net has empty name",
          f"{len(empty_names)} nets have empty names at indices: {empty_names[:5]}")

    bad_anim = [n for n in nets if not (-1 <= n['anim_group'] <= 10)]
    check(not bad_anim,
          "all anim_group values in [-1, 10]",
          f"{len(bad_anim)} nets have invalid anim_group: {bad_anim[:3]}")

    net_names = {n['name'] for n in nets}
    named = [n for n in nets if not n['name'].startswith('_net')]
    info(f"{len(named)}/{len(nets)} nets have semantic names")

    return net_names, {n['name']: n['anim_group'] for n in nets}


def test_address_bus(net_names, net_anim):
    print("[7] Address bus A0..A15")
    ADDR_GROUP = 0
    missing_addr = []
    wrong_group = []
    for i in range(16):
        name = f'A{i}'
        if name not in net_names:
            missing_addr.append(name)
        elif net_anim.get(name) != ADDR_GROUP:
            wrong_group.append((name, net_anim.get(name)))
    check(not missing_addr,
          f"all A0..A15 present ({16} nets)",
          f"missing address nets: {missing_addr}")
    check(not wrong_group,
          "all A0..A15 have anim_group=ADDR(0)",
          f"wrong anim_group on: {wrong_group}")


def test_data_bus(net_names, net_anim):
    print("[8] Data bus D0..D7")
    DATA_GROUP = 1
    missing_data = []
    wrong_group = []
    for i in range(8):
        name = f'D{i}'
        if name not in net_names:
            missing_data.append(name)
        elif net_anim.get(name) != DATA_GROUP:
            wrong_group.append((name, net_anim.get(name)))
    check(not missing_data,
          f"all D0..D7 present ({8} nets)",
          f"missing data nets: {missing_data}")
    check(not wrong_group,
          "all D0..D7 have anim_group=DATA(1)",
          f"wrong anim_group on: {wrong_group}")


def test_control_nets(net_names, net_anim):
    print("[9] Control nets")
    BUS_CTRL_GROUP = 10

    # These must exist in the DMG schematic
    must_have = ['PHI']
    should_have = ['~{RD}', '~{WR}', '~{CS}', '~{MCS}', 'RES']

    # Also accept bare names (without negation markers)
    def net_exists(candidates):
        return any(c in net_names for c in candidates)

    check(net_exists(['PHI']),
          "PHI (clock) net present",
          "PHI (clock) net MISSING")

    rd_ok = net_exists(['~{RD}', 'RD', '/RD'])
    check(rd_ok,
          "~{RD} (read strobe) net present",
          "~{RD}/RD read strobe net MISSING")

    wr_ok = net_exists(['~{WR}', 'WR', '/WR'])
    check(wr_ok,
          "~{WR} (write strobe) net present",
          "~{WR}/WR write strobe net MISSING")

    res_ok = net_exists(['~{RES}', 'RES', 'RST', '~{RESET}', 'RESET'])
    check(res_ok,
          "~{RES}/RES (reset) net present",
          "~{RES}/RES reset net MISSING — expected for IRQ/reset routing")

    # Report all control-group nets
    ctrl_nets = sorted(n for n, g in net_anim.items() if g == BUS_CTRL_GROUP)
    info(f"bus_ctrl nets ({len(ctrl_nets)}): {ctrl_nets}")


def test_labels(labels, n_label_expected):
    print("[10] Labels")

    check(len(labels) == n_label_expected,
          f"parsed {len(labels)} labels == HW_LABEL_COUNT={n_label_expected}",
          f"parsed {len(labels)} labels != HW_LABEL_COUNT={n_label_expected}")

    out_of_range = [(lbl['text'], lbl['nx'], lbl['ny'])
                    for lbl in labels
                    if not (0.0 <= lbl['nx'] <= 1.0 and 0.0 <= lbl['ny'] <= 1.0)]
    check(not out_of_range,
          f"all {len(labels)} labels have coordinates in [0,1]",
          f"{len(out_of_range)} labels outside [0,1]: {out_of_range[:3]}")

    empty = [l for l in labels if not l['text']]
    check(not empty,
          "no label has empty text",
          f"{len(empty)} labels have empty text")


def test_label_net_coverage(labels, net_names):
    print("[11] Label-to-net coverage")
    # Bus-group labels (KiCad hierarchical bus annotations) are not discrete nets.
    # They are curly-brace grouped labels like "{LcdSignals}" or range labels like
    # "P1[0..3]". Skip them — they have no corresponding net entry.
    def is_bus_group_label(text):
        return text.startswith('{') or ('[' in text and '..' in text)

    label_texts = {lbl['text'] for lbl in labels
                   if lbl['text'] and not is_bus_group_label(lbl['text'])}
    bus_group_labels = {lbl['text'] for lbl in labels
                        if lbl['text'] and is_bus_group_label(lbl['text'])}
    if bus_group_labels:
        info(f"{len(bus_group_labels)} bus-group/hierarchical labels skipped "
             f"(not discrete nets): {sorted(bus_group_labels)}")

    # Named nets that are not anonymous
    named_nets = {n for n in net_names if not n.startswith('_net')}
    # Every named net should have at least one label with that text
    nets_without_label = named_nets - label_texts
    if nets_without_label:
        info(f"{len(nets_without_label)} named nets have no matching label "
             f"(may be OK if generated from wire names): "
             f"{sorted(nets_without_label)[:10]}")
    else:
        ok(f"all {len(named_nets)} named nets have a matching label")

    # Every non-bus-group label should map to a net
    labels_without_net = label_texts - net_names
    check(not labels_without_net,
          f"all signal labels resolve to a net",
          f"{len(labels_without_net)} labels have no matching net: "
          f"{sorted(labels_without_net)[:10]}")


def test_wram_nets(net_names, net_anim):
    print("[12] WRAM internal bus (MA0..MA12, MD0..MD7)")
    WRAM_ADDR_GROUP = 3
    WRAM_DATA_GROUP = 2

    missing_ma = [f'MA{i}' for i in range(13) if f'MA{i}' not in net_names]
    missing_md = [f'MD{i}' for i in range(8) if f'MD{i}' not in net_names]

    check(not missing_ma,
          f"all MA0..MA12 present (13 nets)",
          f"missing WRAM addr nets: {missing_ma}")
    check(not missing_md,
          f"all MD0..MD7 present (8 nets)",
          f"missing WRAM data nets: {missing_md}")

    wrong_ma = [(n, net_anim[n]) for n in [f'MA{i}' for i in range(13)]
                if n in net_anim and net_anim[n] != WRAM_ADDR_GROUP]
    wrong_md = [(n, net_anim[n]) for n in [f'MD{i}' for i in range(8)]
                if n in net_anim and net_anim[n] != WRAM_DATA_GROUP]

    check(not wrong_ma,
          "all MA0..MA12 have anim_group=WRAM_ADDR(3)",
          f"wrong anim_group on MA nets: {wrong_ma}")
    check(not wrong_md,
          "all MD0..MD7 have anim_group=WRAM_DATA(2)",
          f"wrong anim_group on MD nets: {wrong_md}")


# ---------------------------------------------------------------------------
# Parse hw_schematic_map.c
# ---------------------------------------------------------------------------

def parse_net_map(map_text):
    """Return list of dicts from hw_net_map[] in hw_schematic_map.c."""
    block = re.search(
        r'const HwNetSemantic hw_net_map\[\]\s*=\s*\{(.*?)\};',
        map_text, re.DOTALL
    )
    if not block:
        return []
    # Each entry: { net_id, "sch_name", "canon_name", KIND, bit, active_low, CONF }
    pat = re.compile(
        r'\{\s*(-?\d+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*'
        r'(\w+)\s*,\s*(-?\d+)\s*,\s*(true|false)\s*,\s*(\w+)\s*\}'
    )
    result = []
    for m in pat.finditer(block.group(1)):
        result.append({
            'net_id':    int(m.group(1)),
            'sch_name':  m.group(2),
            'canon':     m.group(3),
            'kind':      m.group(4),
            'bit':       int(m.group(5)),
            'active_low': m.group(6) == 'true',
            'conf':      m.group(7),
        })
    return result


def parse_component_map(map_text):
    """Return list of dicts from hw_component_map[] in hw_schematic_map.c."""
    block = re.search(
        r'const HwComponentSemantic hw_component_map\[\]\s*=\s*\{(.*?)\};',
        map_text, re.DOTALL
    )
    if not block:
        return []
    pat = re.compile(
        r'\{\s*(-?\d+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*'
        r'(\w+)\s*,\s*"([^"]*)"\s*,\s*(\w+)\s*\}'
    )
    result = []
    for m in pat.finditer(block.group(1)):
        result.append({
            'component_id': int(m.group(1)),
            'ref':          m.group(2),
            'value':        m.group(3),
            'kind':         m.group(4),
            'owner':        m.group(5),
            'conf':         m.group(6),
        })
    return result


# ---------------------------------------------------------------------------
# Fase B tests
# ---------------------------------------------------------------------------

def test_map_net_ids(net_map, n_net):
    print("[14] hw_net_map[] — net_id range")
    bad = [e for e in net_map if not (0 <= e['net_id'] < n_net)]
    check(not bad,
          f"all {len(net_map)} net_map entries have net_id in [0, {n_net})",
          f"{len(bad)} entries have out-of-range net_id: {[e['net_id'] for e in bad]}")

    ids = [e['net_id'] for e in net_map]
    dups = [nid for nid in set(ids) if ids.count(nid) > 1]
    check(not dups,
          "no duplicate net_ids in hw_net_map[]",
          f"duplicate net_ids: {dups}")


def test_map_component_ids(comp_map, n_comp):
    print("[15] hw_component_map[] — component_id range")
    bad = [e for e in comp_map if not (0 <= e['component_id'] < n_comp)]
    check(not bad,
          f"all {len(comp_map)} component_map entries have component_id in [0, {n_comp})",
          f"{len(bad)} entries have out-of-range component_id: {bad}")

    ids = [e['component_id'] for e in comp_map]
    dups = [cid for cid in set(ids) if ids.count(cid) > 1]
    check(not dups,
          "no duplicate component_ids in hw_component_map[]",
          f"duplicate component_ids: {dups}")


def test_map_addr_bus(net_map, nets):
    print("[16] hw_net_map[] — address bus A0..A15")
    net_id_by_name = {n['name']: i for i, n in enumerate(nets)}
    missing, wrong_kind, wrong_bit = [], [], []
    for bit in range(16):
        name = f'A{bit}'
        entries = [e for e in net_map if e['sch_name'] == name]
        if not entries:
            missing.append(name)
            continue
        e = entries[0]
        if e['kind'] != 'HW_SIG_ADDR':
            wrong_kind.append((name, e['kind']))
        if e['bit'] != bit:
            wrong_bit.append((name, e['bit'], bit))
    check(not missing,
          "A0..A15 all in hw_net_map[]",
          f"missing from map: {missing}")
    check(not wrong_kind,
          "A0..A15 all have kind=HW_SIG_ADDR",
          f"wrong kind: {wrong_kind}")
    check(not wrong_bit,
          "A0..A15 all have correct bit index",
          f"wrong bit: {wrong_bit}")


def test_map_data_bus(net_map):
    print("[17] hw_net_map[] — data bus D0..D7")
    missing, wrong_kind, wrong_bit = [], [], []
    for bit in range(8):
        name = f'D{bit}'
        entries = [e for e in net_map if e['sch_name'] == name]
        if not entries:
            missing.append(name)
            continue
        e = entries[0]
        if e['kind'] != 'HW_SIG_DATA':
            wrong_kind.append((name, e['kind']))
        if e['bit'] != bit:
            wrong_bit.append((name, e['bit'], bit))
    check(not missing,
          "D0..D7 all in hw_net_map[]",
          f"missing from map: {missing}")
    check(not wrong_kind,
          "D0..D7 all have kind=HW_SIG_DATA",
          f"wrong kind: {wrong_kind}")
    check(not wrong_bit,
          "D0..D7 all have correct bit index",
          f"wrong bit: {wrong_bit}")


def test_map_wram_bus(net_map):
    print("[18] hw_net_map[] — WRAM bus MA0..MA12 / MD0..MD7")
    missing_ma = [f'MA{i}' for i in range(13)
                  if not any(e['sch_name'] == f'MA{i}' for e in net_map)]
    missing_md = [f'MD{i}' for i in range(8)
                  if not any(e['sch_name'] == f'MD{i}' for e in net_map)]
    wrong_kind_ma = [(e['sch_name'], e['kind']) for e in net_map
                     if re.match(r'^MA\d+$', e['sch_name'])
                     and e['kind'] != 'HW_SIG_WRAM_ADDR']
    wrong_kind_md = [(e['sch_name'], e['kind']) for e in net_map
                     if re.match(r'^MD\d+$', e['sch_name'])
                     and e['kind'] != 'HW_SIG_WRAM_DATA']
    check(not missing_ma,
          "MA0..MA12 all in hw_net_map[]",
          f"missing from map: {missing_ma}")
    check(not missing_md,
          "MD0..MD7 all in hw_net_map[]",
          f"missing from map: {missing_md}")
    check(not wrong_kind_ma,
          "MA0..MA12 all have kind=HW_SIG_WRAM_ADDR",
          f"wrong kind: {wrong_kind_ma}")
    check(not wrong_kind_md,
          "MD0..MD7 all have kind=HW_SIG_WRAM_DATA",
          f"wrong kind: {wrong_kind_md}")


def test_map_control_nets(net_map):
    print("[19] hw_net_map[] — control nets")
    def mapped(sch_name_candidates, expected_kind):
        for e in net_map:
            if e['sch_name'] in sch_name_candidates:
                if e['kind'] == expected_kind:
                    return True, e
                return False, e
        return False, None

    ok_rd,  e = mapped(['~{RD}', 'RD'],   'HW_SIG_CTRL_RD')
    ok_wr,  e = mapped(['~{WR}', 'WR'],   'HW_SIG_CTRL_WR')
    ok_phi, e = mapped(['PHI'],            'HW_SIG_CLOCK')
    ok_res, e = mapped(['~{RES}', 'RES'], 'HW_SIG_RESET')
    ok_cs,  e = mapped(['~{CS}', 'CS'],   'HW_SIG_CTRL_CS')

    check(ok_rd,  "~{RD} mapped as HW_SIG_CTRL_RD",  "~{RD} not mapped as CTRL_RD")
    check(ok_wr,  "~{WR} mapped as HW_SIG_CTRL_WR",  "~{WR} not mapped as CTRL_WR")
    check(ok_phi, "PHI mapped as HW_SIG_CLOCK",       "PHI not mapped as CLOCK")
    check(ok_res, "~{RES} mapped as HW_SIG_RESET",    "~{RES} not mapped as RESET")
    check(ok_cs,  "~{CS} mapped as HW_SIG_CTRL_CS",   "~{CS} not mapped as CTRL_CS")

    # active_low check
    bad_al = [e for e in net_map
              if e['sch_name'] in ('~{RD}', '~{WR}', '~{CS}', '~{MCS}',
                                   '~{MRD}', '~{MWR}', '~{RES}')
              and not e['active_low']]
    check(not bad_al,
          "all ~{X} nets have active_low=true",
          f"active_low=false on: {[e['sch_name'] for e in bad_al]}")


def test_map_components(comp_map):
    print("[20] hw_component_map[] — key components")
    def mapped_as(ref, kind):
        for e in comp_map:
            if e['ref'] == ref:
                return e['kind'] == kind, e
        return False, None

    ok_u1, _ = mapped_as('U1', 'HW_COMP_CPU')
    ok_u2, _ = mapped_as('U2', 'HW_COMP_WRAM')
    ok_u3, _ = mapped_as('U3', 'HW_COMP_WRAM')
    ok_p1, _ = mapped_as('P1', 'HW_COMP_CART')
    ok_x1, _ = mapped_as('X1', 'HW_COMP_TIMER_CLOCK')
    ok_j2, _ = mapped_as('J2', 'HW_COMP_PPU_LCD')

    check(ok_u1, "U1 (DMG CPU) mapped as HW_COMP_CPU",          "U1 not mapped as CPU")
    check(ok_u2, "U2 (WRAM) mapped as HW_COMP_WRAM",            "U2 not mapped as WRAM")
    check(ok_u3, "U3 (WRAM) mapped as HW_COMP_WRAM",            "U3 not mapped as WRAM")
    check(ok_p1, "P1 (cart) mapped as HW_COMP_CART",            "P1 not mapped as CART")
    check(ok_x1, "X1 (crystal) mapped as HW_COMP_TIMER_CLOCK",  "X1 not mapped as TIMER_CLOCK")
    check(ok_j2, "J2 (LCD conn) mapped as HW_COMP_PPU_LCD",     "J2 not mapped as PPU_LCD")


def test_map_coverage(net_map, nets):
    print("[21] hw_net_map[] — semantic coverage of named nets")
    named_nets = {n['name'] for n in nets if not n['name'].startswith('_net')}
    mapped_names = {e['sch_name'] for e in net_map}
    covered = named_nets & mapped_names
    unmapped = sorted(named_nets - mapped_names)
    pct = len(covered) * 100 // len(named_nets) if named_nets else 0
    check(pct >= 80,
          f"{len(covered)}/{len(named_nets)} named nets mapped ({pct}% ≥ 80%)",
          f"only {len(covered)}/{len(named_nets)} named nets mapped ({pct}% < 80%)")
    if unmapped:
        info(f"unmapped named nets ({len(unmapped)}): {unmapped}")
    else:
        ok("all named nets have a semantic mapping")


def test_aspect_ratio(h_text):
    print("[13] A4 aspect ratio")
    # The coordinate system comment should reference 297x210 (A4 landscape)
    has_297 = '297' in h_text
    has_210 = '210' in h_text
    check(has_297 and has_210,
          "header documents A4 dimensions (297x210mm)",
          "A4 dimensions not documented in header")

    # nx = x/297, ny = y/210 → ny must be scaled by 210/297 ≈ 0.707 vs nx
    # All wire/label/junction coordinates should be in [0,1] — already checked above
    # Additionally verify that the ratio is approximately right by checking
    # that the comment says 297x210 (not 210x297)
    if '297x210' in h_text:
        ok("header explicitly states 297x210 (landscape A4)")
    else:
        info("header does not say '297x210' explicitly — verify landscape orientation")


# ---------------------------------------------------------------------------
# Summary report
# ---------------------------------------------------------------------------

def print_summary(defines, components, nets, net_names):
    print()
    print("Summary:")
    print(f"  HW_COMPONENT_COUNT  : {defines.get('HW_COMPONENT_COUNT')}")
    print(f"  HW_WIRE_COUNT       : {defines.get('HW_WIRE_COUNT')}")
    print(f"  HW_NET_COUNT        : {defines.get('HW_NET_COUNT')}")
    print(f"  HW_LABEL_COUNT      : {defines.get('HW_LABEL_COUNT')}")
    print(f"  HW_JUNCTION_COUNT   : {defines.get('HW_JUNCTION_COUNT')}")

    print()
    print("Net coverage by anim_group:")
    group_names = {
        0: 'addr_bus', 1: 'data_bus', 2: 'wram_data', 3: 'wram_addr',
        4: 'clock', 5: 'audio', 6: 'lcd', 7: 'irq',
        8: 'power', 9: 'serial', 10: 'bus_ctrl',
    }
    from collections import Counter
    counts = Counter(n['anim_group'] for n in nets)
    for g, gname in sorted(group_names.items()):
        print(f"    [{g:2}] {gname:12} : {counts.get(g, 0)} nets")
    print(f"    [{-1:2}] {'unlabeled':12} : {counts.get(-1, 0)} nets")

    print()
    print("Components:")
    for c in components:
        print(f"    {c['ref']:6} {c['value']:25} nx={c['nx']:.4f} ny={c['ny']:.4f} sg={c['sg']}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description='Validate HW Schematic generated data (Fase A)')
    ap.add_argument('--h', default='hw_schematic/hw_schematic_data.h',
                    help='path to hw_schematic_data.h')
    ap.add_argument('--c', default='hw_schematic/hw_schematic_data.c',
                    help='path to hw_schematic_data.c')
    ap.add_argument('--map', default='hw_schematic/hw_schematic_map.c',
                    help='path to hw_schematic_map.c (Fase B)')
    ap.add_argument('--summary', action='store_true',
                    help='print summary even if all tests pass')
    args = ap.parse_args()

    h_path   = Path(args.h)
    c_path   = Path(args.c)
    map_path = Path(args.map)

    if not h_path.exists():
        print(f"ERROR: {h_path} not found. Run kicad_extractor.py first.")
        sys.exit(2)
    if not c_path.exists():
        print(f"ERROR: {c_path} not found. Run kicad_extractor.py first.")
        sys.exit(2)

    h_text   = h_path.read_text(encoding='utf-8')
    c_text   = c_path.read_text(encoding='utf-8')
    map_text = map_path.read_text(encoding='utf-8') if map_path.exists() else None

    print("HW Schematic data validation (Fase A+B)")
    print(f"  .h   : {h_path}")
    print(f"  .c   : {c_path}")
    print(f"  .map : {map_path}" + (" (not found — Fase B skipped)" if not map_path.exists() else ""))
    print()

    defines = parse_defines(h_text)
    components = parse_components(c_text)
    wires = parse_wires(c_text)
    nets = parse_nets(c_text)
    labels = parse_labels(c_text)
    junctions = parse_junctions(c_text)

    test_metadata(h_text)
    print()

    n_comp, n_wire, n_net, n_label, n_junct = test_counts(defines)
    print()

    test_anim_defines(h_text)
    print()

    test_components(components, n_comp)
    print()

    test_wires(wires, n_wire, n_net)
    print()

    net_names, net_anim = test_nets(nets, n_net)
    print()

    test_address_bus(net_names, net_anim)
    print()

    test_data_bus(net_names, net_anim)
    print()

    test_control_nets(net_names, net_anim)
    print()

    test_labels(labels, n_label)
    print()

    test_label_net_coverage(labels, net_names)
    print()

    test_wram_nets(net_names, net_anim)
    print()

    test_aspect_ratio(h_text)
    print()

    # ── Fase B — semantic mapping ────────────────────────────────────────────
    if map_text is not None:
        print("── Fase B: Semantic mapping ──────────────────────────────────────────")
        print()

        net_map  = parse_net_map(map_text)
        comp_map = parse_component_map(map_text)

        info(f"hw_net_map entries parsed: {len(net_map)}")
        info(f"hw_component_map entries parsed: {len(comp_map)}")
        print()

        test_map_net_ids(net_map, n_net)
        print()

        test_map_component_ids(comp_map, n_comp)
        print()

        test_map_addr_bus(net_map, nets)
        print()

        test_map_data_bus(net_map)
        print()

        test_map_wram_bus(net_map)
        print()

        test_map_control_nets(net_map)
        print()

        test_map_components(comp_map)
        print()

        test_map_coverage(net_map, nets)
        print()
    else:
        info("hw_schematic_map.c not found — skipping Fase B tests")
        print()

    if args.summary or failures > 0:
        print_summary(defines, components, nets, net_names)
        print()

    if failures == 0:
        print("All tests passed.")
    else:
        print(f"{failures} test(s) FAILED.")

    sys.exit(1 if failures > 0 else 0)


if __name__ == '__main__':
    main()
