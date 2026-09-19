# 03 — Building the KiCad C++ tree (container reproduction notes)

Target audience: an engineer who has a fresh container and needs `common` + `eeschema`
to compile so they can link Rust against the C++ document model.

Reference environment (what these notes were verified on):

| | |
|---|---|
| OS | Ubuntu 24.04 (noble), root |
| CPU / RAM | 4 cores / 15 GB |
| Compiler | gcc / g++ 13.3.0 |
| CMake | 3.28.3 |
| Ninja | 1.11.1 |
| wxWidgets | 3.2.4 (GTK3), from `libwxgtk3.2-dev` |
| Source | `/home/user/kicad-source-mirror`, branch `claude/bold-lamport-ireol3` |
| Build dir | `/tmp/claude-0/kicad-build/build` |

> **Tree version matters.** This is a KiCad 10.99 (post-9.x master) tree. Several
> build options that older guides and the KiCad dev docs still mention **do not
> exist here** — see [Options that do not exist](#options-that-do-not-exist).

> **On a workstation rather than this container**, `devenv.nix` at the repo root
> provides the same dependency set plus the nightly Rust toolchain, on Linux and
> macOS, without touching the host — see
> [§6 The same build from nix](#6-the-same-build-from-nix-devenvnix).

---

## 1. Dependencies

Everything needed is in the Ubuntu 24.04 archive. Nothing had to be built from
source, and no package was blocked by the egress proxy.

### Core set (required)

```bash
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build pkg-config ccache git \
  libcairo2-dev libpng-dev libglu1-mesa-dev libgl1-mesa-dev libglew-dev libegl1-mesa-dev \
  libx11-dev mesa-common-dev libgtk-3-dev libglm-dev \
  libwxgtk3.2-dev libwxgtk-webview3.2-dev \
  libbz2-dev libssl-dev libzstd-dev zlib1g-dev \
  python3-dev python3-pytest \
  libgit2-dev libsecret-1-dev libboost-all-dev libcurl4-openssl-dev \
  unixodbc-dev libspnav-dev \
  nlohmann-json3-dev libfreetype6-dev libharfbuzz-dev libfontconfig1-dev libpixman-1-dev gettext \
  shared-mime-info
```

### Optional set (installed here; see below for what is actually optional)

```bash
apt-get install -y --no-install-recommends \
  libocct-modeling-algorithms-dev libocct-modeling-data-dev libocct-data-exchange-dev \
  libocct-visualization-dev libocct-foundation-dev libocct-ocaf-dev \
  libngspice0-dev libprotobuf-dev protobuf-compiler libnng-dev libzint-dev \
  libpoppler-dev libpoppler-glib-dev swig python3-wxgtk4.0
```

Notes on the optional set:

* **OpenCascade (`libocct-*`)** — only consumed by `3d-viewer` and the STEP/IGES
  importers. There is no `KICAD_USE_OCC` switch in this tree; CMake probes for it and
  adapts. Installing it is cheaper than fighting it.
* **`libngspice0-dev`** — the simulator. Discovered by `cmake/FindNgspice.cmake`;
  `${NGSPICE_LIBRARY}` is linked into `eeschema_kiface`. If absent, CMake still
  configures and eeschema still builds, minus the simulator.
* **protobuf / `libnng-dev`** — these are **not** optional. `kiapi` and `kinng` are
  unconditional dependencies of `kicommon`. Omit them and configure fails.
* **`swig` / `python3-wxgtk4.0`** — not used by this tree at all (SWIG scripting was
  removed upstream). Installed out of caution; safe to skip.
* **`libspnav-dev`** — 3Dconnexion SpaceMouse. Pulls in the `eeschema_navlib` target,
  which `eeschema_kiface_objects` links unconditionally. Keep it installed; it is tiny.

`ccache` is worth configuring before the first build:

```bash
ccache -M 8G
```

---

## 2. Configure

Use the committed helper:

```bash
tools/build/configure-dev.sh /tmp/claude-0/kicad-build/build
```

which is equivalent to:

```bash
cmake -S /home/user/kicad-source-mirror -B /tmp/claude-0/kicad-build/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DKICAD_BUILD_QA_TESTS=ON \
  -DKICAD_BUILD_I18N=OFF \
  -DKICAD_UPDATE_CHECK=OFF \
  -DKICAD_INSTALL_DEMOS=OFF
```

**This configures on the first attempt, in roughly 11–12 seconds.** No option had to
be changed to make configure succeed. `KICAD_BUILD_I18N`, `KICAD_UPDATE_CHECK` and
`KICAD_INSTALL_DEMOS` are convenience only — they trim work, they are not fixes.

Two harmless warnings appear and can be ignored:

```
git describe returned error 128: fatal: No names found, cannot describe anything.
```
The tree has no tags, so the version string falls back to a default.

```
kicad-cli not found — Cannot create newstroke_upgrade_syms target
```
Bootstrap-only target for regenerating the stroke font; irrelevant to us.

### Options that do not exist

These were expected from the task brief / older docs but are **absent** from this
tree. Passing them is not an error, but CMake will warn
`Manually-specified variables were not used by the project`:

| Option | Status in this tree |
|---|---|
| `KICAD_SCRIPTING_WXPYTHON` | **Gone.** SWIG/wxPython scripting was removed upstream; replaced by the IPC API. Nothing to disable. |
| `KICAD_USE_EGL` | **Gone.** EGL vs GLX is decided at runtime / by the wx build. |
| `KICAD_USE_OCC` | **Gone.** OCC is auto-detected; no toggle. |
| `KICAD_SPICE` | **Gone.** Only `KICAD_SPICE_QA` (default `ON`) remains, and it gates spice *tests*, not the simulator. |
| `KICAD_IPC_API` | **Gone.** The IPC API (`kiapi`, protobuf, nng) is unconditional. |

### Options that do exist and matter

From the top-level `CMakeLists.txt`:

| Option | Default | Effect |
|---|---|---|
| `KICAD_BUILD_QA_TESTS` | `ON` | Builds `qa_eeschema`, `qa_common`, … Needed for section 5. |
| `KICAD_SPICE_QA` | `ON` | Spice unit tests only. |
| `KICAD_SIGNAL_INTEGRITY` | `ON` | Signal-integrity tooling. |
| `KICAD_BUILD_I18N` | `OFF` | Translation catalogs. |
| `KICAD_UPDATE_CHECK` | `ON` | Phone-home update check; turn `OFF` for dev. |
| `KICAD_USE_PCH` | `ON` | Precompiled headers. **Leave ON** — a large win on `eeschema_kiface_objects`. |
| `KICAD_USE_SENTRY` | `OFF` | Crash telemetry. |
| `KICAD_INSTALL_DEMOS` | `ON` | Demo projects; `OFF` saves install time. |
| `KICAD_SANITIZE_ADDRESS` / `_THREADS` | `OFF` | ASan/TSan. |
| `KICAD_STDLIB_DEBUG` | `OFF` | **Do not combine with QA tests** — `_GLIBCXX_DEBUG` changes libstdc++ container ABI and crashes against system Boost. CMake warns about this explicitly. |

### Can eeschema be built without pcbnew?

**Yes, mostly.** `eeschema_kiface` does not link `pcbnew` or `pcbcommon`. Building
`ninja eeschema_kiface` does not build the pcbnew kiface.

Two caveats:

1. `eeschema_kiface_objects` links `pads_common`, `libwmf_orcad`, `emf2svg` and `pcm`
   (the Plugin & Content Manager) — these are shared importer/infra libraries that
   live outside `eeschema/`, not pcbnew itself.
2. The CMake target literally named `connectivity` is **pcbnew's** connectivity
   (`pcbnew/connectivity/CMakeLists.txt`). Eeschema's connectivity is
   `CONNECTION_GRAPH` / `SCH_CONNECTION`, compiled **inside**
   `eeschema_kiface_objects`. Do not build the `connectivity` target expecting
   schematic connectivity.

### Disk

`Release` was chosen deliberately. Watch with `df -h /`. The full tree
(build dir + ccache) stays comfortably inside the container's free space, but
`RelWithDebInfo` roughly triples object-file size — do not switch casually.

---

## 3. Build

Real target names (discover them yourself with `ninja -t targets | grep -v /`):

| Target | Artifact | Kind |
|---|---|---|
| `kicommon` | `libkicommon.so.10.99.0` | shared |
| `gal` | `libkigal.so.10.99.0` | shared |
| `common` | `libcommon.a` | static |
| `pcbcommon` | `libpcbcommon.a` | static |
| `eeschema_kiface_objects` | CMake OBJECT library | objects |
| `eeschema_kiface` | `_eeschema.kiface` | MODULE (dlopen'd) |
| `eeschema` | `eeschema` | executable (thin launcher) |
| `qa_eeschema` | `qa/tests/eeschema/qa_eeschema` | test binary |

Recommended order, 4 jobs:

```bash
cd /tmp/claude-0/kicad-build/build
ninja -j4 kicommon
ninja -j4 gal
ninja -j4 common
ninja -j4 eeschema_kiface
ninja -j4 eeschema
ninja -j4 qa_eeschema
```

`common` transitively depends on `gal` and `kicommon`, and `eeschema_kiface` pulls in
everything, so `ninja -j4 eeschema` alone is sufficient — the staged form just gives
you intermediate checkpoints and better failure localisation.

### Timings

Measured on the reference box (4 cores, `-j4`, **cold ccache**, `Release`):

| Target | Wall time | Notes |
|---|---:|---|
| `kicommon` | 15m 06s | 396 edges |
| `common` | 13m 06s | 635 edges cumulative; pulls in `gal` |
| `gal` | 0m 09s | already built as a dependency of `common` |
| `connectivity` | 0m 45s | pcbnew's, not eeschema's — see caveat above |
| `eeschema_kiface` | 62m 41s | 563 edges; the dominant cost |
| `eeschema` | 0m 39s | thin launcher, links against the kiface |
| **total to `eeschema`** | **~1h 52m** | |

`eeschema_kiface_objects` is ~470 translation units and dominates everything else;
budget roughly an hour for it on any 4-core machine.

Because `gal` and `connectivity` are already pulled in as dependencies, the staged
command list in the previous section costs nothing extra over a single
`ninja -j4 eeschema` — it just gives you checkpoints.

With a warm ccache a full rebuild drops to minutes. Do not delete
`~/.cache/ccache` between experiments.

### Artifacts produced

| Path (relative to build dir) | Size |
|---|---:|
| `common/libkicommon.so.10.99.0` | 18.8 MB |
| `common/libcommon.a` | 58.6 MB |
| `common/gal/libkigal.so.10.99.0` | 7.2 MB |
| `eeschema/_eeschema.kiface` | 63.0 MB |
| `eeschema/eeschema` | 170 KB (launcher only) |

The size split between `eeschema` and `_eeschema.kiface` is the clearest evidence of
the kiface architecture: essentially all of eeschema lives in the loadable module.

### Gotchas

* **Do not use `-j` above 4 on this box.** `eeschema_kiface_objects` translation units
  with PCH are memory-hungry; 15 GB / 4 jobs is about the right ratio. Higher
  parallelism risks the OOM killer taking out a compiler mid-build, which surfaces as
  a confusing `ninja: build stopped: subcommand failed`.
* **Generated headers.** `version_header`, `generate_headers` and
  `api_schema_build_copy` are dependencies of `kicommon`. If you ever invoke a
  compiler by hand, generate these first (`ninja version_header generate_headers`)
  or you will get missing-header errors that look like missing system packages.
* **PCH + ccache.** They cooperate here, but if you see bizarre "file changed"
  ccache misses, `ccache -C` and rebuild rather than debugging it.
* The `rust/` directory at the repo root is untracked scaffolding from the migration
  work and is not part of the CMake build.

---

## 4. Running the QA tests

The eeschema unit tests are a single Boost.Test binary.

```bash
cd /tmp/claude-0/kicad-build/build
ninja -j4 qa_eeschema

# run everything
./qa/tests/eeschema/qa_eeschema

# list the test tree
./qa/tests/eeschema/qa_eeschema --list_content

# run one suite, with detail
./qa/tests/eeschema/qa_eeschema --run_test=<Suite>/<Case> --log_level=all
```

Via ctest (runs the whole registered suite set):

```bash
cd /tmp/claude-0/kicad-build/build
ctest -R eeschema --output-on-failure
ctest -N            # list registered tests without running
```

Other useful test binaries built by `KICAD_BUILD_QA_TESTS=ON`: `qa_common`,
`qa_kimath`, `qa_sexpr`, `qa_api`, `qa_pcbnew`, plus the aggregate targets
`qa_all_tests` and `qa_all`.

Tests need schematic fixtures from `qa/data/`; the binary locates them via a
compiled-in path, so run it from the build tree rather than copying it elsewhere.

`qa_eeschema` links the *real* eeschema object library, so it is also the cheapest
harness for exercising the C++ document model from a test without standing up the GUI —
which makes it the natural place to validate the Rust-side FFI boundary later.

### Baseline results on this branch

Build time for `qa_eeschema`: **29m 21s** (272 edges, on top of the main build).
Full run takes about **83 seconds**.

```
1351 test cases out of 1353 passed
2 test cases out of 1353 failed
2 test cases out of 1353 aborted
997290 assertions out of 997292 passed
```

Exit code is **201** (Boost.Test's "failures occurred"), so treat any CI wrapper
accordingly. The two failures are **pre-existing defects in this branch's own
in-flight migration code**, not build or environment problems:

1. **`SchHost/LoadAndRenderProducesGeometry`** — `qa/tests/eeschema/test_sch_host.cpp:139`.
   Segfaults (`memory access violation at address: 0x508`) inside `SCH_HOST::Render()`,
   last checkpoint at line 148. `SCH_HOST` (`eeschema/host/sch_host.cpp:66`) builds a
   `KIGFX::RECORDING_GAL` and renders through it. This is the new headless render path
   and it is genuinely broken — see `04-*` notes / the GAL analysis.
2. **`ConnectivityExport/AllegroUsesPublishedNetsAndPreservesDeviceFiles`** —
   `qa/tests/eeschema/test_connectivity_export.cpp:258`.
   `NETLIST_EXPORTER_ALLEGRO::WriteNetlist()` returns false with the **legacy**
   connectivity engine forced on (`m_ConnectivityEngine = false`). Related to the
   branch's "Enable new connectivity engine by default" change.

Neither failure is caused by the configure options in this document. A green baseline
for everything else means the toolchain and dependency set here are sound.

To reproduce just these two:

```bash
./qa/tests/eeschema/qa_eeschema --run_test=SchHost --log_level=all
./qa/tests/eeschema/qa_eeschema --run_test=ConnectivityExport --log_level=all
```

There are also dedicated tests for the recording backend in the `qa_common` binary
(`qa/tests/common/gal/test_recording_gal.cpp`, `test_draw_stream.cpp`), which were
not built or run in this pass — build `qa_common` if you need that baseline too.

---

## 5. Quick reproduction (copy/paste)

```bash
# 1. deps
export DEBIAN_FRONTEND=noninteractive
apt-get update && apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build pkg-config ccache git \
  libcairo2-dev libpng-dev libglu1-mesa-dev libgl1-mesa-dev libglew-dev libegl1-mesa-dev \
  libx11-dev mesa-common-dev libgtk-3-dev libglm-dev \
  libwxgtk3.2-dev libwxgtk-webview3.2-dev libbz2-dev libssl-dev libzstd-dev zlib1g-dev \
  python3-dev python3-pytest libgit2-dev libsecret-1-dev libboost-all-dev \
  libcurl4-openssl-dev unixodbc-dev libspnav-dev nlohmann-json3-dev libfreetype6-dev \
  libharfbuzz-dev libfontconfig1-dev libpixman-1-dev gettext shared-mime-info \
  libocct-modeling-algorithms-dev libocct-modeling-data-dev libocct-data-exchange-dev \
  libocct-visualization-dev libocct-foundation-dev libocct-ocaf-dev \
  libngspice0-dev libprotobuf-dev protobuf-compiler libnng-dev libzint-dev \
  libpoppler-dev libpoppler-glib-dev
ccache -M 8G

# 2. configure
tools/build/configure-dev.sh /tmp/claude-0/kicad-build/build

# 3. build
ninja -C /tmp/claude-0/kicad-build/build -j4 eeschema qa_eeschema

# 4. test
/tmp/claude-0/kicad-build/build/qa/tests/eeschema/qa_eeschema
```

---

## 6. The same build from nix (`devenv.nix`)

The apt list in §1 is not the only way in. `devenv.nix` at the repo root describes
this environment declaratively and covers **both** halves of the work — the C++
dependency set and the nightly Rust toolchain the schematic UI needs — on Linux and
on macOS. The only host prerequisites are nix and [devenv](https://devenv.sh).

```bash
devenv shell                          # interactive
devenv shell -- <command>             # or just one command

tools/build/configure-dev.sh          # $KICAD_BUILD_DIR is ./build in the shell
ninja -C build -j<N> eeschema qa_eeschema
cd rust && cargo test
```

`devenv.lock` pins nixpkgs and the Rust overlay, so the next machine gets the same
compilers and the same library versions. Three consequences worth knowing:

* The toolchain comes from `languages.rust.channel = "nightly"`, i.e. straight from
  the overlay rather than through rustup. `rust/rust-toolchain.toml` is therefore
  inert inside the shell — it still documents the requirement for rustup users, and
  the nightly check in `cmake/KiCadRust.cmake` is satisfied either way because
  `rustc --version` reports nightly.
* The Linux half of `devenv.nix` (wxGTK3, GL, X11, libsecret, libspnav, poppler)
  mirrors §1's apt list and evaluates, but it has not been built from here; macOS
  is what the section below records.
* The library versions are nixpkgs-current, not Ubuntu-current, and they are
  considerably newer than the container's (boost 1.91 against 1.83, for instance).
  `protobuf` is deliberately pinned to the 29.x series in `devenv.nix`, matching
  what nixpkgs itself builds KiCad against.

### Verified on macOS

| | |
|---|---|
| OS / CPU | macOS 26.6, aarch64 (18 cores) |
| Compiler | clang 21.1.8 from the nix stdenv — **not** Xcode's |
| CMake / Ninja | 4.4.2 / 1.13.2 |
| wxWidgets | 3.2.11, **osx (Cocoa)** port, webview included |
| Other | boost 1.91, OCC 7.9.3, protobuf 29.6, libngspice 45, nng 1.12.3 |
| Rust | 1.100.0-nightly (2026-09-17) |

Unlike the container, `-j14` is fine here; the memory-per-job warning in
[Gotchas](#gotchas) is about that box's 15 GB, not about the build itself.

What was actually run, in that shell, with no extra `-D` flags:

| Step | Result |
|---|---|
| `tools/build/configure-dev.sh` | configures in ~5 s |
| `ninja -j14 kicommon` | clean |
| `ninja -j14 eeschema qa_eeschema` | clean — `_eeschema.kiface` and `eeschema.app` produced |
| `ninja -j10 qa_common` | clean |
| `./qa/tests/eeschema/qa_eeschema` | 1716 cases, **no errors**, exit 0 (1699 before Stage 3 added four, 1703 before Stage 4a added thirteen) |
| `./qa/tests/common/qa_common` | 1509 cases, **no errors** (1477 before Stage 4a added thirty-two) — this includes the recording-GAL and draw-stream suites in `qa/tests/common/gal/`, which §4 records as never having been run |
| `cd rust && cargo test --workspace` | 248 passed, 1 ignored, plus the 14 live-host checks |
| `ninja kicad_sch_host` | the host shared library, 30 exported symbols |
| `ninja eeschema_gpui` | the Rust binary, linked against it |
| `ctest -L rust` | 4/4, including `qa_rust_sch_sys` against the live host |
| `eeschema-gpui --schematic <file>` | opens it and draws it |

This is a later state of the branch than §4's baseline; the two failures recorded
there do not reproduce here.

One flaky failure did, and is fixed rather than recorded:
`HttpLibPlugin/EnumerateSymbolLibMaterializesFields` hung about one run in three,
in `SCH_IO_HTTP_LIB::~SCH_IO_HTTP_LIB` → `std::thread::join()`.
`stopBackgroundRefresh()` cleared its `m_refreshRunning` flag without holding
`m_refreshMutex`, so a `notify_all()` landing between the worker's predicate check
and its wait was lost and the worker then slept out its whole refresh interval.
Setting the flag under the lock is the fix; six consecutive runs pass. Unrelated to
this work beyond having blocked a clean suite — and it is a hang a user with an
HTTP library could hit on closing a project, not only a test.

Getting a macOS build to configure and compile needed five fixes in the tree. All of
them are platform bugs that were simply never exercised, not nix workarounds:

| Where | What |
|---|---|
| `CMakeLists.txt` | `-fexperimental-library` was added for *any* clang on Apple. Only Apple's own clang ships `libc++experimental`; an upstream LLVM toolchain has `std::jthread`/`std::stop_token` in the main library and fails to link with `library not found for -lc++experimental`. Now gated on `AppleClang`. |
| `CMakeLists.txt` | `CMAKE_CXX_SCAN_FOR_MODULES OFF`. Nothing here uses C++20 modules, and `clang-scan-deps` runs as a bare binary that does not see flags a compiler wrapper adds through the environment — it failed on `thirdparty/fmt` with `'algorithm' file not found` while the compiler itself was fine. |
| `CMakeLists.txt` | `FMT_MODULE OFF`, because the line above does not cover it. fmt turns `FMT_USE_CMAKE_MODULES` on by itself whenever the standard is C++20, the generator is Ninja ≥ 1.11 and the compiler is new enough, and `add_module_library( ... USE_CMAKE_MODULES )` then re-enables scanning for that one target — which fails the same way. Found after the fix above, by the `fmt-module` target being the only thing in `ninja` that still could not scan. |
| `cmake/FindOCC.cmake` | The header search knew only FHS paths. It now also applies the `opencascade` path suffix, so a prefix supplied through `CMAKE_PREFIX_PATH`/`CMAKE_INCLUDE_PATH` works — nix, Homebrew, or a local install. |
| `cmake/Findngspice.cmake` | Looked for `libngspice.so.0` on every UNIX. macOS names it `libngspice.0.dylib`, so it now lets `find_library` apply the platform's own naming. |

### macOS-specific things to keep in mind

* **`ninja` with no target does not finish, and that is not this branch's doing.**
  `kicad/project_tree.cpp` calls `wxTreeCtrl::SetStateImages`, which is a
  wxWidgets 3.3 API; the `#if` around it is
  `wxCHECK_VERSION( 3, 3, 0 ) || defined( __WXMAC__ )`, with a comment saying that
  "KiCad for macOS currently has backported SetStateImages for this control". So
  the tree assumes KiCad's own *patched* wx on macOS, and `devenv.nix` supplies a
  stock 3.2.11. The project manager therefore does not compile here. Build named
  targets — `eeschema`, `qa_eeschema`, `qa_common`, `kicad_sch_host`,
  `eeschema_gpui` — all of which are unaffected, and all of which the sections
  above use. Fixing it means either patching wx in `devenv.nix` or widening that
  guard, and it is nothing to do with the Rust UI.
* **The wx port is `osx`, not `gtk`.** The GTK-only QA helpers in
  `qa/qa_utils/CMakeLists.txt` are compiled out, so any test that reaches into
  `GtkPrintSettings` or `GdkDisplay` is a Linux-only test by construction.
* **`kiplatform` builds its Objective-C++ implementations** (`os/apple/*.mm`,
  `port/wxosx/*.mm`) instead of the `os/unix` ones, which is why libsecret and
  Poppler are Linux-only entries in `devenv.nix`.
* **Two noisy but harmless messages.** `qa_common` prints "3Dconnexion driver
  crashed during initialization" — there is no SpaceMouse driver, and support is
  simply switched off for the run. Anything linked against fontconfig used to print
  "Cannot load default config file"; `devenv.nix` now sets `FONTCONFIG_FILE`, which
  is also what lets the outline font list see the system fonts.
* **`tools/rust-gpu-testenv/` is Linux-only** — it exists to give a container a
  software Vulkan device and a headless Wayland compositor. None of it is needed
  here: `cargo test --workspace` passes as-is (248 tests, one `#[ignore]`d for a
  gpui-component leak-detector quirk), and `cargo build -p kicad-eeschema-gpui`
  links against the system Metal stack without Xcode's Metal toolchain being
  installed.
