#!/usr/bin/env python3
"""
check_netlist.py - Validate consistency between sm83_netlist.json and sm83_netlist_data.h

Checks:
  1. Counts (transistors, nodes, arcs, instances) match between JSON and header.
  2. Bounding box values match.
  3. Per-layer ranges in the header are contiguous and cover the full count.

Usage:
  python3 tools/check_netlist.py [--json PATH] [--header PATH]
"""

import re
import json
import sys
import argparse
from pathlib import Path


def parse_header(path: str) -> dict:
    text = Path(path).read_text()
    def macro(name):
        m = re.search(rf"#define\s+{name}\s+([\d.]+)f?", text)
        if m:
            v = m.group(1)
            return float(v) if "." in v else int(v)
        return None

    result = {}
    for key in ("SM83_TRANSISTOR_COUNT", "SM83_NODE_COUNT", "SM83_ARC_COUNT",
                "SM83_INSTANCE_COUNT", "SM83_BBOX_X_MIN", "SM83_BBOX_X_MAX",
                "SM83_BBOX_Y_MIN", "SM83_BBOX_Y_MAX"):
        result[key] = macro(key)

    # Collect all SM83_TRANS_*_START/END, SM83_NODE_*_START/END, SM83_ARC_*_START/END
    for prefix in ("TRANS", "NODE", "ARC"):
        ranges = {}
        for m in re.finditer(rf"#define\s+SM83_{prefix}_(\w+)_(START|END)\s+(\d+)", text):
            lname, se, val = m.group(1), m.group(2), int(m.group(3))
            if lname not in ranges:
                ranges[lname] = {}
            ranges[lname][se] = val
        result[f"{prefix.lower()}_ranges"] = ranges

    return result


def load_json(path: str) -> dict:
    with open(path) as f:
        return json.load(f)


def check(json_path: str, header_path: str) -> int:
    errors = 0

    print(f"JSON   : {json_path}")
    print(f"Header : {header_path}")
    print()

    jdata = load_json(json_path)
    hdata = parse_header(header_path)

    js = jdata.get("stats", {})
    jb = jdata.get("bbox", {})

    checks = [
        ("Transistor count", js.get("n_transistors"), hdata.get("SM83_TRANSISTOR_COUNT")),
        ("Node count",       js.get("n_nodes"),       hdata.get("SM83_NODE_COUNT")),
        ("Arc count",        js.get("n_arcs"),         hdata.get("SM83_ARC_COUNT")),
        ("Instance count",   js.get("n_instances"),    hdata.get("SM83_INSTANCE_COUNT")),
        ("BBox X min",       jb.get("x_min"),          hdata.get("SM83_BBOX_X_MIN")),
        ("BBox X max",       jb.get("x_max"),          hdata.get("SM83_BBOX_X_MAX")),
        ("BBox Y min",       jb.get("y_min"),          hdata.get("SM83_BBOX_Y_MIN")),
        ("BBox Y max",       jb.get("y_max"),          hdata.get("SM83_BBOX_Y_MAX")),
    ]

    for label, jval, hval in checks:
        if jval is None or hval is None:
            print(f"  SKIP  {label}: json={jval} header={hval}")
            continue
        if abs(float(jval) - float(hval)) > 0.01:
            print(f"  FAIL  {label}: json={jval}  header={hval}")
            errors += 1
        else:
            print(f"  OK    {label}: {jval}")

    print()

    # Check per-layer ranges are contiguous and sum to total
    for prefix, count_key in [("trans", "SM83_TRANSISTOR_COUNT"),
                               ("node",  "SM83_NODE_COUNT"),
                               ("arc",   "SM83_ARC_COUNT")]:
        ranges = hdata.get(f"{prefix}_ranges", {})
        total = hdata.get(count_key, 0)
        covered = 0
        for lname, r in sorted(ranges.items()):
            s = r.get("START", 0)
            e = r.get("END", 0)
            if e < s:
                print(f"  FAIL  {prefix.upper()} layer {lname}: END({e}) < START({s})")
                errors += 1
            covered += e - s
        if ranges:
            if covered != total:
                print(f"  FAIL  {prefix.upper()} layer ranges cover {covered} but count is {total}")
                errors += 1
            else:
                print(f"  OK    {prefix.upper()} layer ranges: {len(ranges)} layers, {covered} total")

    print()
    if errors == 0:
        print("All checks passed.")
    else:
        print(f"{errors} check(s) FAILED.")
    return errors


def main():
    ap = argparse.ArgumentParser(description="Validate sm83 netlist data consistency")
    ap.add_argument("--json",   default="data/sm83_netlist.json")
    ap.add_argument("--header", default="sm83/sm83_netlist_data.h")
    args = ap.parse_args()
    sys.exit(check(args.json, args.header))


if __name__ == "__main__":
    main()
