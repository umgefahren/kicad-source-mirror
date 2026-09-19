# The Rust schematic editor UI

This workspace holds the Rust half of eeschema's new user interface: the part
that owns the window, the pixels and the input, built on
[gpui-kit](https://crates.io/crates/gpui-kit) and rendered through wgpu.

It is **opt-in and off by default**. The existing wxWidgets schematic editor is
untouched and unaffected; with `-DKICAD_BUILD_RUST_SCH_UI=OFF` (the default)
cargo is never invoked and the host shared library is not built.

It opens real `.kicad_sch` files — `--schematic` below — holds the C++ session open,
re-records the frame from it whenever the view moves, and hands every pointer move,
click, drag, scroll and key press to KiCad's `TOOL_MANAGER` as a `TOOL_EVENT`. It is
still not an editor, and the reason is on the far side of that seam rather than on
this one: **no tool receives the events**, because every eeschema tool declines a
tool holder that is not a `wxFrame`. `docs/rust-migration/06-what-is-missing.md` is
the honest account of the distance from here to an editor.

## What is and is not here

Everything that makes a schematic a schematic stays in C++ — the document model,
the `.kicad_sch` reader and writer, the connectivity engine, ERC, netlist export
and the interactive tools. None of it is reimplemented here.

What crosses into Rust is the presentation layer. The mechanism is a **draw
stream**: `KIGFX::RECORDING_GAL` implements KiCad's abstract GAL interface by
recording the calls `SCH_PAINTER` makes, rather than rasterising them. The
result is a flat, device-independent command buffer that this workspace decodes
and draws. `SCH_PAINTER` and `KIGFX::VIEW` are not modified, so every rule about
how a schematic looks stays in one place, in C++, still covered by its own tests.

A stream reaches Rust two ways: from a file recorded earlier by
`kicad-sch-dump`, which is how the renderer is developed and tested with no C++
linked, and straight out of memory through the host's C ABI, which is how a real
`.kicad_sch` is opened.

The second is a live connection rather than a one-off. `kicad_sch_ui::document::LiveDocument`
is the seam: the canvas hands a document the camera it is about to paint with and
gets back the frame for it. The binary implements that over a `kicad_sch_sys::Session`;
`ReplayDocument` implements it over a recorded stream, so the whole re-render path
is testable with no host linked. The canvas asks only when the answer could have
changed — a pan, a zoom, a resize — because `KIGFX::VIEW` culls the frame body to
the camera but records retained geometry once, which is what keeps a live re-render
inside the frame budget.

See `docs/rust-migration/` for the full design:

| Document | Contents |
|---|---|
| `01-plan.md` | The architecture and where the cut goes |
| `00-architecture-survey.md` | Layer map, the GAL interface, wx coupling, fixtures |
| `02-gpui-kit-cookbook.md` | How to build against gpui-kit, verified against its sources |
| `03-build-notes.md` | Building the C++ tree |
| `04-host-seam.md` | The C++ session and the C ABI, including what the viewport scale is and is not |
| `06-what-is-missing.md` | Stage by stage: what is done, what is not, and what each costs |

## Crates

| Crate | Role |
|---|---|
| `kicad-gal` | The Rust half of the draw-stream ABI: decoder, validator, on-disk format, and a builder for constructing streams in tests |
| `kicad-sch-render` | Turns a draw stream into gpui primitives; camera, culling and the tessellation cache |
| `kicad-sch-ui` | The application shell: window, menu bar, toolbars, docks, status bar, command palette, input. Deliberately has **no** dependency on `kicad-sch-sys`: it produces `ShellEvent`s and posts them to an `InputSink`, and what is on the other end is the binary's business |
| `kicad-sch-sys` | The C++ host, linked: `bindgen` over `include/sch_host/sch_host_abi.h` and a safe wrapper around a schematic session |
| `kicad-eeschema-gpui` | The binary. The only crate that depends on both the shell and the host, which is why the two adapters live here: `SchematicSession` (shell asks for a frame → host records one) and `HostInputSink` (shell posts an event → host dispatches a `TOOL_EVENT`) |

`kicad-sch-sys` is the only crate here that talks to C++, and it is built so that
the others never have to care: with no host library found it compiles to an API
that reports itself unavailable, so `cargo test` in a checkout with no CMake build
behind it still passes. `kicad_sch_sys::is_available()` is how the binary knows
which it has.

The ABI itself is defined once, in C, at
`include/gal/recording/draw_stream_abi.h`, and both sides assert its layout at
compile time.

## Why there is no wgpu pipeline here

An early version of this plan had a bespoke wgpu renderer with SDF primitives.
It turned out to be both impossible and unnecessary.

Impossible, because gpui does not expose its `wgpu::Device` or queue, its
renderer has no custom-pass hook, and the only way to hand it an image is as CPU
bytes — around 25–33 MB of memcpy plus a multi-millisecond readback stall per
1080p frame, which does not fit in a 120 Hz budget.

Unnecessary, because gpui's own primitives are enough. `PathBuilder` is a full
lyon frontend — stroked polylines with caps, joins and miter limits, dash
arrays, elliptical arcs, béziers, filled polygons with fill rules — and it is
rasterised by gpui's wgpu renderer into a 4× MSAA intermediate. That is better
antialiasing than a naive custom pipeline would have given us, for none of the
work.

So the rendering is wgpu; we simply do not write the shaders.

## Toolchain

gpui 0.3.5 uses the still-unstable `std::hint::cold_path`, so **a nightly
compiler is required**. `rust-toolchain.toml` pins it and rustup will honour
that automatically for anything run inside this directory. This pin is a
dependency constraint, not a preference, and should be removed once gpui builds
on stable.

`devenv shell` (see `devenv.nix` at the repo root) supplies the same nightly
directly, without rustup in the picture, along with everything the C++ half of
the build needs.

## Building and testing

```sh
# On its own, against recorded streams. No C++ needed.
cd rust
cargo build
cargo test

# As part of the KiCad build, which also builds the C++ host library and tells
# cargo where it is.
cmake -S . -B build -DKICAD_BUILD_RUST_SCH_UI=ON
cmake --build build --target eeschema_gpui
```

With `KICAD_BUILD_QA_TESTS=ON`, `cargo test` for each crate is registered with
CTest and runs alongside the rest of the QA suite.

### Running it

```sh
# A real schematic, through the C++ host: eeschema's reader, eeschema's painter,
# no file in between, and re-recorded from the live session on every pan and zoom.
build/rust-target/release/eeschema-gpui --schematic demos/video/video.kicad_sch

# A stream recorded earlier by kicad-sch-dump. Works in a build with no host.
build/rust-target/release/eeschema-gpui --stream qa/data/draw_streams/ecc83_pp_v2.kgds
```

`--schematic` needs the host library. Outside the CMake build, point
`KICAD_SCH_HOST_DIR` at the directory holding `libkicad_sch_host` (or set
`KICAD_BUILD_DIR` to a build tree) before `cargo build`; setting
`KICAD_SCH_HOST_DIR=` empty forces the no-host build, which is how that path
stays tested on a machine that has one.

**Input reaches the tool framework and stops there.** With `--schematic` the binary
installs `HostInputSink` instead of `NullSink`, so a pointer move, a click, a drag,
a scroll, a key press, a tool button and a menu item all cross into C++: the first
five as `TOOL_EVENT`s through `HOST_TOOL_DISPATCHER`, the last two as
`TOOL_ACTION`s by name. Nothing answers, because `TOOL_MANAGER` has no tools — see
`docs/rust-migration/06-what-is-missing.md` Stage 4b. With `--stream` there is no
session to talk to and the sink is still the null one.

Two things this makes visible in the shell today: the cursor the *tools* would read
is the grid-snapped one the host reports, and the status bar's selection count comes
from the host rather than from the shell, so it is honest about being zero rather
than pretending the shell has a selection of its own.

Keys cross the boundary **by name** — `"escape"`, `"f11"`, `"w"` — and C++ maps them
onto KiCad's `WXK_*` codes in one function,
`HOST_TOOL_DISPATCHER::KeyCodeFromName`. That is on purpose: a `WXK_*` table
transcribed into Rust would be one wrong entry per silently broken shortcut, with
nothing in the build to notice. `grep -rn "WXK_" rust/crates/` should find nothing but
comments saying where the mapping lives.

`--frame-stats` puts the frame timing and the renderer's own per-frame numbers in
the status bar, which is where the live path is visible: the groups drawn and
culled change as you pan, and the paths do not get rebuilt.

### Tests that need a GPU

The renderer's GPU-backed tests look for a Vulkan adapter and **skip** with a
clear message when there is none, so `cargo test` stays green on a machine
without one. To run them anywhere, use Mesa's software Vulkan device:

```sh
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export LIBGL_ALWAYS_SOFTWARE=1
cargo test
```

`tools/rust-gpu-testenv/` sets that up, along with a headless Wayland
compositor for running and screenshotting the real application:

```sh
./tools/rust-gpu-testenv/setup-testenv.sh          # once
./tools/rust-gpu-testenv/run-headless.sh <binary>  # run under headless sway
./tools/rust-gpu-testenv/screenshot.sh out.png     # capture it
```

This is how the UI is checked visually — gpui's own screenshot support is
macOS-only.
