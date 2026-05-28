#!/usr/bin/env python3
"""
trace_register_nets.py - Resolve SM83 net_ids for register bits via spatial tracing.

Strategy (Etapa E of sm83_real_cpu_die_trace_plan.md):
  Each named instance in sm83_instances[] (e.g. "reg_a[0]") is a D flip-flop
  cell placed at a known (x, y) coordinate.  We find the transistors spatially
  close to that instance and identify which net appears most frequently as a
  source/drain (s1/s2) terminal — that is the output net of the cell.

  For DFF cells the output latch transistors cluster tightly around the cell
  centre and share one net as both s1 and s2 across several transistors.  The
  net that appears most as a shared terminal is the Q output.

  Confidence assigned:
    PROBABLE  (2) — found by spatial proximity, no inversion ambiguity
    PROXY     (1) — best candidate but fewer than MIN_HITS neighbours

Output:
  Prints a C initialiser block ready to paste into sm83_semantic_map.c.
  Also prints a summary table for manual review.
"""

import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT    = Path(__file__).parent.parent
DATA_C  = ROOT / "sm83" / "sm83_netlist_data.c"

# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------

FLOAT_RE = r'([+-]?[\d.]+f?)'
INT_RE   = r'(-?\d+)'

def _f(s): return float(s.rstrip('f'))

def parse_instances(text):
    """Return list of (name, cell, x, y) from sm83_instances[]."""
    m = re.search(r'const Sm83Instance sm83_instances\[.*?\] = \{(.*?)\};',
                  text, re.DOTALL)
    if not m:
        sys.exit("ERROR: could not find sm83_instances[]")
    block = m.group(1)
    pat = re.compile(
        r'\{\s*'
        r'[+-]?[\d.]+f?,\s*'   # nx
        r'[+-]?[\d.]+f?,\s*'   # ny
        r'([+-]?[\d.]+f?),\s*' # x  ← group 1
        r'([+-]?[\d.]+f?),\s*' # y  ← group 2
        r'"([^"]*)",\s*'        # name ← group 3
        r'"([^"]*)"'            # cell ← group 4
        r'\s*\}'
    )
    insts = []
    for mm in pat.finditer(block):
        insts.append((_f(mm.group(1)), _f(mm.group(2)), mm.group(3), mm.group(4)))
    return insts


def parse_transistors(text):
    """Return list of (x, y, layer, gate, s1, s2)."""
    m = re.search(r'const Sm83Transistor sm83_transistors\[.*?\] = \{(.*?)\};',
                  text, re.DOTALL)
    if not m:
        sys.exit("ERROR: could not find sm83_transistors[]")
    block = m.group(1)
    pat = re.compile(
        r'\{\s*'
        r'[+-]?[\d.]+f?,\s*'   # nx
        r'[+-]?[\d.]+f?,\s*'   # ny
        r'[+-]?[\d.]+f?,\s*'   # nw
        r'[+-]?[\d.]+f?,\s*'   # nh
        r'([+-]?[\d.]+f?),\s*' # x  ← group 1
        r'([+-]?[\d.]+f?),\s*' # y  ← group 2
        r'[+-]?[\d.]+f?,\s*'   # w
        r'[+-]?[\d.]+f?,\s*'   # h
        r'(\d+),\s*'            # layer ← group 3
        r'(-?\d+),\s*'          # gate  ← group 4
        r'(-?\d+),\s*'          # s1    ← group 5
        r'(-?\d+)'              # s2    ← group 6
        r'\s*\}'
    )
    trans = []
    for mm in pat.finditer(block):
        trans.append((
            _f(mm.group(1)), _f(mm.group(2)),
            int(mm.group(3)),
            int(mm.group(4)), int(mm.group(5)), int(mm.group(6))
        ))
    return trans


# ---------------------------------------------------------------------------
# Tracing
# ---------------------------------------------------------------------------

# Instances we care about — must match sm83_semantic_map entries
REGISTER_PREFIXES = [
    "reg_pcl", "reg_pch",
    "reg_a",
    "reg_b", "reg_c", "reg_d", "reg_e", "reg_h", "reg_l",
    "reg_spl", "reg_sph",
    "reg_ir",
    "flag_z", "flag_n", "flag_h", "flag_c",
    "dbus_bridge",
    "idu",
]

RADIUS   = 50.0   # Electric VLSI units — cells are ~50x50 to 200x200 units wide
MIN_HITS = 2      # minimum transistor neighbours to assign PROBABLE confidence

NTRANS = 4
PTRANS = 5


