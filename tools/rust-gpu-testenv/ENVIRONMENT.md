# Rust GPU test environment (headless wgpu / gpui on Linux)

This documents a working setup, verified end-to-end in this container, for
running and screenshot-testing a Rust GUI app that uses **wgpu** (Vulkan) and
**gpui**-style windowing (Wayland or X11) with no real GPU or display.

Container: Ubuntu 24.04.4 LTS, `Linux 6.18.44-fc-v33`, x86_64, root, 4 cores,
15 GB RAM, ~29 GB free disk at the start of setup. apt was allowed and worked
normally through the pre-configured egress proxy.

Scripts in this directory:
- `setup-testenv.sh` — installs everything below. Idempotent.
- `run-headless.sh` — starts a compositor (or Xvfb), exports the right env
  vars, runs your command, tears down on exit.
- `screenshot.sh` — screenshots the currently running compositor/X server.

## TL;DR: the exact incantation

```bash
# Software Vulkan (lavapipe) — this is the ICD filename on this Mesa version:
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export LIBGL_ALWAYS_SOFTWARE=1     # software GL/EGL (llvmpipe) fallback
export GALLIUM_DRIVER=llvmpipe

# Headless Wayland (recommended path):
export XDG_RUNTIME_DIR=/tmp/some-private-0700-dir
export WLR_BACKENDS=headless
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=pixman
sway -c /dev/null &                 # creates $XDG_RUNTIME_DIR/wayland-1 (or -0)
export WAYLAND_DISPLAY=wayland-1    # whatever socket sway actually created

# Now run your wgpu/gpui/winit app — it will pick up Wayland automatically
# and use lavapipe for rendering.
./your-app
```

Or, simpler, just use the wrapper: `./run-headless.sh ./your-app --flags`.

X11 fallback (also fully verified):
```bash
export DISPLAY=:50
Xvfb :50 -screen 0 1280x720x24 &
# same VK_ICD_FILENAMES / LIBGL_ALWAYS_SOFTWARE as above
./your-app
```

## What is installed (via `setup-testenv.sh`)

**Software Vulkan (goal 1) — WORKS**
- `mesa-vulkan-drivers` (25.2.8-0ubuntu0.24.04.2) — provides the lavapipe
  (`llvmpipe`) software Vulkan ICD.
  - **Important gotcha**: on this Mesa/Ubuntu version the ICD json is named
    `/usr/share/vulkan/icd.d/lvp_icd.json`, **not**
    `lvp_icd.x86_64.json` as older docs/instructions sometimes say. Both
    filenames are checked by `run-headless.sh`/`setup-testenv.sh`.
- `vulkan-tools` (`vulkaninfo`), `libvulkan1`, `libvulkan-dev`,
  `vulkan-validationlayers` — all installed.
- `mesa-utils`, `libgl1-mesa-dri`, `libosmesa6`/`libosmesa6-dev`,
  `libegl1-mesa-dev`, `libgles2-mesa-dev` — software GL / OSMesa fallback.

Verified with:
```
$ VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json vulkaninfo --summary
```
(run without a display or XDG_RUNTIME_DIR; those two warnings are harmless
and only affect surface-capability queries, not device creation)
```
Vulkan Instance Version: 1.3.275
...
Devices:
========
GPU0:
	apiVersion         = 1.4.318
	driverVersion      = 25.2.8
	vendorID           = 0x10005
	deviceID           = 0x0000
	deviceType         = PHYSICAL_DEVICE_TYPE_CPU
	deviceName         = llvmpipe (LLVM 20.1.2, 256 bits)
	driverID           = DRIVER_ID_MESA_LLVMPIPE
	driverName         = llvmpipe
	driverInfo         = Mesa 25.2.8-0ubuntu0.24.04.2 (LLVM 20.1.2)
	conformanceVersion = 1.3.1.1
```

**Headless compositors (goal 2) — WORKS (Wayland via sway, and X11 via Xvfb)**
- `sway` (1.9, wlroots 0.17) — **primary/recommended**. Starts fully headless
  with:
  ```
  XDG_RUNTIME_DIR=<private dir>
  WLR_BACKENDS=headless
  WLR_LIBINPUT_NO_DEVICES=1
  WLR_RENDERER=pixman        # avoids needing GBM/DRM/a real render node
  sway -c /dev/null           # or a minimal config, see run-headless.sh
  ```
  This creates a `wayland-N` socket under `$XDG_RUNTIME_DIR` (usually
  `wayland-1` when nothing else is using that runtime dir) and a headless
  output named `HEADLESS-1`, default resolution 1280x720. Resolution is
  controllable via `output HEADLESS-1 resolution WxH` in the sway config.
