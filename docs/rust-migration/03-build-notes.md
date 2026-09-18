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

See `TIMINGS` section appended at the end of this file. On 4 cores with a cold
ccache this is a multi-hour build; `kicommon` alone is ~400 compilation units and
`eeschema_kiface_objects` is the single largest chunk.

With a warm ccache a full rebuild drops to minutes. Do not delete
`~/.cache/ccache` between experiments.

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
