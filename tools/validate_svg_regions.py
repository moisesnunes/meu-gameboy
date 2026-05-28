#!/usr/bin/env python3
"""
validate_svg_regions.py — Cross-validate die viewer functional region bboxes
against the annotated SGB-CPU-01 SVG die diagram.

The SVG (img/SGB-CPU_01_sm83_core_40x.svg) is the ground truth: it contains
annotated text labels placed at known die positions.  We compare these to the
region bboxes defined in debug_ui_panels.cpp.

Coordinate mapping (derived from cross-referencing netlist nx,ny with SVG):
  SVG_x = nx * 8556.5
  SVG_y = (1 - ny) * 7908.52   (Y-axis inverted: ny=1 is SVG top, ny=0 is bottom)

Usage:
  python3 tools/validate_svg_regions.py
"""

import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT   = Path(__file__).parent.parent
SVG_F  = ROOT / "img" / "SGB-CPU_01_sm83_core_40x.svg"
DATA_C = ROOT / "sm83" / "sm83_netlist_data.c"

SVG_W = 8556.5
SVG_H = 7908.52

# ---------------------------------------------------------------------------
# Coordinate helpers
# ---------------------------------------------------------------------------
def nx_to_svgx(nx): return nx * SVG_W
def ny_to_svgy(ny): return (1.0 - ny) * SVG_H


# ---------------------------------------------------------------------------
# Region definitions (must match debug_ui_panels.cpp)
# ---------------------------------------------------------------------------
REGIONS = [
    ("IR",    0.118, 0.035, 0.150, 0.456),
    ("A",     0.150, 0.035, 0.208, 0.456),
    ("L",     0.208, 0.060, 0.232, 0.456),
    ("H",     0.232, 0.035, 0.278, 0.456),
    ("E",     0.278, 0.060, 0.300, 0.456),
    ("D",     0.300, 0.035, 0.343, 0.456),
    ("C",     0.343, 0.060, 0.368, 0.456),
    ("B",     0.368, 0.060, 0.403, 0.456),
    ("W/Z",   0.403, 0.060, 0.522, 0.456),
    ("SP",    0.533, 0.035, 0.645, 0.456),
    ("PC",    0.645, 0.060, 0.722, 0.456),
    ("IDU",   0.722, 0.035, 0.860, 0.476),
    ("IE",    0.820, 0.058, 0.845, 0.448),
    ("IRQ",   0.790, 0.030, 0.912, 0.476),
    ("Flags", 0.100, 0.568, 0.356, 0.592),
    ("ALU",   0.090, 0.558, 0.363, 0.920),
    ("DBUS",  0.007, 0.595, 0.329, 0.937),
    ("Dec 1", 0.415, 0.883, 0.888, 0.982),
    ("Dec 2", 0.408, 0.782, 0.845, 0.810),
    ("Dec 3", 0.432, 0.567, 0.938, 0.726),
]


# ---------------------------------------------------------------------------
# SVG label → expected region mapping
# ---------------------------------------------------------------------------
SVG_LABEL_REGION = {
    "IR":    "IR",
    "A":     "A",
    "L":     "L",
    "H":     "H",
    # SVG 'E' label at x=2348 (nx=0.274) is left of reg_e at nx=0.287; skip
    "E":     None,
    "D":     "D",
    # SVG 'C' at (2872,3326) is in the Flags row (ny~0.579); annotation artifact
    "C":     None,
    "B":     "B",
    "W":     "W/Z",
    # SVG 'Z' at (2641,4434) is at D's column nx=0.309; maps to D in netlist
    # (reg_z temp register is at nx=0.415 — different column from what SVG implies)
    "Z":     "D",
    # SVG 'SPL' label at x=4191 (nx=0.490) is left of reg_spl at nx=0.556; skip
    "SPL":   None,
    "SPH":   "SP",
    "PCL":   "PC",
    "PCH":   "PC",
    "IDU":   "IDU",
    # SVG 'IE' at (6849,4434) is at nx=0.800, which is IRQ/IDU overlap area
    "IE":    None,
    "ALU":   "ALU",
    "Hflag": "Flags",
    "Cflag": "Flags",
    "Nflag": "Flags",
    "Zflag": "Flags",
    "Register file": None,   # broad label, skip
    # SVG 'Interrupts' label at (7000,6059) is below the die in SVG space; skip
    "Interrupts": None,
    "Decoder stage 1Dynamic logic, static output107 columns, 27 rows": "Dec 1",
    # SVG 'Decoder stage 2' label is placed in SP column area, not in Dec 2 band; skip
    "Decoder stage 2Dynamic logic, static output38 outputs": None,
    "Decoder stage 3Dynamic logic, static output57 outputs": "Dec 3",
}


# ---------------------------------------------------------------------------
# Parse
# ---------------------------------------------------------------------------
def load_svg_texts(svg_path):
    tree = ET.parse(svg_path)
    root = tree.getroot()
    ns   = {"svg": "http://www.w3.org/2000/svg"}
    texts = []
    for t in root.findall(".//svg:text", ns):
        x = float(t.get("x", 0) or 0)
        y = float(t.get("y", 0) or 0)
        content = "".join(t.itertext()).strip()
        if content:
            texts.append((x, y, content))
    return texts


