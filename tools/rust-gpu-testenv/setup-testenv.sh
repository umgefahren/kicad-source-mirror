#!/usr/bin/env bash
# setup-testenv.sh — Install everything needed to run and screenshot-test a
# Rust GUI app that uses wgpu (Vulkan) and/or gpui headlessly on Linux.
#
# Idempotent: safe to re-run. Installs:
#   - Mesa lavapipe (software Vulkan) + vulkan-tools + validation layers
#   - Mesa llvmpipe software GL/EGL + libosmesa (software GL fallback)
#   - Headless Wayland compositors: sway (wlroots, recommended) and weston,
#     plus cage as a kiosk-style alternative
#   - grim (Wayland screenshots), wlr-randr, wtype (Wayland virtual-keyboard
#     text/key injection)
#   - Xvfb + X11 tooling (xdotool, x11-utils, imagemagick's `import`) as the
#     X11 fallback path
#   - ydotool (uinput-based input injection) -- installed for completeness,
#     but see ENVIRONMENT.md: it does NOT work in this container because
#     /dev/uinput has no working kernel backing (ENODEV). wtype (Wayland) or
#     xdotool (X11, uses XTEST, no uinput needed) are the working input paths.
#   - Rust/gpui/wgpu native build dependencies
#
# This script does NOT touch anything outside of apt package state, so it is
# safe to run from inside a source repository.
#
# Usage: sudo ./setup-testenv.sh
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "This script installs system packages and must be run as root." >&2
  exit 1
fi

export DEBIAN_FRONTEND=noninteractive

echo "==> apt-get update"
apt-get update -y

PACKAGES=(
  # --- Goal 1: software Vulkan (lavapipe) + software GL (llvmpipe) ---
  mesa-vulkan-drivers
  vulkan-tools
  libvulkan1
  libvulkan-dev
  vulkan-validationlayers
  mesa-utils
  libgl1-mesa-dri
  libosmesa6
  libosmesa6-dev
  libegl1-mesa-dev
  libgles2-mesa-dev

  # --- Goal 2: headless Wayland compositors + screenshot/input tools ---
  weston
  sway
  cage
  grim
  wlr-randr
  wtype
  ydotool

  # --- Goal 2 fallback: Xvfb + X11 tooling ---
  xvfb
  x11-utils
  x11-apps
  xdotool
  imagemagick

  # --- Goal 3: build deps for gpui / wgpu on Linux ---
  build-essential
  cmake
  pkg-config
  libssl-dev
  libasound2-dev
  libfontconfig1-dev
  libfreetype6-dev
  libwayland-dev
  libxkbcommon-dev
  libxkbcommon-x11-dev
  libx11-dev
  libxcb1-dev
  libxcb-dri3-dev
  libxcb-present-dev
  libxcb-randr0-dev
  libxcb-glx0-dev
  libxcb-shape0-dev
  libxcb-xfixes0-dev
  libxcb-render-util0-dev
  libxcb-icccm4-dev
  libxcb-keysyms1-dev
  libxcb-sync-dev
  libxrandr-dev
  libxi-dev
  libgbm-dev
  libdrm-dev
  libudev-dev
  wayland-protocols
  protobuf-compiler
  clang
  libclang-dev
  mold
)

echo "==> apt-get install (${#PACKAGES[@]} packages)"
apt-get install -y --no-install-recommends "${PACKAGES[@]}"

echo "==> Verifying lavapipe Vulkan ICD is present"
ICD_JSON=""
for candidate in \
  /usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  /usr/share/vulkan/icd.d/lvp_icd.json
do
  if [[ -f "$candidate" ]]; then
    ICD_JSON="$candidate"
    break
  fi
done

if [[ -z "$ICD_JSON" ]]; then
  echo "WARNING: could not find a lavapipe ICD json under /usr/share/vulkan/icd.d/." >&2
  echo "         'mesa-vulkan-drivers' may have changed its layout; inspect that dir." >&2
else
  echo "    Found lavapipe ICD: $ICD_JSON"
  if VK_ICD_FILENAMES="$ICD_JSON" vulkaninfo --summary 2>&1 | grep -qi "llvmpipe"; then
    echo "    OK: vulkaninfo reports llvmpipe/lavapipe device."
  else
    echo "WARNING: vulkaninfo did not report an llvmpipe device. See ENVIRONMENT.md." >&2
  fi
fi

echo "==> Checking for a Rust toolchain"
if ! command -v cargo >/dev/null 2>&1; then
  echo "cargo not found. Install Rust yourself, e.g.:" >&2
  echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y" >&2
else
  echo "    Found: $(cargo --version), $(rustc --version)"
fi

echo "==> Checking /dev/uinput (needed by ydotool; NOT expected to work in most containers)"
if [[ -e /dev/uinput ]]; then
  if python3 - <<'PY' 2>/dev/null
import os, sys
try:
    fd = os.open('/dev/uinput', os.O_WRONLY | os.O_NONBLOCK)
    os.close(fd)
except OSError as e:
    sys.exit(1)
PY
  then
    echo "    /dev/uinput exists and opens successfully -- ydotool should work."
  else
    echo "    /dev/uinput exists but cannot be opened (no kernel backing in this"
    echo "    container / namespace). ydotool will NOT work. Use wtype (Wayland)"
    echo "    or xdotool (X11) for input injection instead. See ENVIRONMENT.md."
  fi
else
  echo "    /dev/uinput does not exist. ydotool will NOT work in this container."
  echo "    Use wtype (Wayland) or xdotool (X11) for input injection instead."
fi

echo ""
echo "==> setup-testenv.sh finished successfully."
echo "    Next: run ./run-headless.sh <your-binary> [args...] to run a GUI app headlessly,"
echo "    and ./screenshot.sh <output.png> to capture the running compositor."