- `weston` (13.0.0) — also starts headless
  (`weston --backend=headless-backend.so --width=W --height=H
  --socket=NAME`) and is supported as an alternate backend
  (`TESTENV_BACKEND=weston`), but **grim was only verified against sway** in
  this session; weston's own `weston-screenshooter` is wired up in
  `screenshot.sh` as weston's fallback path but was not exercised end-to-end
  here (time-boxed — sway fully satisfied the requirement).
- `cage` — installed but not exercised; it's a kiosk-style single-app
  wlroots compositor and should also work for a single fullscreen window,
  documented here only as "installed, not verified."
- `grim` — **verified working** against sway headless: takes a full-output
  screenshot to PNG. This is the screenshot tool `run-headless.sh` /
  `screenshot.sh` use for the Wayland path.
- `wlr-randr` — installed for output introspection under wlroots
  compositors.
- **X11 fallback** — `Xvfb` (already present in the base image),
  `x11-utils`, `x11-apps`, `xdotool`, `imagemagick` (`import`) — all
  **verified working**: Xvfb starts on an arbitrary free `:N` display,
  `import -window root` produces a correct screenshot PNG.

**Input injection — PARTIAL: Wayland virtual-keyboard works, uinput does NOT**
- `wtype` — **verified working** under sway headless (runs, exit code 0,
  uses the Wayland `virtual-keyboard` / `virtual-pointer` protocols that
  wlroots compositors implement natively; does **not** need `/dev/uinput`).
  This is the recommended way to inject keyboard input under the Wayland
  path.
- `xdotool` — installed for the X11 path. `xdotool key`/`type`/`mousemove`
  use the X `XTEST` extension, which Xvfb supports and which also does
  **not** need `/dev/uinput`. Not exhaustively exercised against a live app
  in this session, but `xdotool getdisplaygeometry` against the running
  Xvfb succeeded, confirming XTEST/X11 connectivity.
- `ydotool` / `ydotoold` — **installed but DOES NOT WORK in this
  container.** `/dev/uinput` does not exist by default, and even after
  `mknod /dev/uinput c 10 223` (which succeeds, i.e. the process has
  `CAP_MKNOD`), opening it fails with `ENODEV` ("No such device"): the
  kernel this container's namespace sees has no real uinput driver backing
  that node. `ydotool` aborts with `failed to open uinput device`. This is
  a fundamental container/kernel-namespace limitation, not a missing
  package — **do not spend time trying to fix this**; use `wtype`
  (Wayland) or `xdotool` (X11) instead, or drive the app via its own
  in-process test harness/automation hooks if it needs synthetic input
  events at a layer above OS-level input injection.

**Build deps for gpui/wgpu on Linux (goal 3) — all installed**
`build-essential cmake pkg-config libssl-dev libasound2-dev
libfontconfig1-dev libfreetype6-dev libwayland-dev libxkbcommon-dev
libxkbcommon-x11-dev libx11-dev libxcb1-dev libxcb-dri3-dev
libxcb-present-dev libxcb-randr0-dev libxcb-glx0-dev libxcb-shape0-dev
libxcb-xfixes0-dev libxcb-render-util0-dev libxcb-icccm4-dev
libxcb-keysyms1-dev libxcb-sync-dev libxrandr-dev libxi-dev libgbm-dev
libdrm-dev libudev-dev libvulkan-dev libegl1-mesa-dev libgles2-mesa-dev
wayland-protocols protobuf-compiler clang libclang-dev mold`.

Rust toolchain was already present: `rustc 1.94.1`, `cargo 1.94.1`
(`/root/.cargo/bin`). `setup-testenv.sh` checks for it and prints an
install hint if missing, but does not install Rust itself.

## Verification: wgpu offscreen render (goal 4) — PASSED

Program: `/tmp/claude-0/gpuprobe` (Cargo project, not part of this repo —
copy its `src/main.rs`/`Cargo.toml` if you want to keep it; contents are
reproduced in git history of this deliverable's authoring session if
needed, or trivially rewritten: it's ~230 lines using `wgpu` + `pollster`
+ `png`).

- **wgpu version used: 30.0.1** (latest on crates.io at the time of setup;
  `winit = "0.30.13"`, `pollster = "1.0.1"`, `png = "0.18.1"`).
- Adapter enumeration (`instance.enumerate_adapters(Backends::all())`)
  found:
  - `name="llvmpipe (LLVM 20.1.2, 256 bits)" backend=Vulkan device_type=Cpu driver="llvmpipe" driver_info="Mesa 25.2.8-0ubuntu0.24.04.2 (LLVM 20.1.2)"`
  - `name="llvmpipe (LLVM 20.1.2, 256 bits)" backend=Gl device_type=Cpu driver="" driver_info="4.5 (Core Profile) Mesa 25.2.8-0ubuntu0.24.04.2"`