def find_output_net(inst_x, inst_y, transistors, radius=RADIUS):
    """
    Find the most likely Q-output net of a DFF at (inst_x, inst_y).

    A DFF output node is shared across multiple transistors in a cross-coupled
    latch.  We count how many times each net appears as s1 or s2 in transistors
    near the cell.  The net with the highest count is the output.

    Returns (net_id, hit_count, all_candidates).
    """
    counter = Counter()
    for (tx, ty, layer, gate, s1, s2) in transistors:
        if abs(tx - inst_x) > radius or abs(ty - inst_y) > radius:
            continue
        if s1 >= 0: counter[s1] += 1
        if s2 >= 0: counter[s2] += 1

    if not counter:
        return -1, 0, []

    ranked = counter.most_common()
    best_id, best_count = ranked[0]
    return best_id, best_count, ranked


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print(f"Reading {DATA_C} ...")
    text = DATA_C.read_text()

    print("Parsing instances ...")
    instances = parse_instances(text)
    print(f"  {len(instances)} instances found")

    print("Parsing transistors ...")
    transistors = parse_transistors(text)
    print(f"  {len(transistors)} transistors found")

    # Filter to register/bus instances only
    reg_insts = [
        (x, y, name, cell)
        for (x, y, name, cell) in instances
        if any(name.startswith(p) for p in REGISTER_PREFIXES)
    ]
    print(f"  {len(reg_insts)} register/bus instances selected\n")

    # Trace each instance
    results = []   # (name, net_id, hits, confidence, candidates_top3)
    for (x, y, name, cell) in sorted(reg_insts, key=lambda r: r[2]):
        net_id, hits, ranked = find_output_net(x, y, transistors)
        if hits >= MIN_HITS:
            conf = "PROBABLE"
        elif hits > 0:
            conf = "PROXY"
        else:
            conf = "UNKNOWN"
        top3 = ranked[:3] if ranked else []
        results.append((name, net_id, hits, conf, top3))

    # --- Summary table ---
    print(f"{'='*72}")
    print(f"  {'Instance':<22}  {'net_id':>7}  {'hits':>5}  {'conf':<10}  {'top candidates'}")
    print(f"  {'-'*22}  {'-'*7}  {'-'*5}  {'-'*10}  {'-'*30}")
    for (name, nid, hits, conf, top3) in results:
        top_str = "  ".join(f"{n}({c})" for n, c in top3)
        print(f"  {name:<22}  {nid:>7}  {hits:>5}  {conf:<10}  {top_str}")

    # --- C initialiser block ---
    print(f"\n{'='*72}")
    print("  C initialiser block (for sm83_semantic_map.c):")
    print(f"{'='*72}\n")

    # Group by signal
    sig_map = {
        "reg_pcl": ("SM83_SEM_PCL",   "PC low byte"),
        "reg_pch": ("SM83_SEM_PCH",   "PC high byte"),
        "reg_a":   ("SM83_SEM_REG_A", "Accumulator A"),
        "reg_b":   ("SM83_SEM_REG_B", "Register B"),
        "reg_c":   ("SM83_SEM_REG_C", "Register C"),
        "reg_d":   ("SM83_SEM_REG_D", "Register D"),
        "reg_e":   ("SM83_SEM_REG_E", "Register E"),
        "reg_h":   ("SM83_SEM_REG_H", "Register H"),
        "reg_l":   ("SM83_SEM_REG_L", "Register L"),
        "reg_spl": ("SM83_SEM_SPL",   "Stack Pointer low"),
        "reg_sph": ("SM83_SEM_SPH",   "Stack Pointer high"),
        "reg_ir":  ("SM83_SEM_IR",    "Instruction Register"),
        "flag_z":  ("SM83_SEM_FLAG_Z","Flag Z"),
        "flag_n":  ("SM83_SEM_FLAG_N","Flag N"),
        "flag_h":  ("SM83_SEM_FLAG_H","Flag H"),
        "flag_c":  ("SM83_SEM_FLAG_C","Flag C"),
        "dbus_bridge": ("SM83_SEM_DBUS", "Data Bus"),
        "idu":     ("SM83_SEM_IDU",   "IDU output"),
    }

    current_group = None
    for (name, nid, hits, conf, top3) in results:
        # Determine prefix and bit
        prefix = next((p for p in REGISTER_PREFIXES if name.startswith(p)), name)
        bit_part = name[len(prefix):]
        bit = 0
        if bit_part.startswith("[") and bit_part.endswith("]"):
            bit = int(bit_part[1:-1])

        sig, label = sig_map.get(prefix, (f"SM83_SEM_UNKNOWN_SIG /*{prefix}*/", prefix))

        if sig != current_group:
            print(f"    /* ── {label} ── */")
            current_group = sig

        conf_macro = conf  # PROBABLE / PROXY / UNKNOWN
        note = f"hits={hits}" if hits > 0 else "not found"
        print(f"    {{ {nid:>5}, \"{name}\", {sig}, {bit}, {conf_macro}, \"{note}\" }},")

    print("\nDone.")


if __name__ == "__main__":
    main()