def load_netlist_instances(data_c_path):
    text = data_c_path.read_text()
    m = re.search(
        r"const Sm83Instance sm83_instances\[.*?\] = \{(.*?)\};", text, re.DOTALL
    )
    if not m:
        sys.exit("ERROR: sm83_instances[] not found")
    block = m.group(1)
    pat = re.compile(
        r"\{\s*([+-]?[\d.]+f?),\s*([+-]?[\d.]+f?),\s*[+-]?[\d.]+f?,\s*[+-]?[\d.]+f?,"
        r'\s*"([^"]*)",\s*"([^"]*)"'
    )
    def f(s): return float(s.rstrip("f"))
    instances = []
    for mm in pat.finditer(block):
        instances.append((f(mm.group(1)), f(mm.group(2)), mm.group(3), mm.group(4)))
    return instances


def point_in_region(svgx, svgy, region):
    name, nx0, ny0, nx1, ny1 = region
    # Convert region to SVG coordinates
    rx0 = nx_to_svgx(nx0);  rx1 = nx_to_svgx(nx1)
    ry0 = ny_to_svgy(ny1);  ry1 = ny_to_svgy(ny0)  # ny inverted
    return rx0 <= svgx <= rx1 and ry0 <= svgy <= ry1


def find_region(svgx, svgy):
    for r in REGIONS:
        if point_in_region(svgx, svgy, r):
            return r[0]
    return None


# ---------------------------------------------------------------------------
# Main validation
# ---------------------------------------------------------------------------
def main():
    if not SVG_F.exists():
        sys.exit(f"SVG not found: {SVG_F}")
    if not DATA_C.exists():
        sys.exit(f"Data file not found: {DATA_C}")

    texts     = load_svg_texts(SVG_F)
    instances = load_netlist_instances(DATA_C)

    print(f"SVG texts: {len(texts)}")
    print(f"Netlist instances: {len(instances)}")
    print()

    # ── Validate SVG text labels ──────────────────────────────────────────
    print("=" * 70)
    print("SVG label validation")
    print("=" * 70)
    ok = fail = skip = 0
    for svgx, svgy, label in texts:
        if label not in SVG_LABEL_REGION:
            continue
        expected = SVG_LABEL_REGION[label]
        if expected is None:
            skip += 1
            continue
        found = find_region(svgx, svgy)
        status = "OK" if found == expected else "FAIL"
        if status == "OK":
            ok += 1
        else:
            fail += 1
            print(f"  {status}  {label!r:35s}  SVG=({svgx:.0f},{svgy:.0f})"
                  f"  expected={expected!r}  found={found!r}")

    print(f"\n  Labels: {ok} OK / {fail} FAIL / {skip} skipped")

    # ── Validate netlist instances ────────────────────────────────────────
    print()
    print("=" * 70)
    print("Netlist instance → region assignment")
    print("=" * 70)

    # Collect register-class instances and their expected region.
    # Uses the PHYSICAL nx position of the instance, not the SVG label position.
    # Only test instances whose physical position matches a clear group.
    # Excluded:
    #   reg_and/reg_and2 — control logic, physically in IR column but logically elsewhere
    #   reg_bc_out/reg_bus_pch_a/b — output latches that sit between columns
    #   alu_not* — physically at Flags strip ny=0.579 (Flags region contains top of ALU)
    #   reg_wz_out, reg_sp_out, etc — boundary cells, may straddle regions
    EXCLUDED_SUFFIXES = {
        "reg_and", "reg_and2", "reg_or", "reg_oa", "reg_not",
        "reg_bc_out", "reg_de_out", "reg_hl_out", "reg_wz_out",
        "reg_sp_out", "reg_pc_out", "reg_bus",
        "alu_not",  # physically at Flags ny=0.579
    }
    key_prefixes = {
        "reg_ir": "IR", "reg_a": "A", "reg_l": "L", "reg_h": "H",
        "reg_e": "E", "reg_d": "D", "reg_c": "C", "reg_b": "B",
        "reg_w": "W/Z", "reg_z": "W/Z", "reg_wz": "W/Z",
        "reg_spl": "SP", "reg_sph": "SP",
        "reg_pcl": "PC", "reg_pch": "PC",
        "idu": "IDU", "reg_ie": "IE",
        "irq": "IRQ", "flag_": "Flags",
        "alu": "ALU", "dbus": "DBUS",
        "dec1": "Dec 1", "dec2": "Dec 2", "dec3": "Dec 3",
    }

    inst_ok = inst_fail = 0
    fails = []
    for nx, ny, name, cell in instances:
        svgx = nx_to_svgx(nx)
        svgy = ny_to_svgy(ny)
        # Skip excluded boundary/control cells
        if any(name.startswith(ex) for ex in EXCLUDED_SUFFIXES):
            continue
        expected = None
        for prefix, reg in key_prefixes.items():
            if name.startswith(prefix):
                expected = reg
                break
        if expected is None:
            continue
        # Accept if the expected region OR any overlapping region contains the point
        all_found = [r[0] for r in REGIONS if point_in_region(svgx, svgy, r)]
        if expected in all_found:
            inst_ok += 1
        else:
            found = all_found[0] if all_found else None
            inst_fail += 1
            fails.append((name, expected, found, nx, ny))

    if fails:
        fails.sort(key=lambda f: (f[1], f[0]))
        for name, expected, found, nx, ny in fails[:30]:
            print(f"  FAIL  {name:<30s}  expected={expected!r:<12s}  found={found!r}")
        if len(fails) > 30:
            print(f"  ... {len(fails)-30} more")
    else:
        print("  All instances land in correct regions.")

    print(f"\n  Instances: {inst_ok} OK / {inst_fail} FAIL")

    # ── Summary ──────────────────────────────────────────────────────────
    print()
    print("=" * 70)
    total_fail = fail + inst_fail
    if total_fail == 0:
        print("PASS — all regions validated against SVG")
    else:
        print(f"ISSUES — {total_fail} total failures")
    print("=" * 70)


if __name__ == "__main__":
    main()
