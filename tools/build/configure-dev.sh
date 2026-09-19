#!/usr/bin/env bash
#
# configure-dev.sh -- configure a KiCad dev build tree for the Rust/eeschema migration work.
#
# This reproduces the exact configure that is known to work in the project container
# (Ubuntu 24.04, gcc 13.3, cmake 3.28, wxWidgets 3.2.4 GTK3).
#
# Usage:
#   tools/build/configure-dev.sh [BUILD_DIR] [-- <extra cmake args>]
#
# Defaults:
#   SRC_DIR   = the repo this script lives in
#   BUILD_DIR = $KICAD_BUILD_DIR, or /tmp/claude-0/kicad-build/build
#
# See docs/rust-migration/03-build-notes.md for the apt package list and target names.

set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

BUILD_DIR="${KICAD_BUILD_DIR:-/tmp/claude-0/kicad-build/build}"
EXTRA_ARGS=()

# First non-"--" argument is the build dir; everything after "--" goes to cmake.
if [[ $# -gt 0 && "$1" != "--" ]]; then
    BUILD_DIR="$1"
    shift
fi
if [[ ${#} -gt 0 && "${1:-}" == "--" ]]; then
    shift
    EXTRA_ARGS=("$@")
fi

# ccache is optional but makes rebuilds dramatically cheaper. Only wire it up if present.
LAUNCHER_ARGS=()
if command -v ccache >/dev/null 2>&1; then
    LAUNCHER_ARGS+=(
        -DCMAKE_C_COMPILER_LAUNCHER=ccache
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
    )
else
    echo "warning: ccache not found; builds will not be cached." >&2
fi

GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
    GENERATOR="Ninja"
else
    echo "warning: ninja not found; falling back to Unix Makefiles (much slower)." >&2
fi

echo "Source dir : $SRC_DIR"
echo "Build dir  : $BUILD_DIR"
echo "Generator  : $GENERATOR"

mkdir -p "$BUILD_DIR"

# Notes on the options below:
#  * CMAKE_BUILD_TYPE=Release            -- keeps the build tree small; RelWithDebInfo roughly
#                                           triples the object-file footprint.
#  * KICAD_BUILD_QA_TESTS=ON             -- needed for qa_eeschema / qa_common.
#  * KICAD_BUILD_I18N=OFF                -- default; skips the translation catalogs.
#  * KICAD_USE_PCH=ON                    -- default; precompiled headers, a large win on eeschema.
#  * KICAD_UPDATE_CHECK=OFF              -- no phone-home from a dev build.
#  * KICAD_USE_SENTRY=OFF                -- default; no crash telemetry.
#
# Deliberately NOT passed (these options do not exist in this tree -- see the build notes):
#   KICAD_SCRIPTING_WXPYTHON, KICAD_USE_EGL, KICAD_USE_OCC, KICAD_SPICE, KICAD_IPC_API
# Passing them is harmless but produces "Manually-specified variables were not used" warnings.

exec cmake -S "$SRC_DIR" -B "$BUILD_DIR" -G "$GENERATOR" \
    -DCMAKE_BUILD_TYPE=Release \
    "${LAUNCHER_ARGS[@]}" \
    -DKICAD_BUILD_QA_TESTS=ON \
    -DKICAD_BUILD_I18N=OFF \
    -DKICAD_UPDATE_CHECK=OFF \
    -DKICAD_INSTALL_DEMOS=OFF \
    "${EXTRA_ARGS[@]}"
