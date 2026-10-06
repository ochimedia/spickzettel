#!/usr/bin/env python3
"""Converts the icon SVGs in assets/icons/ into src/ui/icons_generated.h
- a C++ data table of drawing commands replayable through Dear ImGui's
ImDrawList, so icons render as true vector shapes (tessellated at whatever
size they're drawn) instead of a rasterized icon-font atlas baked at one
fixed size.

Not part of the CMake build - run by hand whenever assets/icons/*.svg
changes, and the generated header committed like any other source file.
Requires `pip install svgpathtools` (not a build-time dependency of the
project itself, only of this one dev-time script).

Only handles what this icon set actually needs: path `d` commands M/L/C/Q/A
(A restricted to circular, unrotated arcs - see emit_arc_circular), plus
plain <circle>, <rect>, <line> elements. Extend it if a future icon needs
more (an elliptical or rotated arc would need the general SVG-arc-to-bezier
conversion this deliberately doesn't implement, since nothing here uses it).
"""
import math
import os
import xml.etree.ElementTree as ET
from svgpathtools import parse_path, Line, CubicBezier, QuadraticBezier, Arc

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
SVG_DIR = os.path.join(REPO_ROOT, "assets", "icons")
OUT_HEADER = os.path.join(REPO_ROOT, "src", "ui", "icons_generated.h")

ICONS = [
    "pen", "eraser", "eraser-rect", "camera", "layout-grid",
    "layer-down", "layer-up", "move",
    "copy", "trash", "plus", "x", "undo", "redo", "rectangle", "line", "type",
    "minimize", "maximize", "restore", "more-vertical", "target", "note",
    "pin", "select", "scissors", "clipboard", "keyboard", "settings",
]


def cpp_ident(name):
    return "".join(w.capitalize() for w in name.split("-"))


def emit_moveto(cmds, pt):
    cmds.append(("MoveTo", pt.real, pt.imag, 0, 0, 0, 0))


def emit_lineto(cmds, pt):
    cmds.append(("LineTo", pt.real, pt.imag, 0, 0, 0, 0))


def emit_cubicto(cmds, c1, c2, end):
    cmds.append(("CubicTo", c1.real, c1.imag, c2.real, c2.imag, end.real, end.imag))


def emit_arc_circular(cmds, center, radius, start_pt, end_pt, sweep):
    # Only handles circular arcs (rx == ry, no rotation) - verified true for
    # every arc in this icon set before relying on it; process_path_d raises
    # for anything else rather than silently mis-rendering it.
    a0 = math.atan2(start_pt.imag - center.imag, start_pt.real - center.real)
    a1 = math.atan2(end_pt.imag - center.imag, end_pt.real - center.real)
    # Normalize sweep direction: SVG sweep-flag=1 means positive-angle
    # (clockwise in a y-down system, which matches screen/ImGui space
    # directly - no extra flip needed going from SVG to ImGui coords).
    if sweep == 1:
        while a1 < a0:
            a1 += 2 * math.pi
    else:
        while a1 > a0:
            a1 -= 2 * math.pi
    cmds.append(("ArcTo", center.real, center.imag, radius, a0, a1, 0))


def process_path_d(d, cmds):
    path = parse_path(d)
    cur = None
    for seg in path:
        start = seg.start
        if cur is None or abs(start - cur) > 1e-6:
            if cur is not None:
                cmds.append(("EndSubpath", 0, 0, 0, 0, 0, 0))
            emit_moveto(cmds, start)
        if isinstance(seg, Line):
            emit_lineto(cmds, seg.end)
        elif isinstance(seg, CubicBezier):
            emit_cubicto(cmds, seg.control1, seg.control2, seg.end)
        elif isinstance(seg, QuadraticBezier):
            # Promote quadratic to cubic (not used by this icon set, but
            # handled in case a future icon needs it).
            c1 = seg.start + 2.0 / 3.0 * (seg.control - seg.start)
            c2 = seg.end + 2.0 / 3.0 * (seg.control - seg.end)
            emit_cubicto(cmds, c1, c2, seg.end)
        elif isinstance(seg, Arc):
            rx, ry = seg.radius.real, seg.radius.imag
            if abs(rx - ry) > 1e-3 or abs(seg.rotation) > 1e-3:
                raise ValueError(f"non-circular arc not supported: {seg}")
            emit_arc_circular(cmds, seg.center, rx, seg.start, seg.end, seg.sweep)
        else:
            raise ValueError(f"unsupported segment type: {seg}")
        cur = seg.end
    cmds.append(("EndSubpath", 0, 0, 0, 0, 0, 0))


def process_svg(path):
    tree = ET.parse(path)
    root = tree.getroot()
    cmds = []
    for el in root:
        tag = el.tag.split("}")[-1]
        if tag == "path":
            process_path_d(el.attrib["d"], cmds)
        elif tag == "circle":
            cx, cy, r = float(el.attrib["cx"]), float(el.attrib["cy"]), float(el.attrib["r"])
            cmds.append(("Circle", cx, cy, r, 0, 0, 0))
        elif tag == "rect":
            x, y = float(el.attrib["x"]), float(el.attrib["y"])
            w, h = float(el.attrib["width"]), float(el.attrib["height"])
            rx = float(el.attrib.get("rx", 0))
            cmds.append(("RoundedRect", x, y, x + w, y + h, rx, 0))
        elif tag == "line":
            x1, y1 = float(el.attrib["x1"]), float(el.attrib["y1"])
            x2, y2 = float(el.attrib["x2"]), float(el.attrib["y2"])
            cmds.append(("MoveTo", x1, y1, 0, 0, 0, 0))
            cmds.append(("LineTo", x2, y2, 0, 0, 0, 0))
            cmds.append(("EndSubpath", 0, 0, 0, 0, 0, 0))
        else:
            raise ValueError(f"unsupported element: {tag}")
    return cmds


def main():
    all_icons = {}
    for name in ICONS:
        all_icons[name] = process_svg(os.path.join(SVG_DIR, f"{name}.svg"))

    lines = []
    lines.append("// Generated by scripts/gen_icons.py from assets/icons/*.svg - do not edit")
    lines.append("// by hand, regenerate instead. See IconOp's doc comment (icon_draw.h) for")
    lines.append("// what each command's fields mean.")
    lines.append("#pragma once")
    lines.append("")
    lines.append('#include "ui/icon_draw.h"')
    lines.append("")
    lines.append("namespace sz::ui::icons {")
    lines.append("")
    for name, cmds in all_icons.items():
        ident = cpp_ident(name)
        lines.append(f"constexpr IconCmd k{ident}Cmds[] = {{")
        for op, a, b, c, d, e, f in cmds:
            lines.append(f"    {{IconOp::{op}, {a:.4f}f, {b:.4f}f, {c:.4f}f, {d:.4f}f, {e:.4f}f, {f:.4f}f}},")
        lines.append("};")
        lines.append(f"constexpr Icon k{ident} = {{k{ident}Cmds, IM_ARRAYSIZE(k{ident}Cmds)}};")
        lines.append("")
    lines.append("}  // namespace sz::ui::icons")
    lines.append("")

    with open(OUT_HEADER, "w") as fh:
        fh.write("\n".join(lines))
    print(f"wrote {OUT_HEADER}")
    for name, cmds in all_icons.items():
        print(f"  {name}: {len(cmds)} commands")


if __name__ == "__main__":
    main()
