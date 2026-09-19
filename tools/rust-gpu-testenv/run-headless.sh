#!/usr/bin/env bash
# run-headless.sh — start a headless Wayland compositor (or Xvfb), export the
# env vars a wgpu/gpui app needs to run against software Vulkan/GL, run the
# command given as "$@" inside that session, then tear everything down.
#
# Usage:
#   ./run-headless.sh my-binary --flag
#   TESTENV_BACKEND=x11 ./run-headless.sh my-binary --flag
#   TESTENV_BACKEND=weston ./run-headless.sh my-binary --flag
#
# Env vars you can set to control behavior:
#   TESTENV_BACKEND   "sway" (default), "weston", or "x11".
#   TESTENV_WIDTH     output width  (default 1280)
#   TESTENV_HEIGHT    output height (default 720)
#   TESTENV_KEEP_LOG  if set to 1, don't delete the compositor log on exit
#   TESTENV_ENV_FILE  if set, this script writes the live session's env vars
#                     (XDG_RUNTIME_DIR, WAYLAND_DISPLAY/DISPLAY, VK_ICD_FILENAMES,
#                     ...) to this path as `export KEY=VALUE` lines as soon as
#                     the compositor is up, and removes it on cleanup. This
#                     lets a second, concurrent shell take a screenshot of the
#                     app while it is running, e.g.:
#                       TESTENV_ENV_FILE=/tmp/te.env ./run-headless.sh ./my-app --run-seconds 10 &
#                       until [[ -f /tmp/te.env ]]; do sleep 0.2; done
#                       sleep 1  # let the app's window map
#                       ( source /tmp/te.env && ./screenshot.sh /tmp/shot.png )
#                       wait
#
# The command's exit code is propagated as this script's exit code.
#
# What this script exports into the child command's environment:
#   XDG_RUNTIME_DIR        fresh, private, mode-0700 runtime dir
#   WAYLAND_DISPLAY        (Wayland backends only) the compositor's socket name
#   DISPLAY                (x11 backend only) the Xvfb display, e.g. ":123"
#   VK_ICD_FILENAMES       points at the lavapipe (software Vulkan) ICD json
#   LIBGL_ALWAYS_SOFTWARE  1, forces llvmpipe software GL/EGL
#   WLR_BACKENDS           headless   (sway/wlroots only)
#   WLR_LIBINPUT_NO_DEVICES 1         (sway/wlroots only; no real input devices)
#   WLR_RENDERER           pixman     (sway/wlroots only; avoids needing GBM/DRM)
#
# See ENVIRONMENT.md in this directory for the full rationale, what works,
# and known limitations (notably: ydotool/uinput does not work in this
# container; use wtype under Wayland or xdotool under X11 instead).
set -uo pipefail

BACKEND="${TESTENV_BACKEND:-sway}"
WIDTH="${TESTENV_WIDTH:-1280}"
HEIGHT="${TESTENV_HEIGHT:-720}"
KEEP_LOG="${TESTENV_KEEP_LOG:-0}"
ENV_FILE="${TESTENV_ENV_FILE:-}"

