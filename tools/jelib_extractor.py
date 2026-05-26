#!/usr/bin/env python3
"""
jelib_extractor.py - SM83 .jelib layout extractor

Reads the Electric VLSI .jelib file from dmg-schematics/sm83_cells/sm83.jelib
and generates:
  - data/sm83_netlist.json   (optional inspection artifact)
  - sm83_netlist_data.c/.h   (static C arrays used by the emulator)

Usage:
  python3 tools/jelib_extractor.py [--jelib PATH] [--out-dir DIR] [--no-json]

Format notes (Electric VLSI .jelib):
  C<name>;N{lay}  - cell header
  N<type>|<id>|<group>|<x>|<y>|<w>|<h>|<orient>|  - node
  A<layer>|<net>|<width>|<flags>|<tail-id>|<tail-port>|<tx>|<ty>|<head-id>|<head-port>|<hx>|<hy>
  E<name>||<group>|<node-id>|<port>|<direction>  - export
  I<cell>|<inst-name>|<group>|<x>|<y>|...  - instance (in top-level cell)
"""

import re
import sys
import json
import argparse
from pathlib import Path
from collections import defaultdict


# ---------------------------------------------------------------------------
# Layer name normalization
# ---------------------------------------------------------------------------
LAYER_MAP = {
    "Metal-1":       "metal1",
    "Polysilicon-1": "poly",
    "N-Active":      "nactive",
    "P-Active":      "pactive",
    "N-Transistor":  "ntrans",
    "P-Transistor":  "ptrans",
}

LAYER_IDS = {
    "metal1":  0,
    "poly":    1,
    "nactive": 2,
    "pactive": 3,
    "ntrans":  4,
    "ptrans":  5,
}

ARC_LAYER_NAMES = {"Metal-1", "Polysilicon-1", "N-Active", "P-Active"}


def norm_layer(raw: str) -> str:
    return LAYER_MAP.get(raw, raw.lower().replace("-", ""))


