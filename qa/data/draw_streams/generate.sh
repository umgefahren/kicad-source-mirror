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

gen "demos/ecc83/ecc83-pp_v2.kicad_sch"                      ecc83_pp_v2
gen "qa/data/eeschema/api_kitchen_sink.kicad_sch"            api_kitchen_sink
gen "demos/complex_hierarchy/complex_hierarchy.kicad_sch"    complex_hierarchy
gen "demos/pic_programmer/pic_sockets.kicad_sch"             pic_sockets

echo "done"
