#!/usr/bin/env bash
# screenshot.sh — capture a PNG of the currently running headless compositor
# (started by run-headless.sh, or manually) to the path given as $1.
#
# Usage:
#   ./screenshot.sh /path/to/output.png
#
# Auto-detects which backend is active from the environment:
#   - If WAYLAND_DISPLAY is set: uses `grim` (works under sway and weston,
#     as long as the compositor implements wlr-screencopy / the relevant
#     screenshot protocol -- sway does; weston's headless-backend also
#     exposes screenshooting via `weston-screenshooter` as a fallback).
#   - Else if DISPLAY is set: uses ImageMagick's `import -window root`
#     (X11/Xvfb path).
#
# This script must be run with the same environment (XDG_RUNTIME_DIR,
# WAYLAND_DISPLAY / DISPLAY) that the compositor and app were started with.
# If you used run-headless.sh directly to launch your app (recommended),
# take the screenshot from a second command wrapped by run-headless.sh
# pointed at the same running compositor is NOT directly supported since
# run-headless.sh tears the compositor down when its command exits; instead,
# either:
#   (a) have your test harness / app itself call this script (or shell out)
#       while it is still running under run-headless.sh, or
#   (b) start the compositor persistently yourself (copy the relevant case
#       branch out of run-headless.sh) and call screenshot.sh against it.
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <output.png>" >&2
  exit 2
fi

OUT="$1"
mkdir -p "$(dirname -- "$OUT")"

if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
  if command -v grim >/dev/null 2>&1; then
    if grim "$OUT" 2>/tmp/screenshot-grim-err.log; then
      echo "Wrote screenshot (grim, Wayland): $OUT" >&2
      exit 0
    else
      echo "grim failed:" >&2
      cat /tmp/screenshot-grim-err.log >&2
      # Fall through to weston-screenshooter if available.
    fi
  fi
  if command -v weston-screenshooter >/dev/null 2>&1; then
    # weston-screenshooter writes to the current directory by default.
    TMP_SHOT_DIR="$(mktemp -d)"
    (cd "$TMP_SHOT_DIR" && weston-screenshooter)
    SHOT="$(find "$TMP_SHOT_DIR" -name '*.png' | head -n1)"
    if [[ -n "$SHOT" ]]; then
      mv "$SHOT" "$OUT"
      rm -rf "$TMP_SHOT_DIR"
      echo "Wrote screenshot (weston-screenshooter): $OUT" >&2
      exit 0
    fi
    rm -rf "$TMP_SHOT_DIR"
  fi
  echo "ERROR: WAYLAND_DISPLAY is set but no working screenshot tool found (tried grim, weston-screenshooter)." >&2
  exit 1

elif [[ -n "${DISPLAY:-}" ]]; then
  if command -v import >/dev/null 2>&1; then
    import -window root "$OUT"
    echo "Wrote screenshot (ImageMagick import, X11): $OUT" >&2
    exit 0
  fi
  echo "ERROR: DISPLAY is set but ImageMagick's 'import' is not installed." >&2
  exit 1

else
  echo "ERROR: neither WAYLAND_DISPLAY nor DISPLAY is set. Run this from inside" >&2
  echo "       run-headless.sh's session, or export the matching display var." >&2
  exit 1
fi