if [[ $# -eq 0 ]]; then
  echo "usage: $0 [--] <command> [args...]" >&2
  exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"

# --- pick a lavapipe ICD json (name differs across mesa versions) ----------
VK_ICD=""
for candidate in \
  /usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  /usr/share/vulkan/icd.d/lvp_icd.json
do
  if [[ -f "$candidate" ]]; then
    VK_ICD="$candidate"
    break
  fi
done
if [[ -z "$VK_ICD" ]]; then
  echo "WARNING: no lavapipe ICD json found under /usr/share/vulkan/icd.d/." >&2
  echo "         Did you run ./setup-testenv.sh? Vulkan will likely fail to init." >&2
fi

RUN_DIR="$(mktemp -d /tmp/rust-gpu-testenv.XXXXXX)"
chmod 700 "$RUN_DIR"
LOG_FILE="$RUN_DIR/compositor.log"

COMPOSITOR_PID=""
CHILD_STATUS=1

cleanup() {
  if [[ -n "$COMPOSITOR_PID" ]] && kill -0 "$COMPOSITOR_PID" 2>/dev/null; then
    kill "$COMPOSITOR_PID" 2>/dev/null
    for _ in $(seq 1 20); do
      kill -0 "$COMPOSITOR_PID" 2>/dev/null || break
      sleep 0.1
    done
    kill -9 "$COMPOSITOR_PID" 2>/dev/null || true
  fi
  if [[ -n "$ENV_FILE" ]]; then
    rm -f "$ENV_FILE"
  fi
  if [[ "$KEEP_LOG" != "1" ]]; then
    rm -rf "$RUN_DIR"
  else
    echo "Compositor log kept at: $LOG_FILE" >&2
  fi
}
trap cleanup EXIT INT TERM

write_env_file() {
  [[ -z "$ENV_FILE" ]] && return 0
  {
    echo "export XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"
    [[ -n "${WAYLAND_DISPLAY:-}" ]] && echo "export WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
    [[ -n "${DISPLAY:-}" ]] && echo "export DISPLAY=$DISPLAY"
    [[ -n "${VK_ICD_FILENAMES:-}" ]] && echo "export VK_ICD_FILENAMES=$VK_ICD_FILENAMES"
    echo "export LIBGL_ALWAYS_SOFTWARE=1"
    echo "export GALLIUM_DRIVER=llvmpipe"
  } > "$ENV_FILE"
}

export_common_gpu_env() {
  export LIBGL_ALWAYS_SOFTWARE=1
  export GALLIUM_DRIVER=llvmpipe
  if [[ -n "$VK_ICD" ]]; then
    export VK_ICD_FILENAMES="$VK_ICD"
  fi
}

run_command_with_status() {
  set +e
  "$@"
  CHILD_STATUS=$?
  set -e
}

case "$BACKEND" in
  sway)
    export XDG_RUNTIME_DIR="$RUN_DIR"
    export WLR_BACKENDS=headless
    export WLR_LIBINPUT_NO_DEVICES=1
    export WLR_RENDERER=pixman
    export_common_gpu_env

    # Minimal sway config: just set the headless output size. No bars, no
    # extra IPC noise.
    SWAY_CFG="$RUN_DIR/sway.conf"
    cat > "$SWAY_CFG" <<EOF
output HEADLESS-1 resolution ${WIDTH}x${HEIGHT}
EOF

    sway -c "$SWAY_CFG" >"$LOG_FILE" 2>&1 &
    COMPOSITOR_PID=$!

    # Wait for the wayland socket to appear.
    for _ in $(seq 1 50); do
      SOCK="$(find "$RUN_DIR" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' 2>/dev/null | head -n1)"
      [[ -n "$SOCK" ]] && break
      kill -0 "$COMPOSITOR_PID" 2>/dev/null || break
      sleep 0.1
    done
    if [[ -z "${SOCK:-}" ]]; then
      echo "ERROR: sway did not create a wayland socket. Log:" >&2
      cat "$LOG_FILE" >&2
      exit 1
    fi
    export WAYLAND_DISPLAY="$(basename "$SOCK")"
    echo "sway headless up: XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=$WAYLAND_DISPLAY" >&2
    write_env_file

    run_command_with_status "$@"
    ;;

  weston)
    export XDG_RUNTIME_DIR="$RUN_DIR"
    export_common_gpu_env

    weston --backend=headless-backend.so --width="$WIDTH" --height="$HEIGHT" \
      --socket=wayland-testenv >"$LOG_FILE" 2>&1 &
    COMPOSITOR_PID=$!

    for _ in $(seq 1 50); do
      [[ -S "$RUN_DIR/wayland-testenv" ]] && break
      kill -0 "$COMPOSITOR_PID" 2>/dev/null || break
      sleep 0.1
    done
    if [[ ! -S "$RUN_DIR/wayland-testenv" ]]; then
      echo "ERROR: weston did not create its wayland socket. Log:" >&2
      cat "$LOG_FILE" >&2
      exit 1
    fi
    export WAYLAND_DISPLAY=wayland-testenv
    echo "weston headless up: XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=$WAYLAND_DISPLAY" >&2
    write_env_file

    run_command_with_status "$@"
    ;;

  x11)
    export XDG_RUNTIME_DIR="$RUN_DIR"
    export_common_gpu_env

    # Find a free display number.
    DISPLAY_NUM=""
    for n in $(seq 50 999); do
      if [[ ! -e "/tmp/.X11-unix/X$n" ]]; then
        DISPLAY_NUM="$n"
        break
      fi
    done
    if [[ -z "$DISPLAY_NUM" ]]; then
      echo "ERROR: could not find a free X display number." >&2
      exit 1
    fi

    Xvfb ":$DISPLAY_NUM" -screen 0 "${WIDTH}x${HEIGHT}x24" >"$LOG_FILE" 2>&1 &
    COMPOSITOR_PID=$!

    for _ in $(seq 1 50); do
      [[ -e "/tmp/.X11-unix/X$DISPLAY_NUM" ]] && break
      kill -0 "$COMPOSITOR_PID" 2>/dev/null || break
      sleep 0.1
    done
    if [[ ! -e "/tmp/.X11-unix/X$DISPLAY_NUM" ]]; then
      echo "ERROR: Xvfb did not start. Log:" >&2
      cat "$LOG_FILE" >&2
      exit 1
    fi
    export DISPLAY=":$DISPLAY_NUM"
    echo "Xvfb up: DISPLAY=$DISPLAY" >&2
    write_env_file

    run_command_with_status "$@"
    ;;

  *)
    echo "ERROR: unknown TESTENV_BACKEND '$BACKEND' (want sway|weston|x11)" >&2
    exit 2
    ;;
esac

exit "$CHILD_STATUS"