# ---------------------------------------------------------------------------
# .jelib parser
# ---------------------------------------------------------------------------
class JelibParser:
    def __init__(self, path: str):
        self.path = path
        self.cells: dict[str, dict] = {}   # cell_name -> {nodes, arcs, exports}
        self._current_cell: str | None = None
        self._parse()

    def _parse(self):
        with open(self.path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()

        for raw in lines:
            line = raw.rstrip("\n")
            if not line or line.startswith("#"):
                continue

            ch = line[0]

            if ch == "C":
                self._handle_cell(line)
            elif ch == "N" and self._current_cell is not None:
                self._handle_node(line)
            elif ch == "A" and self._current_cell is not None:
                self._handle_arc(line)
            elif ch == "E" and self._current_cell is not None:
                self._handle_export(line)
            elif ch == "I" and self._current_cell is not None:
                self._handle_instance(line)

    def _handle_cell(self, line: str):
        # Cname;version{view}|...
        m = re.match(r"C([^;]+);", line)
        if not m:
            return
        name = m.group(1)
        if "{lay}" not in line:
            self._current_cell = None
            return
        self._current_cell = name
        if name not in self.cells:
            self.cells[name] = {"nodes": {}, "arcs": [], "exports": {}, "instances": []}

    def _handle_node(self, line: str):
        # N<type>|<id>|<group>|<x>|<y>|<w>|<h>|<orient>|...
        parts = line.split("|")
        if len(parts) < 5:
            return
        ntype_raw = parts[0][1:]  # strip leading 'N'
        nid = parts[1]
        try:
            x = float(parts[3]) if parts[3] else 0.0
            y = float(parts[4]) if parts[4] else 0.0
            w = float(parts[5]) if len(parts) > 5 and parts[5] else 0.0
            h = float(parts[6]) if len(parts) > 6 and parts[6] else 0.0
        except ValueError:
            x = y = w = h = 0.0

        layer = norm_layer(ntype_raw)
        cell = self.cells[self._current_cell]
        cell["nodes"][nid] = {
            "id": nid,
            "type": ntype_raw,
            "layer": layer,
            "x": x,
            "y": y,
            "w": w,
            "h": h,
        }

    def _handle_arc(self, line: str):
        # Electric VLSI arc format (13 fields):
        # A<layer>|<net>|<width>|<tail-rot>|<tail-node-id>|<tail-port>|<tx>|<ty>|<head-node-id>|<head-port>|<hx>|<hy>
        # Example: AMetal-1|net@1|||R1800|contact@0||0|0|contact@1||29.5|0
        parts = line.split("|")
        if len(parts) < 13:
            return
        layer_raw = parts[0][1:]
        if layer_raw not in ARC_LAYER_NAMES:
            return
        net = parts[1]
        try:
            width_str = parts[2].strip()
            width = float(width_str) if width_str else 0.0
        except ValueError:
            width = 0.0
        # Electric VLSI arc field layout (0-indexed after split on '|'):
        #  0=Alayer  1=net  2=width  3=tail-rot  4=tail-rot2  5=tail-node  6=tail-port
        #  7=tx  8=ty  9=head-node  10=head-port  11=hx  12=hy
        tail_id = parts[5]    # tail node ID (e.g. "contact@0")
        tail_port = parts[6]  # tail port name
        try:
            tx = float(parts[7]) if parts[7] else 0.0
            ty = float(parts[8]) if parts[8] else 0.0
            hx = float(parts[11]) if parts[11] else 0.0
            hy = float(parts[12]) if parts[12] else 0.0
        except ValueError:
            tx = ty = hx = hy = 0.0
        head_id = parts[9]    # head node ID (e.g. "contact@1")
        head_port = parts[10] # head port name

        cell = self.cells[self._current_cell]
        cell["arcs"].append({
            "net": net,
            "layer": norm_layer(layer_raw),
            "width": width,
            "tail": tail_id,
            "tail_port": tail_port,
            "tx": tx, "ty": ty,
            "head": head_id,
            "head_port": head_port,
            "hx": hx, "hy": hy,
        })

    def _handle_export(self, line: str):
        # E<name>||<group>|<node-id>|<port>|<direction>
        parts = line.split("|")
        if len(parts) < 4:
            return
        name = parts[0][1:]
        node_id = parts[3]
        port = parts[4] if len(parts) > 4 else ""
        direction = parts[5] if len(parts) > 5 else ""
        cell = self.cells[self._current_cell]
        cell["exports"][name] = {"node": node_id, "port": port, "dir": direction}

    def _handle_instance(self, line: str):
        # I<cell>;version{view}|<inst-name>|<group>|<x>|<y>|...
        parts = line.split("|")
        if len(parts) < 5:
            return
        cell_ref_raw = parts[0][1:]
        cell_ref = re.sub(r";[^{]*\{[^}]*\}", "", cell_ref_raw)
        inst_name = parts[1]
        try:
            x = float(parts[3]) if parts[3] else 0.0
            y = float(parts[4]) if parts[4] else 0.0
        except ValueError:
            x = y = 0.0
        mirrored = "X" in (parts[5] if len(parts) > 5 else "")
        cell = self.cells[self._current_cell]
        cell["instances"].append({
            "cell": cell_ref,
            "name": inst_name,
            "x": x,
            "y": y,
            "mirrored": mirrored,
        })


# ---------------------------------------------------------------------------
# Flatten top-level cell into world coordinates
# ---------------------------------------------------------------------------
def _net_id(net_index: dict, net_name: str) -> int:
    """Return a stable integer ID for a net name, creating one if needed."""
    if net_name not in net_index:
        net_index[net_name] = len(net_index)
    return net_index[net_name]


def _assign_transistor_port(trans_map: dict, key: tuple, port: str, net_id: int):
    """Record gate/s1/s2 connectivity for a transistor identified by key=(inst,local_id)."""
    if key not in trans_map:
        return
    t = trans_map[key]
    if port.startswith("poly"):
        t["gate_net"] = net_id
    else:  # diff-top / diff-bottom / diff-left / diff-right = source or drain
        if t.get("s1_net", -1) == -1:
            t["s1_net"] = net_id
        elif t.get("s2_net", -1) == -1 and t["s1_net"] != net_id:
            t["s2_net"] = net_id


def flatten_sm83(parser: JelibParser) -> dict:
    """
    Walk all instances in the top-level 'sm83' cell.
    For each instance, look up its child cell and place its transistors/nodes
    at the world coordinates (instance_pos + local_pos).
    Returns a dict with lists: transistors, nodes, arcs, nets, instances.

    Each transistor now carries gate_net, s1_net, s2_net — integer indices into
    the nets[] list — extracted from arc port names (poly-* = gate, diff-* = s/d).
    """
    top_cell = parser.cells.get("sm83")
    if top_cell is None:
        raise RuntimeError("Top-level 'sm83' cell not found in .jelib")

    transistors = []
    nodes = []
    arcs_flat = []

    trans_id = 0
    node_id_counter = 0

    # Maps (inst_name, local_node_id) -> global index in nodes[]
    node_global_index: dict[tuple, int] = {}

    # Maps (inst_name, local_trans_id) -> transistor dict (for port assignment)
    trans_by_key: dict[tuple, dict] = {}

    # Global net name -> integer ID table
    net_index: dict[str, int] = {}

    def _process_cell_arcs(cell_arcs, inst_name, ix, iy, mirror):
        """Flatten a cell's arcs into world coords and annotate transistor ports."""
        for arc in cell_arcs:
            tail_global = node_global_index.get((inst_name, arc["tail"]), -1)
            head_global = node_global_index.get((inst_name, arc["head"]), -1)
            net = arc["net"]
            nid = _net_id(net_index, net)
            # Annotate transistor gate/source/drain from port names
            tail_port = arc.get("tail_port", "")
            head_port = arc.get("head_port", "")
            _assign_transistor_port(trans_by_key, (inst_name, arc["tail"]), tail_port, nid)
            _assign_transistor_port(trans_by_key, (inst_name, arc["head"]), head_port, nid)
            arcs_flat.append({
                "net": net,
                "net_id": nid,
                "layer": arc["layer"],
                "width": arc["width"],
                "inst": inst_name,
                "tx": ix + (-arc["tx"] if mirror else arc["tx"]),
                "ty": iy + arc["ty"],
                "hx": ix + (-arc["hx"] if mirror else arc["hx"]),
                "hy": iy + arc["hy"],
                "tail_node": tail_global,
                "head_node": head_global,
            })

    # --- instances from top-level cell ---
    for inst in top_cell["instances"]:
        child_name = inst["cell"]
        child = parser.cells.get(child_name)
        if child is None:
            continue

        ix, iy = inst["x"], inst["y"]
        mirror = inst["mirrored"]
        inst_name = inst["name"]

        for nid, node in child["nodes"].items():
            layer = node["layer"]
            lx = node["x"]
            ly = node["y"]
            wx = ix + (-lx if mirror else lx)
            wy = iy + ly

            if layer in ("ntrans", "ptrans"):
                t = {
                    "id": trans_id,
                    "local_id": nid,
                    "cell": child_name,
                    "inst": inst_name,
                    "layer": layer,
                    "x": wx,
                    "y": wy,
                    "w": node["w"],
                    "h": node["h"],
                    "gate_net": -1,
                    "s1_net": -1,
                    "s2_net": -1,
                }
                transistors.append(t)
                trans_by_key[(inst_name, nid)] = t
                trans_id += 1
            else:
                node_global_index[(inst_name, nid)] = node_id_counter
                nodes.append({
                    "id": node_id_counter,
                    "local_id": nid,
                    "cell": child_name,
                    "inst": inst_name,
                    "layer": layer,
                    "x": wx,
                    "y": wy,
                    "w": node["w"],
                    "h": node["h"],
                })
                node_id_counter += 1

        _process_cell_arcs(child["arcs"], inst_name, ix, iy, mirror)

    # --- direct nodes/arcs in top-level sm83 cell ---
    for nid, node in top_cell["nodes"].items():
        layer = node["layer"]
        if layer in ("ntrans", "ptrans"):
            t = {
                "id": trans_id,
                "local_id": nid,
                "cell": "sm83",
                "inst": "sm83",
                "layer": layer,
                "x": node["x"],
                "y": node["y"],
                "w": node["w"],
                "h": node["h"],
                "gate_net": -1,
                "s1_net": -1,
                "s2_net": -1,
            }
            transistors.append(t)
            trans_by_key[("sm83", nid)] = t
            trans_id += 1
        else:
            node_global_index[("sm83", nid)] = node_id_counter
            nodes.append({
                "id": node_id_counter,
                "local_id": nid,
                "cell": "sm83",
                "inst": "sm83",
                "layer": layer,
                "x": node["x"],
                "y": node["y"],
                "w": node["w"],
                "h": node["h"],
            })
            node_id_counter += 1

    _process_cell_arcs(top_cell["arcs"], "sm83", 0.0, 0.0, False)

    # Build sorted net list (index == net_id)
    nets = [""] * len(net_index)
    for name, idx in net_index.items():
        nets[idx] = name

    # Compute bounding box
    all_xs = [n["x"] for n in transistors + nodes] + \
             [a["tx"] for a in arcs_flat] + [a["hx"] for a in arcs_flat]
    all_ys = [n["y"] for n in transistors + nodes] + \
             [a["ty"] for a in arcs_flat] + [a["hy"] for a in arcs_flat]

    bbox = {
        "x_min": min(all_xs) if all_xs else 0,
        "x_max": max(all_xs) if all_xs else 0,
        "y_min": min(all_ys) if all_ys else 0,
        "y_max": max(all_ys) if all_ys else 0,
    }

    instances_named = []
    for inst in top_cell["instances"]:
        instances_named.append({
            "name": inst["name"],
            "cell": inst["cell"],
            "x": inst["x"],
            "y": inst["y"],
            "mirrored": inst["mirrored"],
        })

    from collections import Counter
    trans_per_layer = Counter(t["layer"] for t in transistors)
    node_per_layer  = Counter(n["layer"] for n in nodes)
    arc_per_layer   = Counter(a["layer"] for a in arcs_flat)

    # Connectivity stats
    gate_ok = sum(1 for t in transistors if t["gate_net"] >= 0)
    sd_ok   = sum(1 for t in transistors if t["s1_net"] >= 0)

    return {
        "bbox": bbox,
        "transistors": transistors,
        "nodes": nodes,
        "arcs": arcs_flat,
        "nets": nets,
        "instances": instances_named,
        "stats": {
            "n_transistors": len(transistors),
            "n_nodes": len(nodes),
            "n_arcs": len(arcs_flat),
            "n_nets": len(nets),
            "n_instances": len(instances_named),
            "cells": len(parser.cells),
            "transistors_per_layer": dict(trans_per_layer),
            "nodes_per_layer": dict(node_per_layer),
            "arcs_per_layer": dict(arc_per_layer),
            "transistors_with_gate": gate_ok,
            "transistors_with_sd": sd_ok,
        },
    }


# ---------------------------------------------------------------------------
# Normalize coordinates to [0, 1] space
# ---------------------------------------------------------------------------
def normalize(data: dict) -> dict:
    bbox = data["bbox"]
    xmin, xmax = bbox["x_min"], bbox["x_max"]
    ymin, ymax = bbox["y_min"], bbox["y_max"]
    xrange = xmax - xmin or 1.0
    yrange = ymax - ymin or 1.0

    def nx(v): return (v - xmin) / xrange
    def ny(v): return (v - ymin) / yrange

    def nw(v): return v / xrange
    def nh(v): return v / yrange

    for t in data["transistors"]:
        t["nx"] = nx(t["x"])
        t["ny"] = ny(t["y"])
        t["nw"] = nw(t.get("w", 0.0))
        t["nh"] = nh(t.get("h", 0.0))
    for n in data["nodes"]:
        n["nx"] = nx(n["x"])
        n["ny"] = ny(n["y"])
        n["nw"] = nw(n.get("w", 0.0))
        n["nh"] = nh(n.get("h", 0.0))
    for a in data["arcs"]:
        a["ntx"] = nx(a["tx"])
        a["nty"] = ny(a["ty"])
        a["nhx"] = nx(a["hx"])
        a["nhy"] = ny(a["hy"])
    for inst in data["instances"]:
        inst["nx"] = nx(inst["x"])
        inst["ny"] = ny(inst["y"])
    return data


# ---------------------------------------------------------------------------
# JSON output
# ---------------------------------------------------------------------------
def write_json(data: dict, path: str):
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
    print(f"  JSON: {path}  ({Path(path).stat().st_size // 1024} KB)")


# ---------------------------------------------------------------------------
# C/H code generation
# ---------------------------------------------------------------------------
C_HEADER_COMMENT = """\
/* sm83_netlist_data.h  -- AUTO-GENERATED by tools/jelib_extractor.py -- DO NOT EDIT */
#pragma once
#include <stdint.h>

"""

C_SOURCE_COMMENT = """\
/* sm83_netlist_data.c  -- AUTO-GENERATED by tools/jelib_extractor.py -- DO NOT EDIT */
#include "sm83_netlist_data.h"

"""


def _compute_layer_ranges(items: list, id_field: str = "layer") -> dict:
    """Return {layer_id: (start, end)} ranges assuming items are sorted by layer."""
    ranges = {}
    for i, item in enumerate(items):
        lid = item[id_field] if isinstance(item[id_field], int) else LAYER_IDS.get(item[id_field], 0)
        if lid not in ranges:
            ranges[lid] = [i, i + 1]
        else:
            ranges[lid][1] = i + 1
    return {k: tuple(v) for k, v in ranges.items()}


def write_c(data: dict, c_path: str, h_path: str):
    stats = data["stats"]
    bbox = data["bbox"]
    instances = data["instances"]
    nets = data.get("nets", [])

    # Sort transistors, nodes and arcs by layer so ranges are contiguous
    transistors = sorted(data["transistors"], key=lambda t: LAYER_IDS.get(t["layer"], 0))
    nodes       = sorted(data["nodes"],       key=lambda n: LAYER_IDS.get(n["layer"], 0))
    arcs        = sorted(data["arcs"],        key=lambda a: LAYER_IDS.get(a["layer"], 0))

    trans_ranges = _compute_layer_ranges(transistors)
    node_ranges  = _compute_layer_ranges(nodes)
    arc_ranges   = _compute_layer_ranges(arcs)

    # -------- .h --------
    h_lines = [C_HEADER_COMMENT]

    h_lines.append("/* Layer IDs */\n")
    for name, lid in LAYER_IDS.items():
        h_lines.append(f"#define SM83_LAYER_{name.upper()}  {lid}\n")
    h_lines.append("\n")

    h_lines.append("/* Counts */\n")
    h_lines.append(f"#define SM83_TRANSISTOR_COUNT  {stats['n_transistors']}\n")
    h_lines.append(f"#define SM83_NODE_COUNT        {stats['n_nodes']}\n")
    h_lines.append(f"#define SM83_ARC_COUNT         {stats['n_arcs']}\n")
    h_lines.append(f"#define SM83_NET_COUNT         {stats.get('n_nets', 0)}\n")
    h_lines.append(f"#define SM83_INSTANCE_COUNT    {stats['n_instances']}\n")
    h_lines.append("\n")

    h_lines.append("/* Die bounding box (Electric VLSI units) */\n")
    h_lines.append(f"#define SM83_BBOX_X_MIN  {bbox['x_min']:.2f}f\n")
    h_lines.append(f"#define SM83_BBOX_X_MAX  {bbox['x_max']:.2f}f\n")
    h_lines.append(f"#define SM83_BBOX_Y_MIN  {bbox['y_min']:.2f}f\n")
    h_lines.append(f"#define SM83_BBOX_Y_MAX  {bbox['y_max']:.2f}f\n")
    h_lines.append("\n")

    # Per-layer index ranges (start inclusive, end exclusive) for transistors/nodes/arcs.
    # Arrays are sorted by layer so iterating [START, END) gives all shapes on that layer.
    LAYER_NAMES_UPPER = {v: k.upper() for k, v in LAYER_IDS.items()}
    h_lines.append("/* Per-layer index ranges into sm83_transistors[] (sorted by layer) */\n")
    for lid in sorted(LAYER_IDS.values()):
        lname = LAYER_NAMES_UPPER[lid]
        s, e = trans_ranges.get(lid, (0, 0))
        h_lines.append(f"#define SM83_TRANS_{lname}_START  {s}\n")
        h_lines.append(f"#define SM83_TRANS_{lname}_END    {e}\n")
    h_lines.append("\n")

    h_lines.append("/* Per-layer index ranges into sm83_nodes[] (sorted by layer) */\n")
    for lid in sorted(LAYER_IDS.values()):
        lname = LAYER_NAMES_UPPER[lid]
        s, e = node_ranges.get(lid, (0, 0))
        h_lines.append(f"#define SM83_NODE_{lname}_START  {s}\n")
        h_lines.append(f"#define SM83_NODE_{lname}_END    {e}\n")
    h_lines.append("\n")

    h_lines.append("/* Per-layer index ranges into sm83_arcs[] (sorted by layer) */\n")
    for lid in sorted(LAYER_IDS.values()):
        lname = LAYER_NAMES_UPPER[lid]
        s, e = arc_ranges.get(lid, (0, 0))
        h_lines.append(f"#define SM83_ARC_{lname}_START  {s}\n")
        h_lines.append(f"#define SM83_ARC_{lname}_END    {e}\n")
    h_lines.append("\n")

    h_lines.append("""\
/* Transistor: normalized position and size (nx,ny,nw,nh in [0,1]), layer (0=ntrans,1=ptrans).
 * gate_net/s1_net/s2_net are indices into sm83_nets[]; -1 = unresolved. */
typedef struct {
    float nx, ny;     /* normalized die coordinates (center) */
    float nw, nh;     /* normalized width/height */
    float x, y;       /* raw Electric VLSI units */
    float w, h;       /* raw width/height */
    uint8_t layer;    /* SM83_LAYER_NTRANS or SM83_LAYER_PTRANS */
    int32_t gate_net; /* index into sm83_nets[], -1 = unknown */
    int32_t s1_net;   /* source/drain net 1, -1 = unknown */
    int32_t s2_net;   /* source/drain net 2, -1 = unknown */
} Sm83Transistor;

/* Geometry node (contact, pin, active area, poly, metal pin) */
typedef struct {
    float nx, ny;   /* normalized die coordinates (center) */
    float nw, nh;   /* normalized width/height */
    float x, y;     /* raw Electric VLSI units */
    float w, h;     /* raw width/height */
    uint8_t layer;
} Sm83Node;

/* Arc (wire segment between two nodes) */
typedef struct {
    float ntx, nty, nhx, nhy;    /* normalized tail/head positions */
    float tx, ty, hx, hy;         /* raw coordinates */
    float width;
    uint8_t layer;
    int32_t tail_node, head_node; /* indices into sm83_nodes[], -1 if unknown */
    int32_t net_id;               /* index into sm83_nets[], -1 if unknown */
} Sm83Arc;

/* Named instance (sub-cell placement in top-level die) */
typedef struct {
    float nx, ny;
    float x, y;
    const char *name;
    const char *cell;
} Sm83Instance;

extern const Sm83Transistor sm83_transistors[SM83_TRANSISTOR_COUNT];
extern const Sm83Node       sm83_nodes[SM83_NODE_COUNT];
extern const Sm83Arc        sm83_arcs[SM83_ARC_COUNT];
extern const char * const   sm83_nets[SM83_NET_COUNT];
extern const Sm83Instance   sm83_instances[SM83_INSTANCE_COUNT];
""")

    with open(h_path, "w") as f:
        f.writelines(h_lines)
    print(f"  Header: {h_path}")

    # -------- .c --------
    c_lines = [C_SOURCE_COMMENT]

    # transistors
    c_lines.append(f"const Sm83Transistor sm83_transistors[SM83_TRANSISTOR_COUNT] = {{\n")
    for t in transistors:
        lid = LAYER_IDS.get(t["layer"], 0)
        c_lines.append(
            f"    {{ {t['nx']:.6f}f, {t['ny']:.6f}f,  "
            f"{t.get('nw', 0.0):.6f}f, {t.get('nh', 0.0):.6f}f,  "
            f"{t['x']:.2f}f, {t['y']:.2f}f,  "
            f"{t.get('w', 0.0):.2f}f, {t.get('h', 0.0):.2f}f,  {lid},  "
            f"{t.get('gate_net', -1)}, {t.get('s1_net', -1)}, {t.get('s2_net', -1)} }},\n"
        )
    c_lines.append("};\n\n")

    # nodes
    c_lines.append(f"const Sm83Node sm83_nodes[SM83_NODE_COUNT] = {{\n")
    for n in nodes:
        lid = LAYER_IDS.get(n["layer"], 0)
        c_lines.append(
            f"    {{ {n['nx']:.6f}f, {n['ny']:.6f}f,  "
            f"{n.get('nw', 0.0):.6f}f, {n.get('nh', 0.0):.6f}f,  "
            f"{n['x']:.2f}f, {n['y']:.2f}f,  "
            f"{n.get('w', 0.0):.2f}f, {n.get('h', 0.0):.2f}f,  {lid} }},\n"
        )
    c_lines.append("};\n\n")

    # arcs
    c_lines.append(f"const Sm83Arc sm83_arcs[SM83_ARC_COUNT] = {{\n")
    for a in arcs:
        lid = LAYER_IDS.get(a["layer"], 0)
        tail_n = a.get("tail_node", -1)
        head_n = a.get("head_node", -1)
        net_id = a.get("net_id", -1)
        c_lines.append(
            f"    {{ {a['ntx']:.6f}f, {a['nty']:.6f}f, {a['nhx']:.6f}f, {a['nhy']:.6f}f,  "
            f"{a['tx']:.2f}f, {a['ty']:.2f}f, {a['hx']:.2f}f, {a['hy']:.2f}f,  "
            f"{a['width']:.2f}f, {lid}, {tail_n}, {head_n}, {net_id} }},\n"
        )
    c_lines.append("};\n\n")

    # nets
    c_lines.append(f"const char * const sm83_nets[SM83_NET_COUNT] = {{\n")
    for name in nets:
        name_esc = name.replace('\\', '\\\\').replace('"', '\\"')
        c_lines.append(f'    "{name_esc}",\n')
    c_lines.append("};\n\n")

    # instances
    c_lines.append(f"const Sm83Instance sm83_instances[SM83_INSTANCE_COUNT] = {{\n")
    for inst in instances:
        name_esc = inst["name"].replace('"', '\\"')
        cell_esc = inst["cell"].replace('"', '\\"')
        c_lines.append(
            f'    {{ {inst["nx"]:.6f}f, {inst["ny"]:.6f}f,  '
            f'{inst["x"]:.2f}f, {inst["y"]:.2f}f,  '
            f'"{name_esc}", "{cell_esc}" }},\n'
        )
    c_lines.append("};\n")

    with open(c_path, "w") as f:
        f.writelines(c_lines)
    print(f"  Source: {c_path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="Extract SM83 .jelib -> C data")
    parser.add_argument("--jelib", default="/tmp/dmg-schematics/sm83_cells/sm83.jelib")
    parser.add_argument("--out-dir", default=".")
    parser.add_argument("--no-json", action="store_true")
    args = parser.parse_args()

    out = Path(args.out_dir)

    print(f"Parsing {args.jelib} ...")
    jp = JelibParser(args.jelib)
    print(f"  Parsed {len(jp.cells)} cells")

    print("Flattening top-level sm83 cell ...")
    data = flatten_sm83(jp)
    s = data["stats"]
    print(f"  Transistors : {s['n_transistors']}  (N+P)")
    for lname, cnt in sorted(s["transistors_per_layer"].items()):
        print(f"    {lname}: {cnt}")
    print(f"  Nodes       : {s['n_nodes']}")
    for lname, cnt in sorted(s["nodes_per_layer"].items()):
        print(f"    {lname}: {cnt}")
    print(f"  Arcs        : {s['n_arcs']}")
    for lname, cnt in sorted(s["arcs_per_layer"].items()):
        print(f"    {lname}: {cnt}")
    print(f"  Nets        : {s.get('n_nets', 0)}")
    print(f"  Instances   : {s['n_instances']}")
    print(f"  Trans with gate : {s.get('transistors_with_gate', 0)} / {s['n_transistors']}")
    print(f"  Trans with s/d  : {s.get('transistors_with_sd', 0)} / {s['n_transistors']}")
    bbox = data["bbox"]
    print(f"  BBox        : ({bbox['x_min']:.1f},{bbox['y_min']:.1f}) -> ({bbox['x_max']:.1f},{bbox['y_max']:.1f})")

    print("Normalizing coordinates ...")
    normalize(data)

    print("Writing output ...")
    if not args.no_json:
        write_json(data, str(out / "data" / "sm83_netlist.json"))
    write_c(data, str(out / "sm83" / "sm83_netlist_data.c"), str(out / "sm83" / "sm83_netlist_data.h"))

    print("Done.")


if __name__ == "__main__":
    main()
