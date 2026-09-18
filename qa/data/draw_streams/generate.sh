#!/bin/sh
# Regenerate the golden draw streams in this directory.
#
# These are recorded by kicad-sch-dump from schematics already in the tree, so
# they are derived data — but they are checked in on purpose, because they let
# the Rust decoder and renderer be developed and regression-tested without
# linking any C++ at all.
#
# Usage:  qa/data/draw_streams/generate.sh <path-to-kicad-sch-dump>
#
# The viewport is pinned so that the output is reproducible: the recorded
# geometry is in world coordinates and does not depend on it, but the frame's
# BEGIN_FRAME dimensions and the grid command do.

set -e

DUMP="${1:?usage: generate.sh <path-to-kicad-sch-dump>}"
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)

VIEWPORT="-W 1920 -H 1080"

gen() # <source .kicad_sch, relative to the repo root> <output basename>
{
    echo "recording $1 -> $2.kgds"
    "$DUMP" $VIEWPORT --sheet 0 -o "$HERE/$2.kgds" "$ROOT/$1" > "$HERE/$2.txt"
}

# Pass a project ROOT sheet here. A child sheet has to be reached through its
# root's hierarchy rather than opened directly, so select sheets with --sheet
# rather than naming the child file.
#
# A sibling .kicad_pro is NOT required: SCH_HOST stands up the empty project
# when the settings manager has none, which is what a project-less schematic
# falls back to. api_kitchen_sink below is exactly that case.

# Tier 0 — smoke. Small, one sheet, covers wires, junctions, labels, symbols.
gen "demos/ecc83/ecc83-pp_v2.kicad_sch"                      ecc83_pp_v2

# Tier 1 — opcode coverage. The only file in the tree that exercises bitmaps,
# beziers, tables, transforms and text boxes at once, so it is the one that
# tells a renderer which opcode it is missing.
gen "qa/data/eeschema/api_kitchen_sink.kicad_sch"            api_kitchen_sink

# Tier 2 — hierarchy: the same screen reached at more than one path.
gen "demos/complex_hierarchy/complex_hierarchy.kicad_sch"    complex_hierarchy

# Tier 2 — a small, dense analog sheet with simulation fields.
gen "demos/simulation/sallen_key/sallen_key.kicad_sch"       sallen_key

# Not checked in: at ~1.1 MB its stream is too large to carry in the tree.
# Generate it locally when a bigger, denser sheet is wanted:
#   gen "demos/pic_programmer/pic_programmer.kicad_sch"      pic_programmer

echo "done"
