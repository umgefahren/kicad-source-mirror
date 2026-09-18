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

# Each fixture source must be a project ROOT sheet sitting beside its
# .kicad_pro. Two constraints, both currently real:
#   - the loader reaches through the project while constructing the SCHEMATIC,
#     so a schematic with no .kicad_pro beside it is not loadable;
#   - a child sheet has to be reached through its root's hierarchy rather than
#     opened directly, so pass the root here and select sheets with --sheet.
gen "demos/ecc83/ecc83-pp_v2.kicad_sch"                      ecc83_pp_v2
gen "demos/complex_hierarchy/complex_hierarchy.kicad_sch"    complex_hierarchy
# Not checked in: at ~1.1 MB its stream is too large to carry in the tree.
# Generate it locally when a bigger, denser sheet is wanted:
#   gen "demos/pic_programmer/pic_programmer.kicad_sch"      pic_programmer
gen "demos/simulation/sallen_key/sallen_key.kicad_sch"       sallen_key

echo "done"
