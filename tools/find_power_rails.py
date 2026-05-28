#!/usr/bin/env python3
"""
find_power_rails.py - Rank candidate VCC/GND nets in the SM83 netlist.

Strategy (from sm83_real_cpu_die_trace_plan.md §2):
  - Net connected to many P-transistor sources/drains → VCC candidate
  - Net connected to many N-transistor sources/drains → GND candidate
  - Net with large geometric spread (many arcs) → rail candidate
  - Known-name check (vcc/vdd/gnd/vss) → fast path (probably empty)

All data is parsed from sm83/sm83_netlist_data.c to match exactly what the
runtime uses. The JSON is not used because it lacks named nets.

Output: ranked candidate tables + suggested #define values.
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).parent.parent
DATA_C = ROOT / "sm83" / "sm83_netlist_data.c"

# ---------------------------------------------------------------------------
# Parse sm83_netlist_data.c
# ---------------------------------------------------------------------------

def parse_nets(text):
    """Return list of net names (index = net_id)."""
    m = re.search(r'const char \* const sm83_nets\[.*?\] = \{(.*?)\};',
                  text, re.DOTALL)
    if not m:
        sys.exit("ERROR: could not find sm83_nets[] in data.c")
    block = m.group(1)
    names = re.findall(r'"([^"]*)"', block)
    print(f"  Parsed {len(names)} nets")
    return names


def parse_transistors(text):
    """
    Return list of (layer, gate_net, s1_net, s2_net).
    layer: 4=NTRANS, 5=PTRANS
    Fields from struct: nx ny nw nh x y w h layer gate_net s1_net s2_net
    """
    m = re.search(r'const Sm83Transistor sm83_transistors\[.*?\] = \{(.*?)\};',
                  text, re.DOTALL)
    if not m:
        sys.exit("ERROR: could not find sm83_transistors[] in data.c")
    block = m.group(1)
    # Each entry: { f f f f f f f f  i  i i i }
    pattern = re.compile(
        r'\{\s*'
        r'[+-]?[\d.ef]+,\s*'  # nx
        r'[+-]?[\d.ef]+,\s*'  # ny
        r'[+-]?[\d.ef]+,\s*'  # nw
        r'[+-]?[\d.ef]+,\s*'  # nh
        r'[+-]?[\d.ef]+,\s*'  # x
        r'[+-]?[\d.ef]+,\s*'  # y
        r'[+-]?[\d.ef]+,\s*'  # w
        r'[+-]?[\d.ef]+,\s*'  # h
        r'(\d+),\s*'           # layer  ← group 1
        r'(-?\d+),\s*'         # gate   ← group 2
        r'(-?\d+),\s*'         # s1     ← group 3
        r'(-?\d+)'             # s2     ← group 4
        r'\s*\}'
    )
    transistors = []
    for m2 in pattern.finditer(block):
        layer    = int(m2.group(1))
        gate_net = int(m2.group(2))
        s1_net   = int(m2.group(3))
        s2_net   = int(m2.group(4))
        transistors.append((layer, gate_net, s1_net, s2_net))
    print(f"  Parsed {len(transistors)} transistors")
    return transistors


def parse_arcs(text):
    """Return list of net_id per arc (last int field in each arc entry)."""
    m = re.search(r'const Sm83Arc sm83_arcs\[.*?\] = \{(.*?)\};',
                  text, re.DOTALL)
    if not m:
        print("  WARNING: could not find sm83_arcs[] — skipping arc spread score")
        return []
    block = m.group(1)
    # Arc struct fields: ntx nty nhx nhy  tx ty hx hy  width  layer  tail_node head_node  net_id
    pattern = re.compile(
        r'\{\s*'
        r'[+-]?[\d.ef]+,\s*'  # ntx
        r'[+-]?[\d.ef]+,\s*'  # nty
        r'[+-]?[\d.ef]+,\s*'  # nhx
        r'[+-]?[\d.ef]+,\s*'  # nhy
        r'[+-]?[\d.ef]+,\s*'  # tx
        r'[+-]?[\d.ef]+,\s*'  # ty
        r'[+-]?[\d.ef]+,\s*'  # hx
        r'[+-]?[\d.ef]+,\s*'  # hy
        r'[+-]?[\d.ef]+,\s*'  # width
        r'\d+,\s*'             # layer
        r'-?\d+,\s*'           # tail_node
        r'-?\d+,\s*'           # head_node
        r'(-?\d+)'             # net_id ← group 1
        r'\s*\}'
    )
    arc_nets = [int(m2.group(1)) for m2 in pattern.finditer(block)]
    print(f"  Parsed {len(arc_nets)} arcs")
    return arc_nets


# ---------------------------------------------------------------------------
# Scoring
# ---------------------------------------------------------------------------

NTRANS = 4
PTRANS = 5


def score_nets(nets, transistors, arc_nets):
    net_count = len(nets)

    # --- Transistor terminal connectivity ---
    # For each net: how many times it appears as s1/s2 of N-transistors vs P-transistors
    n_terminal = defaultdict(int)  # net → count as N-trans terminal
    p_terminal = defaultdict(int)  # net → count as P-trans terminal
    gate_of    = defaultdict(int)  # net → count as gate of any transistor

    for layer, gate, s1, s2 in transistors:
        if gate >= 0: gate_of[gate] += 1
        if layer == NTRANS:
            if s1 >= 0: n_terminal[s1] += 1
            if s2 >= 0: n_terminal[s2] += 1
        elif layer == PTRANS:
            if s1 >= 0: p_terminal[s1] += 1
            if s2 >= 0: p_terminal[s2] += 1

    # --- Arc count per net (geometric spread) ---
    arc_count = defaultdict(int)
    for nid in arc_nets:
        if nid >= 0:
            arc_count[nid] += 1

    # --- Known-name fast path ---
    VCC_NAMES = {'vcc', 'vdd', 'VCC', 'VDD'}
    GND_NAMES = {'gnd', 'vss', 'GND', 'VSS'}

    # --- Build score table ---
    results = []
    for nid, name in enumerate(nets):
        nt = n_terminal[nid]
        pt = p_terminal[nid]
        ga = gate_of[nid]
        ac = arc_count[nid]
        total = nt + pt + ga + ac

        # GND score: driven by many N-trans terminals, not many P-trans
        # VCC score: driven by many P-trans terminals, not many N-trans
        gnd_score = nt * 3 + ac - pt
        vcc_score = pt * 3 + ac - nt

        is_named_vcc = name in VCC_NAMES
        is_named_gnd = name in GND_NAMES

        results.append({
            'id':         nid,
            'name':       name,
            'n_term':     nt,
            'p_term':     pt,
            'gate_of':    ga,
            'arc_count':  ac,
            'total':      total,
            'gnd_score':  gnd_score,
            'vcc_score':  vcc_score,
            'named_vcc':  is_named_vcc,
            'named_gnd':  is_named_gnd,
        })

    return results


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def print_table(title, rows, key, n=20):
    print(f"\n{'='*70}")
    print(f"  {title}  (top {n})")
    print(f"{'='*70}")
    print(f"  {'rank':>4}  {'net_id':>7}  {'name':<30}  {'score':>8}  "
          f"{'n_term':>7}  {'p_term':>7}  {'arcs':>7}  {'gates':>6}")
    print(f"  {'-'*4}  {'-'*7}  {'-'*30}  {'-'*8}  {'-'*7}  {'-'*7}  {'-'*7}  {'-'*6}")
    ranked = sorted(rows, key=lambda r: r[key], reverse=True)[:n]
    for rank, r in enumerate(ranked, 1):
        print(f"  {rank:>4}  {r['id']:>7}  {r['name']:<30}  {r[key]:>8}  "
              f"{r['n_term']:>7}  {r['p_term']:>7}  {r['arc_count']:>7}  {r['gate_of']:>6}")


def print_named(rows):
    named = [r for r in rows if r['named_vcc'] or r['named_gnd']]
    if named:
        print(f"\n{'='*70}")
        print("  NAMED POWER RAILS (vcc/vdd/gnd/vss found by name!)")
        print(f"{'='*70}")
        for r in named:
            tag = 'VCC' if r['named_vcc'] else 'GND'
            print(f"  [{tag}]  net_id={r['id']}  name={r['name']}  "
                  f"n_term={r['n_term']}  p_term={r['p_term']}  arcs={r['arc_count']}")
    else:
        print("\n  (no nets named vcc/vdd/gnd/vss — heuristic ranking only)")


def suggest_defines(rows):
    top_gnd = sorted(rows, key=lambda r: r['gnd_score'], reverse=True)[:3]
    top_vcc = sorted(rows, key=lambda r: r['vcc_score'], reverse=True)[:3]

    print(f"\n{'='*70}")
    print("  SUGGESTED #defines (verify manually before committing!)")
    print(f"{'='*70}")
    print(f"\n  /* GND candidates (high N-terminal count) */")
    for r in top_gnd:
        print(f"  #define SM83_GND_NET  {r['id']}  /* {r['name']} "
              f"n={r['n_term']} p={r['p_term']} arcs={r['arc_count']} */")
    print(f"\n  /* VCC candidates (high P-terminal count) */")
    for r in top_vcc:
        print(f"  #define SM83_VCC_NET  {r['id']}  /* {r['name']} "
              f"n={r['n_term']} p={r['p_term']} arcs={r['arc_count']} */")

    print(f"\n  Best single guess:")
    gnd = top_gnd[0]
    vcc = top_vcc[0]
    print(f"    GND → net_id {gnd['id']} ({gnd['name']})")
    print(f"    VCC → net_id {vcc['id']} ({vcc['name']})")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    print(f"Reading {DATA_C} ...")
    text = DATA_C.read_text()
    print(f"  File size: {len(text):,} bytes\n")

    print("Parsing nets ...")
    nets = parse_nets(text)

    print("Parsing transistors ...")
    transistors = parse_transistors(text)

    print("Parsing arcs ...")
    arc_nets = parse_arcs(text)

    print("\nScoring ...")
    rows = score_nets(nets, transistors, arc_nets)

    print_named(rows)
    print_table("GND CANDIDATES  (ranked by gnd_score = N-terminals*3 + arcs - P-terminals)",
                rows, 'gnd_score', n=25)
    print_table("VCC CANDIDATES  (ranked by vcc_score = P-terminals*3 + arcs - N-terminals)",
                rows, 'vcc_score', n=25)
    print_table("MOST CONNECTED  (ranked by total = n_term + p_term + gate_of + arcs)",
                rows, 'total', n=25)
    suggest_defines(rows)

    print("\nDone.")


if __name__ == "__main__":
    main()