- `request_adapter()` (no `compatible_surface`, `PowerPreference::None`)
  selected the Vulkan/llvmpipe adapter above.
- Rendered a solid-red triangle over a solid-blue background to a
  256x256 `Rgba8UnormSrgb` offscreen render target, copied it to a
  buffer, read it back, and wrote a PNG.
- **Pixel verification**: corner pixel (background) = `[0, 0, 255, 255]`
  (blue), a pixel inside the triangle = `[255, 0, 0, 255]` (red). Exact
  match to what the shader specifies — **no gamma-correction surprises**
  at these saturated values. `RESULT: PASS` printed by the probe.

wgpu **30.0.1's API differs noticeably** from older tutorials/docs (this
is a fast-moving crate). Notable breaking changes hit while writing the
probe, in case they help downstream agents:
- `Instance::enumerate_adapters()` is now `async` and returns
  `Vec<Adapter>` directly (not `impl Iterator`).
- `RequestAdapterOptions` gained an `apply_limit_buckets: bool` field.
- `DeviceDescriptor` gained an `experimental_features` field.
- `InstanceDescriptor` gained `memory_budget_thresholds` and `display`
  fields (`display: None` is fine for offscreen/most windowed use).
- `PipelineLayoutDescriptor.push_constant_ranges` was replaced by
  `immediate_size: u32`.
- `RenderPipelineDescriptor.multiview` was renamed `multiview_mask`, and
  `RenderPassDescriptor` gained a `multiview_mask` field too.
- `Device::poll()` takes a `PollType`; use
  `wgpu::PollType::wait_indefinitely()` (there is no `PollType::Wait`
  unit-like variant/fn any more, it's a struct variant with named fields
  — the convenience constructor is what you want).
- `Buffer::get_mapped_range()` (on `BufferSlice`) now returns
  `Result<BufferView, MapRangeError>`, not `BufferView` directly.
- `SurfaceConfiguration` gained a `color_space: SurfaceColorSpace` field
  (`SurfaceColorSpace::Auto` is the sane default).
- `Surface::get_current_texture()` now returns an explicit
  `CurrentSurfaceTexture` enum (`Success`/`Suboptimal`/`Timeout`/
  `Occluded`/`Outdated`/`Lost`/`Validation`) instead of
  `Result<SurfaceTexture, SurfaceError>`.
- Presenting a frame is now `queue.present(surface_texture)`, not
  `surface_texture.present()`.

## Verification: windowed app under a compositor (goal 5) — PASSED on BOTH Wayland and X11

Program: `/tmp/claude-0/winitprobe` — `winit` 0.30.13 + `wgpu` 30.0.1,
opens a real window, clears every frame to solid green
(`rgb(0, 255, 0)`), runs for N seconds via `ApplicationHandler`.

- **Wayland (sway headless)**: window opened, adapter selected was again
  `llvmpipe ... backend=Vulkan`. `grim` screenshot of the sway output
  (1280x720, sway is a tiling WM so the single window filled the whole
  output) showed `srgb(0,255,0)` everywhere except the window title bar
  sway draws. 352-470 frames presented over a ~6-8s run (i.e. real
  presentation/vsync-paced rendering is happening, not a stall).
- **X11 (Xvfb, no window manager)**: window opened at its natural
  320x240 logical size in the top-left of the root window (no WM to
  place/decorate it). `import -window root` screenshot showed
  `srgb(0,255,0)` for the whole `0,0`-`319,0`(and beyond) window region
  and `srgb(0,0,0)` (Xvfb's black default background) elsewhere —
  exactly as expected. ~11900-18600 frames presented over 6-8s (X11 has
  no compositor-imposed frame pacing here, so it free-runs much faster
  than Wayland's presentation loop).

Both paths are wired into `run-headless.sh` (`TESTENV_BACKEND=sway`
default, or `TESTENV_BACKEND=x11`) and confirmed working through the
actual deliverable scripts (not just ad hoc shell), including the
concurrent-screenshot pattern (`TESTENV_ENV_FILE`, see script header
comments and `screenshot.sh`).

## What works

- Software Vulkan via lavapipe (`llvmpipe`) — wgpu creates a real device,
  renders, reads back correctly. **This is the load-bearing piece and it
  works.**
- Software GL via llvmpipe (`LIBGL_ALWAYS_SOFTWARE=1`) — wgpu also
  enumerates a `Gl`-backend llvmpipe adapter; not used for the main
  probes (Vulkan was selected) but available as a fallback if a
  particular wgpu/gpui build prefers GL.
- Headless Wayland via **sway** — window creation, per-frame rendering,
  presentation, and `grim` screenshots all confirmed.
- Headless Wayland via **weston** — starts cleanly; screenshot path
  (`weston-screenshooter`) wired into `screenshot.sh` but not run
  end-to-end (not needed once sway worked).
- Headless X11 via **Xvfb** — window creation, rendering, presentation,
  and `import` screenshots all confirmed. Full plan-B path as requested.
- Keyboard/pointer synthetic input via **wtype** (Wayland,
  virtual-keyboard protocol) — runs successfully against sway.
- `xdotool` is available for the X11 path (XTEST-based, no uinput
  needed) but was only smoke-tested (`getdisplaygeometry`), not used to
  drive a live app's keyboard/mouse in this session.

## What does NOT work / known limitations

- **`ydotool`/`ydotoold` (uinput-based input injection) does not work in
  this container.** `/dev/uinput` is absent by default; even after
  manually creating the device node (`mknod`, which succeeds), opening it
  fails with `ENODEV`. The kernel/namespace this container runs under has
  no real uinput driver exposed. This is environmental, not a packaging
  problem — do not re-attempt without also getting `/dev/uinput` properly
  passed through with a working driver (e.g. `--device=/dev/uinput` on a
  host that actually has the `uinput` kernel module loaded and grants the
  container access to it). **Use `wtype` under Wayland, or `xdotool`
  under X11, instead.**
- **`weston`'s screenshot path was not verified end-to-end** (only sway +
  grim was). If a downstream agent specifically needs weston, budget time
  to verify `weston-screenshooter` output format/location, or add
  `grim` support if weston's `wlr-screencopy` protocol happens to be
  available in this weston build (not checked).
- **`cage` was installed but never actually launched/tested.**
- **This is a shared, concurrently-used container.** While doing this
  work, another agent's `apt-get install` and cargo builds were observed
  running at the same time (visible in `ps aux`), competing for the 4
  CPU cores and eating into the ~29 GB free disk. All package installs
  above succeeded despite this, but be aware that build times and disk
  headroom are not exclusively yours; check `df -h /` before large
  builds.
- **wgpu's API moves fast.** The exact field names/enum shapes documented
  above are for **wgpu 30.0.1** specifically (crates.io, Sept 2026). Pin
  to that version (or re-verify) rather than assuming older tutorials'
  code compiles as-is — see the breaking-changes list above.
- No real GPU is present or needed; everything here is CPU-rendered via
  llvmpipe. This is fine for correctness/screenshot testing but is not
  representative of real-GPU performance or timing-sensitive behavior.
- `vulkaninfo`/apps that query surface capabilities print harmless
  warnings when `DISPLAY`/`XDG_RUNTIME_DIR` aren't set (device creation
  itself is unaffected) — don't mistake those for real failures.

## Script reference

- `setup-testenv.sh` — installs all of the above via apt. Idempotent
  (`apt-get install` no-ops on already-installed packages). Run as root.
- `run-headless.sh <cmd> [args...]` — starts the compositor/Xvfb, exports
  `XDG_RUNTIME_DIR`, `WAYLAND_DISPLAY` or `DISPLAY`, `VK_ICD_FILENAMES`,
  `LIBGL_ALWAYS_SOFTWARE=1`, `GALLIUM_DRIVER=llvmpipe`, plus (for sway)
  `WLR_BACKENDS=headless`, `WLR_LIBINPUT_NO_DEVICES=1`,
  `WLR_RENDERER=pixman`; runs `cmd`; tears the compositor down and
  removes its private runtime dir on exit (including on Ctrl-C).
  Controlled via `TESTENV_BACKEND` (`sway`|`weston`|`x11`, default
  `sway`), `TESTENV_WIDTH`/`TESTENV_HEIGHT` (default 1280x720),
  `TESTENV_KEEP_LOG=1` to keep the compositor's stdout/stderr log instead
  of deleting it, and `TESTENV_ENV_FILE=/path` to have it write the live
  session's env vars to a file (for a concurrent `screenshot.sh` call —
  see the script's header comment for the exact pattern).
- `screenshot.sh <output.png>` — screenshots the compositor/X server
  identified by the current `WAYLAND_DISPLAY`/`DISPLAY` env vars (`grim`
  for Wayland with a `weston-screenshooter` fallback, `import -window
  root` for X11).

All three scripts are self-contained shell scripts with no dependency on
anything else in this repository; they can be copied elsewhere if needed.
