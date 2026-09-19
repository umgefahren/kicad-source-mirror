# Eeschema Architecture Survey — Rust (gpui-kit + wgpu) Migration

**Repo:** `/home/user/kicad-source-mirror`
**Branch at survey time:** `claude/bold-lamport-ireol3`, HEAD `6dd893f7` ("Document the schematic connectivity engine")
**Date:** 2026-09-18
**Status:** read-only reconnaissance of the pre-existing tree. No existing repo file was modified.

## The decided cut line

**Option 1 — "UI/view only". This is the user's decision, not a trade-off this document
re-opens.** The sections below are the reasoning and the implementation detail that support
it, not an evaluation of alternatives.

| Stays in C++ | Replaced by Rust |
|---|---|
| Document model (`SCH_ITEM` & subclasses, `SCH_SCREEN`, `SCH_SHEET_PATH`, `SCHEMATIC`, `LIB_SYMBOL`) | wxWidgets windowing: frames, dialogs, menus, toolbars, panes |
| File I/O (`eeschema/sch_io/**`, all 13 backends) | The OpenGL GAL rasteriser (`OPENGL_GAL`) and `EDA_DRAW_PANEL_GAL` |
| Connectivity engine (`eeschema/connectivity/**`), `CONNECTION_GRAPH` | Input handling (`TOOL_DISPATCHER`, `WX_VIEW_CONTROLS`) |
| ERC, netlist exporters | Pixels (wgpu) |
| `TOOL_MANAGER`, `TOOL_ACTION`, and the `eeschema/tools/**` algorithm layer | — |

**What this step does and does not achieve.** It replaces the *UI surface*. It does **not**
remove wxWidgets from the build: `wxString` stays everywhere in the model, `EDA_ITEM` keeps
inheriting `KIGFX::VIEW_ITEM`, and `eeschema_kiface_objects` keeps linking `libwx_*`. That is
fine, because the requirement this cut actually imposes is narrower and testable:
**nothing on the schematic's rendering or input path may construct a wx window.** §2.1 records
what a later, more radical step would additionally need.

**Appendices A and B (the `.kicad_sch` / `.kicad_sym` grammar specs) are reference material,
not part of this change.** They were written before the scope was narrowed and are preserved
because they will be useful later. The file format is never reimplemented in Rust under this
plan; a reviewer should skip them.

## Verdict up front

**The recording-GAL approach works. It is no longer a hypothesis — it has been implemented.**

`RECORDING_GAL` (a `KIGFX::GAL` subclass that records into a flat draw stream) and its backing
`DRAW_STREAM` now exist in `include/gal/recording/` and `common/gal/recording/`. They compile
clean against the real KiCad headers with `-Wall -Wextra`, need **no window, no GL context and
no `wxFrame`**, and their logic tests pass under ASan and UBSan. Glyphs lower to geometry
exactly as this survey predicted, because `KIFONT::STROKE_GLYPH` is already a set of polylines
and `KIFONT::OUTLINE_GLYPH` is already a `SHAPE_POLY_SET`.

Three structural facts from the survey explain why it was tractable:

1. **`KIGFX::GAL` is not an abstract interface — it is a concrete class where almost every
   virtual has an empty `{}` default body** (`include/gal/graphics_abstraction_layer.h:147`
   onwards). A new backend overrides only what it wants and inherits no-ops for the rest.
2. **A non-rasterising GAL subclass already shipped before this work**: `CALLBACK_GAL`
   (`include/callback_gal.h:26`, `common/callback_gal.cpp`) overrides exactly **one** method
   (`DrawGlyph`) and converts glyphs into stroke/triangle/outline callbacks.
3. **A *command-recording* GAL also already shipped**: `CAIRO_GAL_BASE` implements
   `BeginGroup`/`EndGroup`/`DrawGroup` by appending an enum-tagged command list
   (`enum GRAPHICS_COMMAND`, `struct GROUP_ELEMENT`, `typedef std::deque<GROUP_ELEMENT> GROUP`,
   `include/gal/cairo/cairo_gal.h:347-382`) and replaying it.

**The remaining hard parts are not the draw calls. They are:**

- a small set of **synchronous queries** the GAL must answer (§5.4) — all answerable from
  GAL-local state, with **one** genuinely awkward case (`DrawBitmap` needing `wxImage` pixels);
- **retained group caching**: `VIEW` *does* rely on the GAL retaining geometry between frames
  (§5.5), so the recorder needs real group storage, not one flat per-frame buffer;
- **`TOOLS_HOLDER::GetToolCanvas()` returns `wxWindow*` and is pure virtual** (§7.1) — the one
  hard wx type baked into the tool framework's contract;
- the tools layer's **~600 `m_frame->` call sites** and ~70 dialog invocations (§7.3, §7.4),
  each of which must be routed to a non-wx host. ~470 of those are plain model/settings access
  with no wx involvement.

**Build status:** the C++ tree **configures and builds in this container** (§3). wxWidgets 3.2
(GTK), Boost, GLM, GLEW, libgit2, curl, harfbuzz, nlohmann_json and unixODBC are installed from
the Ubuntu archive; configure succeeds in ~12 s. Linking C++ eeschema is a supported first
milestone.

---

## Table of contents

1. [Layer map & wx coupling](#1-layer-map--wx-coupling)
2. [The seam: what the cut actually separates](#2-the-seam-what-the-cut-actually-separates)
3. [Build system reality](#3-build-system-reality)
4. [QA test suite map](#4-qa-test-suite-map)
5. [The recording GAL](#5-the-recording-gal)
6. [The input seam: a non-wx tool dispatcher](#6-the-input-seam-a-non-wx-tool-dispatcher)
7. [The host seam: what `SCH_EDIT_FRAME` must be replaced with](#7-the-host-seam-what-sch_edit_frame-must-be-replaced-with)
8. [The action / hotkey / icon registry](#8-the-action--hotkey--icon-registry)
9. [Rendering rules (what the recorded primitives mean)](#9-rendering-rules)
10. [Test fixture inventory](#10-test-fixture-inventory)
- [Appendix A — `.kicad_sch` grammar (reference only)](#appendix-a--kicad_sch-grammar-reference-only)
- [Appendix B — `.kicad_sym` grammar (reference only)](#appendix-b--kicad_sym-grammar-reference-only)
- [Appendix C — Quick file index](#appendix-c--quick-file-index)

---

## 1. Layer map & wx coupling

### 1.0 Scale of the thing

```
eeschema/**.{cpp,h}          404,585 lines
eeschema/*.cpp (all)         460 files
eeschema/dialogs/*.cpp       124 files   (pure wxWidgets GUI)
eeschema/tools/              ~40 files
eeschema/sch_io/             13 importer backends; kicad_sexpr is 10,598 lines
eeschema/connectivity/       58 files (new engine)
```

### 1.1 Document model

**Files:** `eeschema/sch_item.{h,cpp}`, `sch_symbol.*`, `sch_line.*`, `sch_junction.*`,
`sch_label.*`, `sch_text.*`, `sch_bus_entry.*`, `sch_no_connect.*`, `sch_sheet.*`,
`sch_sheet_pin.*`, `sch_pin.*`, `sch_field.*`, `sch_bitmap.*`, `sch_rule_area.*`,
`sch_table.*`, `sch_tablecell.*`, `sch_shape.*`, `sch_textbox.*`, `sch_group.*`,
`sch_screen.*`, `sch_sheet_path.*`, `schematic.*`, `lib_symbol.*`, `symbol.*`.

**Inheritance chain (the key finding):**

```
KIGFX::VIEW_ITEM (include/view/view_item.h:81)   <- the *renderer's* base class
      ^
      | public
EDA_ITEM  (include/eda_item.h:97)  : public KIGFX::VIEW_ITEM, public SERIALIZABLE
      ^
SCH_ITEM  (eeschema/sch_item.h:169)
      ^
SCH_SYMBOL, SCH_LINE, SCH_LABEL_BASE, SCH_PIN, SCH_FIELD, ...
```

`VIEW_ITEM` forces every model object to implement `ViewBBox()`, `ViewGetLayers()`,
`ViewGetLOD()`, `ViewDraw()` and `GetClass() -> wxString`
(`include/view/view_item.h:103-151`). The document model is therefore **structurally fused
to the GAL view system**. `EDA_ITEM` additionally inherits `SERIALIZABLE` (protobuf IPC API)
and `INSPECTABLE` (the wxAny/wxPropertyGrid-based property system).

**wx coupling grade: LOW-to-MEDIUM, and it is *utility* coupling, not GUI coupling.**

Direct `#include <wx/...>` counts in model headers:

| File | wx includes | Which |
|---|---|---|
| `eeschema/sch_item.h` | 0 | — |
| `eeschema/sch_label.h` | 0 | — |
| `eeschema/sch_pin.h` | 0 | — |
| `eeschema/sch_field.h` | 0 | — |
| `eeschema/lib_symbol.h` | 0 | — |
| `eeschema/sch_text.h` / `sch_table.h` / `sch_shape.h` / `sch_bitmap.h` | 0 | — |
| `eeschema/schematic.h` | 0 | — |
| `eeschema/sch_symbol.h` | 3 | `wx/arrstr.h`, `wx/chartype.h`, `wx/string.h` |
| `eeschema/sch_screen.h` | 4 | `wx/arrstr.h`, `wx/chartype.h`, `wx/gdicmn.h`, `wx/string.h` |
| `eeschema/sch_sheet_path.h` | 1 | `wx/string.h` |
| `eeschema/sch_line.h` | 1 | `wx/pen.h` |
| `eeschema/sch_item.cpp` | 1 | `wx/thread.h` |
| `eeschema/sch_symbol.cpp` / `schematic.cpp` / `sch_screen.cpp` | 1-2 | `wx/log.h`, `wx/filefn.h` |

Transitively, though, `wxString` is *everywhere*: 30 uses in `sch_item.h`, 87 in
`sch_symbol.h`, 62 in `lib_symbol.h`, 44 in `sch_pin.h`, 43 in `sch_sheet.h`, 38 in
`schematic.h`, 29 in `eda_text.h`, 17 in `eda_item.h`. Every user-visible string in the model
is a `wxString`.

**Good news on geometry:** `wxPoint`/`wxSize`/`wxRect` are **completely gone** from the
schematic model — 0 occurrences in every model header checked. KiCad migrated to its own
`VECTOR2I` / `BOX2I` / `EDA_ANGLE` types in `libs/kimath/`. Geometry is already FFI-friendly
plain structs of `int`.

**GUI singletons reachable from the model** — measured by `Pgm()` / `Kiface()` / `eeconfig()`
call counts:

| File | `Pgm()` | `Kiface()` | `eeconfig()` | `ADVANCED_CFG` |
|---|---|---|---|---|
| `sch_symbol.cpp` | 0 | 0 | 0 | 0 |
| `sch_screen.cpp` | 0 | 0 | 0 | 0 |
| `lib_symbol.cpp` | 0 | 0 | 0 | 0 |
| `sch_field.cpp` | 0 | 0 | 0 | 0 |
| `schematic.cpp` | 1 | 0 | 0 | 8 |
| `sch_label.cpp` | 0 | 0 | 0 | 2 |
| `connection_graph.cpp` | 0 | 0 | 0 | 3 |
| `sch_io_kicad_sexpr.cpp` (writer) | 0 | 0 | 0 | 0 |
| `sch_io_kicad_sexpr_parser.cpp` | 1 | 0 | 0 | 0 |
| **`sch_painter.cpp`** | **0** | **1** | **29** | 2 |

So: the **model and the writer are essentially free of application singletons**. The
**painter is not** — it reaches `eeconfig()` (the `EESCHEMA_SETTINGS` object owned by the
kiface DLL) 29 times, which is the single worst GUI-coupling hotspot in the non-dialog code.

### 1.2 File I/O

**Files:** `eeschema/sch_io/kicad_sexpr/`
- `sch_io_kicad_sexpr.cpp` (2,180 lines) — the `.kicad_sch` **writer** (`save*` methods) plus the loader driver.
- `sch_io_kicad_sexpr_parser.cpp` (6,304 lines) — the **reader** for both `.kicad_sch` and `.kicad_sym`.
- `sch_io_kicad_sexpr_lib_cache.cpp` (958 lines) — the `.kicad_sym` writer + library cache.
- `sch_io_kicad_sexpr_common.cpp` (413 lines) — shared shape/enum-token formatters.
- `eeschema/schematic.keywords` (217 tokens) — feeds the generated `schematic_lexer.h` / `SCHEMATIC_LEXER`.

**Format version constants** — `eeschema/sch_file_versions.h`:

```c
#define SEXPR_SYMBOL_LIB_FILE_VERSION 20260830   // Custom user properties   (sch_file_versions.h:60)
#define SEXPR_SCHEMATIC_FILE_VERSION  20260830   // Custom user properties   (sch_file_versions.h:152)
```

Both currently `20260830`. The file also carries the entire historical changelog as commented-out
`#define`s — invaluable as a migration table (see §5.2).

**wx coupling grade: MEDIUM — utility only.** The IO layer uses `wxString`, `wxFileName`,
`wxDir`, `wxLogTrace`, `wxMemoryOutputStream`, `wxBase64Encode`, `wxImage` (via
`BITMAP_BASE` for `(image ...)` data). No GUI widgets. The bitmap path is the ugliest:
`SCH_IO_KICAD_SEXPR::saveBitmap` (`sch_io_kicad_sexpr.cpp:1163`) round-trips through
`wxImage::SaveImageData` into a `wxMemoryOutputStream`, then base64-encodes with
`wxBase64Encode` (`common/io/kicad/kicad_io_utils.cpp:73`). A Rust port replaces this with
`base64` + `image` crates; the payload is simply the original PNG/JPEG bytes.

The parser calls `Pgm().GetLanguageTag()` exactly once, at the very end of `ParseSchematic`
for font enumeration (`sch_io_kicad_sexpr_parser.cpp:~3595`) — trivially stubbable.

### 1.3 Rendering

**Files:** `eeschema/sch_painter.{h,cpp}` (3,748 lines), `eeschema/sch_view.{h,cpp}`,
`eeschema/sch_render_settings.{h,cpp}`, `eeschema/sch_draw_panel.cpp`,
`common/gal/` (the `gal` SHARED library), `include/view/view.h`, `include/layer_ids.h`,
`include/class_draw_panel_gal.h`.

Architecture: `KIGFX::VIEW` holds `VIEW_ITEM`s bucketed per layer; `EDA_DRAW_PANEL_GAL`
(a `wxScrolledCanvas`, `include/class_draw_panel_gal.h:66`) owns a `GAL` backend
(`OPENGL_GAL` or `CAIRO_GAL`); `SCH_PAINTER` is a `KIGFX::PAINTER` that translates one
`(VIEW_ITEM*, layer)` pair into immediate-mode GAL calls.

The `gal` library (`common/gal/CMakeLists.txt:56`) is a SHARED lib that links
`kicommon kimath kiplatform nlohmann_json cairo pixman OpenGL freetype harfbuzz fontconfig`
— i.e. it links `kicommon`, which links wxWidgets. It also compiles `../view/view.cpp`,
`../view/view_controls.cpp`, `../view/view_item.cpp` into itself, so **VIEW and VIEW_ITEM
live inside the GAL shared library**, not in a neutral core.

**wx coupling grade: HIGH in the *canvas*, LOW in the *painter*.** `EDA_DRAW_PANEL_GAL` *is* a
wx window. `OPENGL_GAL` is a `wxGLCanvas` subclass. `SCH_PAINTER` reads `eeconfig()` 29
times for selection thickness, hidden-pin visibility, default font, net-colour highlight
settings. **This layer is the one being replaced — see §5 for the seam and §9 for the rules it must reproduce.**

### 1.4 Tools / interaction

**Files:** `include/tool/tool_manager.h`, `tool_interactive.h`, `tool_action.h`,
`tool_dispatcher.h`, `tool_event.h`; `eeschema/tools/` (~40 tools:
`sch_selection_tool`, `sch_move_tool`, `sch_line_wire_bus_tool`, `sch_drawing_tools`,
`sch_edit_tool`, `sch_point_editor`, `sch_editor_control`, `sch_navigate_tool`,
`sch_group_tool`, `sch_align_tool`, `ee_grid_helper`, …);
`eeschema/sch_edit_frame.{h,cpp}`, `sch_base_frame.*`, `include/eda_draw_frame.h`.

Frame chain:
```
wxFrame
  ^
EDA_BASE_FRAME (include/eda_base_frame.h:115) : public wxFrame, TOOLS_HOLDER, KIWAY_HOLDER, ...
  ^
KIWAY_PLAYER
  ^
EDA_DRAW_FRAME (include/eda_draw_frame.h:81)
  ^
SCH_BASE_FRAME
  ^
SCH_EDIT_FRAME (eeschema/sch_edit_frame.h:138)
```

`TOOL_DISPATCHER` is a `wxEvtHandler` (`include/tool/tool_dispatcher.h:49`) — it translates
wx mouse/key events into `TOOL_EVENT`s. `TOOL_ACTION` carries `wxString` menu labels,
tooltips and `BITMAPS` icon enums (`include/tool/tool_action.h:305-403`).
`TOOL_MANAGER` holds a `KIGFX::VIEW_CONTROLS*` (`tool_manager.h:669`).

**wx coupling grade: HIGH (frames, dispatcher, actions) / MEDIUM (`TOOL_MANAGER`,
`TOOL_EVENT` — `tool_event.h` has only 5 `wx` mentions).** The coroutine-based tool state
machine (`libs/libcontext`) is interesting prior art but is not portable as-is.
**Replaced (§6, §7).**

### 1.5 Connectivity / ERC / netlist

Two engines coexist:

**(a) Legacy:** `eeschema/connection_graph.{h,cpp}`, `eeschema/sch_connection.{h,cpp}`.
`connection_graph.h:38` includes `<wx/string.h>`, `sch_connection.h:28` includes
`<wx/regex.h>` (bus-vector parsing uses `wxRegEx`).

**(b) New — the "schematic connectivity engine":** `eeschema/connectivity/` (58 files,
`SCH_CONNECTIVITY` namespace). Recent commits on this branch, oldest first:

```
9ecb7614  Publish nets and item rows from components
25db2822  Run the connectivity engine behind a facade
922c9fec  Own the connectivity engine from SCHEMATIC
f75eb247  Read published connectivity from the editor
9f93bd95  Run ERC on published connectivity
c978c7ad  Keep mid-move edits in the move commit
15ad0730  Enable new connectivity engine by default   (common/advanced_config.cpp)
6dd893f7  Document the schematic connectivity engine  (adds eeschema/connectivity/conn_overview.h)
```

`conn_overview.h` (303 lines) is a Doxygen `@page schematic_connectivity`. Summary of its
design, which is directly relevant to a Rust port because **it is already a pure-function
pipeline**:

- `SCHEMATIC` owns one `SCH_CONNECTIVITY::FACADE`, exposed via `SCHEMATIC::Connectivity()`.
  `ADVANCED_CFG::m_ConnectivityEngine` selects it over `CONNECTION_GRAPH`; as of `15ad0730`
  it is **on by default**.
- Two layers: `FACADE` touches the editor model (captures text context, writes results back,
  resolves published keys → live `SCH_ITEM*`); `ENGINE` runs a **14-stage pipeline in which
  no stage after input capture reads the editor model**. Each stage reads only what the
  previous stage owns. All stages share one `SESSION_KEYS` interning table that maps sheet
  paths/names/graph nodes to integer handles.
- Change detection is coarse and version-based: the only model change signal is
  `SCH_SCREEN::ConnectivityRevision()`; there is **no dirty-item list**. Cache entries carry
  `CACHE_VERSIONS`.
- `PUBLICATION` (`conn_publish.h`) is the only state consumers read. `FACADE` rejects rows
  from a screen whose revision changed after the last update, so a consumer can never read a
  net that disagrees with the display.
- Runs on the main thread; only the pure per-partition folds go to the thread pool via
  `SCH_CONNECTIVITY::ParallelFor` (`conn_tasks.h`).
- Exception-safe: if any stage throws, all stage outputs and the publication are discarded
  and rethrown; `SCH_EDIT_FRAME::RecalculateConnections()` shows it in the infobar. An empty
  publication is safe (every query reports "unconnected").

Full-update flow (`conn_overview.h`, `@section sch_conn_full`):
```
SCHEMATIC::RebuildConnectivity -> FACADE::Recalculate(rebuild=true) -> FACADE::updateModel
 -> ENGINE::clearStages -> INPUT_STORE::Capture (SCREEN_FACTS, INSTANCE_FACTS, SCREEN_ISLANDS)
 -> RECORD_STORE::Update -> PARTITIONER::Build(buses)/BindBundle -> SLOT_STORE::Update
 -> PARTITIONER::Build(nets)/DeriveSignal -> PUBLICATION::Update + AUXILIARY::Update
 -> FACADE::Update -> NETCHAIN_MANAGER::Rebuild -> FACADE::ApplyNetclasses -> SUBSCRIPTION
```
Incremental flow skips `clearStages`; `INPUT_STORE` extracts only screens whose revision
changed, `RECORD_STORE` folds only changed inputs, `COMPONENT_CACHE` evaluates cache misses
only.

Glossary terms a Rust port will need verbatim: *screen, instance, frame, fact, item fact,
instance fact, port, island, record, claim, kind, stratum, node, partition, component,
bundle, signal, slot, bus schema, leaf, row, publication, auxiliary, net code, subgraph
code, text epoch, revision, session*.

**ERC:** `eeschema/erc/` (`erc.h`, `erc_tester`, plus 40+ `qa/tests/eeschema/erc/test_*.cpp`).
Now runs against the published connectivity (`9f93bd95`).

**Netlist exporters:** `eeschema/netlist_exporters/` (KiCad XML, Spice, Orcad, CadStar, …).

**wx coupling grade: LOW-MEDIUM.** Only `wxString`/`wxArrayString`/`wxRegEx` appear in
`conn_*.h` (8 files include `<wx/string.h>`; `conn_facts.h:42` also `<wx/arrstr.h>`). The
engine itself has **no GUI dependency**. It is the single most reusable non-trivial piece of
C++ in eeschema.

### 1.6 Coupling summary

| Layer | wx dependency | Kind | Verdict |
|---|---|---|---|
| Geometry (`libs/kimath`, `VECTOR2I`, `BOX2I`, `EDA_ANGLE`, `SHAPE_*`) | none | — | **kept** |
| Document model (`SCH_*`, `LIB_SYMBOL`, `SCH_SCREEN`, `SCHEMATIC`) | `wxString` pervasive; inherits `VIEW_ITEM` | utility + structural-to-renderer | **kept unchanged** |
| `.kicad_sch` / `.kicad_sym` IO | `wxString`, `wxFileName`, `wxImage`, base64 | utility | **kept unchanged** (grammar recorded in Appendices A/B for reference only) |
| Connectivity engine (`eeschema/connectivity/`) | `wxString` only | utility | **kept unchanged** |
| ERC | `wxString` + translation | utility | **kept unchanged** |
| `SCH_PAINTER` / `KIGFX::VIEW` | `eeconfig()` ×29 | mostly settings | **KEPT** — `SCH_PAINTER` and `VIEW` are not touched; see §5 |
| `OPENGL_GAL` / `CAIRO_GAL` / `EDA_DRAW_PANEL_GAL` | `wxGLCanvas`, `wxScrolledCanvas` | **GUI** | **REPLACED** by `RECORDING_GAL` + wgpu (§5) |
| `TOOL_MANAGER`, `TOOL_ACTION`, `eeschema/tools/**` | `wxString`, `BITMAPS`, `m_frame->` | mixed | **KEPT** — rehosted (§7, §8) |
| `TOOL_DISPATCHER` / `WX_VIEW_CONTROLS` | `wxEvtHandler` | **GUI** | **REPLACED** (§6) |
| Dialogs (124 files) | wxFormBuilder | **GUI** | **deferred** — see §7.4 |

---

## 2. The seam: what the cut actually separates

Three distinct seams, in increasing order of difficulty.

```
                          ┌──────────────────────────────────────────┐
                          │  Rust  (gpui-kit window + wgpu surface)  │
                          └───▲──────────────┬───────────────────┬───┘
                              │              │                   │
        SEAM A: pixels        │              │ SEAM B: input     │ SEAM C: host
        command buffer out    │              │ events in         │ callbacks both ways
                              │              ▼                   ▼
   ┌──────────────────────────┴───┐  ┌───────────────┐  ┌──────────────────────┐
   │  RECORDING_GAL : KIGFX::GAL  │  │ RUST_VIEW_    │  │ SCH_HOST             │
   │  (new, replaces OPENGL_GAL)  │  │ CONTROLS +    │  │ (new, replaces the   │
   └──────────────▲───────────────┘  │ RUST_TOOL_    │  │  SCH_EDIT_FRAME's    │
                  │                  │ DISPATCHER    │  │  non-window duties)  │
   ┌──────────────┴───────────────┐  └───────▲───────┘  └──────────▲───────────┘
   │ KIGFX::VIEW + SCH_PAINTER    │          │                     │
   │      (UNCHANGED)             │          │                     │
   └──────────────▲───────────────┘  ┌───────┴───────┐             │
                  │                  │ TOOL_MANAGER  │◄────────────┘
   ┌──────────────┴──────────────────┴───────────────┴─────────────────────────┐
   │ SCH_ITEM / SCHEMATIC / sch_io / connectivity / ERC / eeschema::tools       │
   │                          (ALL UNCHANGED C++)                              │
   └───────────────────────────────────────────────────────────────────────────┘
```

**Seam A — pixels (§5).** A new `KIGFX::GAL` subclass. `SCH_PAINTER` and `KIGFX::VIEW` are
not touched at all. Difficulty: **medium**. Well-precedented inside KiCad itself.

**Seam B — input (§6).** A `VIEW_CONTROLS` subclass plus a dispatcher that constructs
`TOOL_EVENT`s from gpui input. `VIEW_CONTROLS` is *already* an abstract base with the wx
implementation (`WX_VIEW_CONTROLS`) as one of several possible subclasses
(`include/view/view_controls.h`, `include/view/wx_view_controls.h:47`). `TOOL_DISPATCHER` is
**not** abstract and must be reimplemented rather than subclassed, but it is only 827 lines
and most of it is wx-quirk compensation that a Rust host does not need. Difficulty: **low-medium**.

**Seam C — host (§7).** The hard one. `SCH_EDIT_FRAME` is simultaneously a `wxFrame`, the
owner of the `SCHEMATIC`, the undo/redo stacks, settings, the canvas, the tool manager and
~70 dialogs, and the tools call back into it ~600 times. Difficulty: **high**; this is where
the bulk of the work is.

### 2.1 What `eeschema_kiface_objects` contains, and why that matters

There is **no GUI-free core target**. `eeschema_kiface_objects` (`eeschema/CMakeLists.txt:679`)
is one OBJECT library holding `${EESCHEMA_SRCS}` (164 sources) **plus** `${EESCHEMA_DLGS}`
(124 dialogs), `${EESCHEMA_WIDGETS}`, `${EESCHEMA_SIM_SRCS}` and `${EESCHEMA_PRINTING}`. Its
PUBLIC link line is `common libwmf_orcad emf2svg pads_common pcm`
(`eeschema/CMakeLists.txt:707`), and `common` (`common/CMakeLists.txt:946`) links
`${wxWidgets_LIBRARIES}`, `gal`, `kicommon`, `nanodbc`, `Boost::locale`, freetype, harfbuzz.

**What this means, stated as a scope constraint rather than an objection.** wxWidgets remains
a **link-time** dependency of the C++ half under this plan, and that is accepted. The three
structural facts behind it:

- `EDA_ITEM : public KIGFX::VIEW_ITEM` (`include/eda_item.h:97`) — the model is structurally
  fused to the view system. **Irrelevant to this cut**, because the cut keeps `VIEW`.
- `EDA_ITEM` also inherits `INSPECTABLE`, whose `include/properties/property.h:33-49` pulls in
  `<wx/any.h>`, `<wx/bitmap.h>`, `<wx/font.h>`, `<wx/validate.h>` and
  **`<wx/propgrid/property.h>`** — a GUI header, in a base class of every model object.
- `common` is monolithic and is the only route to `STROKE_PARAMS`, `EDA_TEXT`, `PAGE_INFO`,
  `OUTPUTFORMATTER`, `EMBEDDED_FILES`, `KIWAY` and the outline-font stack.

**The requirement this cut actually imposes is narrower, and it is testable:** *nothing on the
schematic rendering or input path may construct a wx window.* `RECORDING_GAL` satisfies it
today — it compiles and runs its logic tests with no window, no GL context and no `wxFrame`,
while still linking against the ordinary wx-linked headers.

**What a later, more radical step would additionally need** (recorded here so it is not
rediscovered): breaking `EDA_ITEM`'s inheritance from `VIEW_ITEM` and `INSPECTABLE`, splitting
`common` into a model half and a GUI half, and re-homing the `KIFONT` stack. That is a
multi-month KiCad-side refactor. **It is explicitly out of scope and is not a prerequisite for
anything in this document.**

### 2.2 CMake target inventory (GUI-free or not)

| Target | Type | Defined at | Links wx? | GUI-free? |
|---|---|---|---|---|
| `core` | STATIC | `libs/core/CMakeLists.txt:8` | no | **yes** |
| `kimath` | STATIC | `libs/kimath/CMakeLists.txt:54` | no | **yes** |
| `sexpr` | STATIC | `libs/sexpr/CMakeLists.txt:27` | no | **yes** |
| `snap` | STATIC | `libs/snap/CMakeLists.txt:22` | no | **yes** |
| `kinng` | STATIC | `libs/kinng/CMakeLists.txt:22` | no | yes |
| `kiplatform` | STATIC | `libs/kiplatform/CMakeLists.txt:117` | yes | no |
| `kicommon` | SHARED | `common/CMakeLists.txt:392` | yes (PCH is `<wx/wx.h>`, `:410`) | no |
| `gal` | SHARED | `common/gal/CMakeLists.txt:56` | yes | no |
| `common` | STATIC | `common/CMakeLists.txt:946` | yes | no |
| `eeschema_kiface_objects` | OBJECT | `eeschema/CMakeLists.txt:679` | yes | no |
| `eeschema_kiface` | MODULE | `eeschema/CMakeLists.txt:726` | yes | no |
| `eeschema` | EXECUTABLE | `eeschema/CMakeLists.txt:663` | yes | no |

**Where the new backend lives.** `gal` is a SHARED library that already compiles
`../view/view.cpp`, `../view/view_item.cpp`, `../view/view_controls.cpp` into itself
(`common/gal/CMakeLists.txt:22-27`) alongside `opengl/` and `cairo/`. The recording backend is
a third sibling and **now exists**:

```
include/gal/recording/recording_gal.h       RECORDING_GAL : public KIGFX::GAL
include/gal/recording/draw_stream.h         DRAW_STREAM
include/gal/recording/draw_stream_abi.h     the C ABI / POD command layout
common/gal/recording/recording_gal.cpp
common/gal/recording/draw_stream.cpp
```

It needs **no** new third-party dependency — unlike `opengl/` (OpenGL, GLEW) and `cairo/`
(cairo, pixman). It compiles clean with `-Wall -Wextra` and its logic tests pass under ASan
and UBSan.

### 2.3 Rust integration in the repo today

**None.** `find . -name Cargo.toml -not -path ./thirdparty/*` → 0 results. No `corrosion`,
no `FindRust.cmake` in `cmake/`. The only "rust" string in a CMake file is English prose in
`pcb_calculator/CMakeLists.txt`.

For a mixed build, **Corrosion** (`corrosion-rs/corrosion`, `corrosion_import_crate()`) is
the standard CMake↔Cargo bridge and would go in `cmake/`. Local toolchain is present and
working: `rustc 1.94.1 (e408947bf 2026-03-25)`, `cargo 1.94.1 (29ea6fb6a 2026-03-24)`.

## 3. Build system reality

> **This section was corrected after the build agent finished.** An earlier revision of this
> document reported that the tree could not be configured. **That is no longer true and was
> only ever true of a container that had not had its dependencies installed.**

**The C++ tree configures and builds in this container.** Authoritative reference:
**`docs/rust-migration/03-build-notes.md`**, which carries the dependency list, the exact
`cmake` invocation, timings and gotchas, and which owns `/tmp/claude-0/kicad-build/`.

Summary of the verified state:

| | |
|---|---|
| OS / toolchain | Ubuntu 24.04, gcc 13.3.0, CMake 3.28.3, Ninja 1.11.1 |
| wxWidgets | **3.2.4 (GTK3)**, from `libwxgtk3.2-dev` |
| Also installed | Boost, GLM, GLEW, libgit2, curl, harfbuzz, nlohmann_json, unixODBC, cairo, pixman, freetype, fontconfig, zstd, OpenSSL |
| Source of deps | **all from the Ubuntu 24.04 archive** — nothing built from source, nothing blocked by the egress proxy |
| Configure | **succeeds on the first attempt, ~11–12 s** |
| Build progress at time of writing | `kicommon` complete (~15 min); `common` at 81/421 objects |
| Rust toolchain | `rustc 1.94.1`, `cargo 1.94.1` — present and usable |

Configure command (or `tools/build/configure-dev.sh <builddir>`):

```bash
cmake -S /home/user/kicad-source-mirror -B <builddir> -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DKICAD_BUILD_QA_TESTS=ON -DKICAD_BUILD_I18N=OFF \
  -DKICAD_UPDATE_CHECK=OFF -DKICAD_INSTALL_DEMOS=OFF
```

Two points that matter for the migration plan:

- **Linking the C++ eeschema is a supported, reproducible first milestone.** There is no
  build-availability argument against Option 1.
- **Do not exceed `-j4` on this box.** `eeschema_kiface_objects` translation units use PCH and
  are memory-hungry; 15 GB / 4 jobs is the working ratio. See the Gotchas in the build notes.

The QA suite (§4) builds as a single Boost.Test binary; `Boost::unit_test_framework` is
present and `-DKICAD_BUILD_QA_TESTS=ON` is part of the standard configure above.

## 4. QA test suite map

### 4.1 Structure

```
qa/
├── CMakeLists.txt
├── data/               <- all fixtures (350 .kicad_sch, 21 .kicad_sym, plus .kicad_pcb, .net, ...)
├── mocks/              <- qa/mocks/kicad/common_mocks.cpp  (mock Pgm/Kiface; every suite links it)
├── qa_utils/           <- shared helpers, wxWidgets-free assertions, geometry printers
├── schematic_utils/    <- qa_schematic_utils: schematic loading/fixture helpers
├── pcbnew_utils/
├── resources/
├── fuzz/               <- fuzz harnesses (qa/data/fuzz/kicad_sym/… corpus)
├── tools/
└── tests/
    ├── common/     config/  libs/  api/  cli/  diff_merge/  spice/  gerbview/
    ├── eeschema/   <- 199 entries
    └── pcbnew/
```

### 4.2 Framework

**Boost.Test** throughout. Registration:

- `cmake/KiCadQABuildUtils.cmake:52` `function( kicad_add_boost_test TEST_EXEC_TARGET TEST_NAME )`
  → `add_test( NAME ${TEST_NAME} COMMAND $<TARGET_FILE:${TEST_EXEC_TARGET}> ${BOOST_TEST_PARAMS} )`
  (line 65), plus `add_dependencies( qa_all_tests ${TEST_EXEC_TARGET} )` (line 70). With
  `KICAD_TEST_XML_OUTPUT=ON` it passes `--logger=JUNIT,warning,<name>.boost-results.xml:HRF,message`.
  With `KICAD_BUILD_QA_TESTS=OFF` the targets get `EXCLUDE_FROM_ALL` (line 76) but remain
  buildable via the `qa_all_tests` meta-target.
- `qa/tests/eeschema/CMakeLists.txt:336` `add_executable( qa_eeschema ${QA_EESCHEMA_SRCS} )`
- `:341` links `eeschema_kiface_objects common kicommon pads_common kimath qa_utils
  qa_schematic_utils markdown_lib Boost::headers Boost::unit_test_framework`
- `:356` `target_compile_definitions( qa_eeschema PUBLIC EESCHEMA )` — "pretend to be
  eeschema (for units, etc)"
- `:359` `kicad_add_boost_test( qa_eeschema qa_eeschema )`
- `:361` `setup_qa_env( qa_eeschema )`
- Entry point: `qa/tests/eeschema/test_module.cpp`; every suite also compiles
  `qa/mocks/kicad/common_mocks.cpp` ("need the mock Pgm for many functions",
  `CMakeLists.txt:47`).

**This suite builds here.** `Boost::unit_test_framework` is installed and the standard configure in §3 passes `-DKICAD_BUILD_QA_TESTS=ON`. See `docs/rust-migration/03-build-notes.md` §4 for running it.

### 4.3 eeschema test source files by area

**Schematic file format / IO / round-trip** — the ones that matter most for a Rust port:

| File | Covers |
|---|---|
| `test_save_load_schematic.cpp` | full save→load cycle of `.kicad_sch` |
| `test_sch_ellipse_roundtrip.cpp` | `(ellipse)` / `(ellipse_arc)` primitives (fmt v20260508); **no fixture file uses them — built programmatically** |
| `test_sch_line_ending_io.cpp` | `(start_shape)` / `(end_shape)` line endings (fmt v20260818) |
| `test_pin_map_roundtrip.cpp` | `(pin_maps)` / `(associated_footprints)` / `(pin_map_override)` (fmt v20260629) |
| `test_legacy_load.cpp` | pre-s-expr `.sch` legacy loader |
| `test_history_autosave_legacy.cpp` | autosave/`-bak` handling |
| `test_sch_screen.cpp` | `SCH_SCREEN` item store, rtree |
| `test_sch_sheet.cpp`, `test_sch_sheet_list.cpp`, `test_sch_sheet_path.cpp` | hierarchy, `KIID_PATH`, instance paths |
| `test_sch_symbol.cpp` | `SCH_SYMBOL` transforms, instances |
| `test_schematic.cpp`, `test_schematic_root_input.cpp` | `SCHEMATIC` lifecycle |
| `test_schematic_clipboard_export.cpp` | the `Format(SCH_SELECTION*)` clipboard dialect |
| `test_flat_hierarchy.cpp`, `test_multi_top_level_sheets.cpp` | fmt v20251012 flat hierarchy |
| `test_project_name_instances.cpp` | `(instances (project …))` grouping |
| `test_saveas_copy_subsheets.cpp` | Save-As subsheet copying |
| `test_symbol_library.cpp`, `test_symbol_library_parse_error.cpp`, `test_symbol_library_manager.cpp`, `test_symbol_library_adapter.cpp` | `.kicad_sym` load/save/error recovery |
| `test_symbol_embedded_files.cpp` | `(embedded_files)` / `(embedded_fonts)` |
| `test_symbol_svg_export.cpp` | symbol rasterisation reference |
| `test_stacked_pin_nomenclature.cpp`, `test_stacked_pin_conversion.cpp` | fmt v20250901 / v20260622 stacked-pin notation & escaping |
| `test_variant_symbol_compatibility.cpp`, `test_variant_field_resolution.cpp` | `(variant …)` blocks |
| `test_sch_dynamic_properties.cpp` | `(custom_property)` (fmt v20260830) |
| `test_sch_biu.cpp` | **basic internal-unit conversion — read this to confirm the 1 IU = 100 nm convention** |
| `qa/tests/common/test_format_units.cpp` | `FormatInternalUnits` output formatting (compiled into `qa_eeschema`) |

**Connectivity** (new engine — 16 files):
`test_connectivity_algo.cpp`, `_bus.cpp`, `_claims.cpp`, `_dump.cpp`, `_engine.cpp`,
`_export.cpp`, `_facade.cpp`, `_facts.cpp`, `_loader.cpp`, `_navigation.cpp`,
`_pin_name.cpp`, `_publish.cpp`, `_reference.cpp`, `_revision.cpp`, `_text.cpp`,
`test_incremental_netlister.cpp`, `test_update_items_connectivity.cpp`,
`test_resolve_drivers.cpp`, `test_hierarchy_driver_ranking.cpp`.

**Bus semantics:** `test_bus_parsing.cpp`, `test_bus_alias_whitespace.cpp`,
`test_bus_entry_concurrency.cpp`, `test_bus_migration_gate.cpp`,
`test_bus_migration_precondition.cpp`, `test_bus_net_name_determinism.cpp`,
`test_label_bus_connectivity.cpp`.

**Net chains** (fmt v20260512, `qa/tests/eeschema/net_chains/`, 12 files):
`test_net_chains.cpp`, `_remove.cpp`, `_branching.cpp`, `_class_color.cpp`,
`test_net_chain_remove_rename.cpp`, `_save_root_only.cpp`, `_resolve_terminals.cpp`,
`_hierarchical_roundtrip.cpp`, `_manual.cpp`, `_recalc_refresh.cpp`,
`_synthetic_filter.cpp`, `_rollback.cpp`.

**ERC** (`qa/tests/eeschema/erc/`, ~40 files): `test_erc_empty_label`, `_four_way`,
`_label_not_connected`, `_multiple_pin_to_pin`, `_rule_area`, `_stacking_pins`,
`_label_names`, `_global_labels`, `_no_connect`, `_unconnected_pins`,
`_label_connectivity`, `_connectivity_refresh`, `_hierarchy_connectivity`,
`_hierarchical_schematics`, `_label_multiple_wires`, `_marker_counts`,
`_shared_pin_multiunit`, `_unconnected_wire_endpoints`, `_bus_conflicts`,
`_wire_bus_entry`, `_ground_pins`, `_netclasses`, `_field_names`, `_footprint_paths`,
`_pin_map_capture`, `_off_grid`, `_pin_syntax`, `_implicit_power`,
`_bus_member_label_local_pins`, `_marker_deduplication`, `_net_chain_pin_to_pin`,
`_pin_context`, `_power_pin_marker_location`, `_lib_symbol_mismatch`,
`_lib_symbol_field_position`, `_footprint_filters`, `_text_var_issue24442`.

**Foreign importers** (`qa/tests/eeschema/sch_io/`): `altium/`, `database/`, `pads/`,
`geda/`, `diptrace/`, `pcad/`, `orcad/`, `cadstar/`; plus `test_eagle_plugin.cpp`,
`test_easyedapro_v3_{import,plugin}.cpp`, `test_http_lib_{connection,plugin}.cpp`.

**Rendering-adjacent** (useful oracles for §7):
`test_pin_stacked_layout.cpp`, `test_pin_text_overlap.cpp`,
`test_pin_number_default_style_clearance.cpp`, `test_autoplace_fields.cpp`,
`test_ee_grid_helper.cpp`, `test_crossing_label.cpp`, `test_diff_canvas_context.cpp`,
`test_sch_textbox.cpp`.

**Netlist export:** `test_netlist_exporter_xml_chain_gating.cpp`, `_stacked.cpp`,
`_terminal_pins.cpp`, `_nested_groups.cpp`; `netdiff.py` for netlist comparison.

**Conditionally compiled:**
- `sch_io/diptrace/test_diptrace_sch_import_local.cpp` — only if the (uncommitted) file exists
  (`CMakeLists.txt:391`).
- `sch_io/database/test_sch_io_database.cpp` — only with `KICAD_TEST_DATABASE_LIBRARIES`
  (needs a live ODBC driver), passing `QA_DBLIB_SETTINGS_PATH=qa/data/dblib/qa_dblib.kicad_dbl`.

---

## 5. The recording GAL

**Header:** `include/gal/graphics_abstraction_layer.h` (1,460 lines).
**Base class:** `class GAL_API GAL : GAL_DISPLAY_OPTIONS_OBSERVER` — `:110`.
**Reference backends:** `include/gal/opengl/opengl_gal.h` (overrides 60 methods),
`include/gal/cairo/cairo_gal.h`, `include/callback_gal.h` (overrides **1**).

### 5.0 The shape of the interface — read this first

`GAL` is **not** an abstract base class. Only two methods anywhere in the class are pure
virtual, and both are on `VIEW_CONTROLS`, not `GAL`. Every drawing virtual has an **empty
inline body**:

```cpp
virtual void DrawLine( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint ) {};
```
— `graphics_abstraction_layer.h:147`

This has three consequences a Rust-side implementer must internalise:

1. **A recording backend can be written incrementally.** Override `DrawLine` first, render a
   schematic with only lines, then add primitives. Nothing crashes; unimplemented primitives
   silently draw nothing.
2. **Forgetting to override something is silent.** There is no compiler check. The
   completeness table in §5.1 is therefore the contract: every row marked *required* must be
   overridden or that content disappears.
3. **Large parts of the "interface" are not virtual at all** — they are plain inline
   accessors over protected members of `GAL` itself (colours, line width, glyph size, justify,
   zoom, world/screen matrices, grid size/origin/colour). The recording backend gets those
   **for free** and must *not* reimplement them. This is what makes §5.4's synchronous-query
   problem tractable.

### 5.1 Complete list of `KIGFX::GAL` virtuals a new backend must consider

Grouped as requested. "Req." = must a recording backend override it for eeschema to render
correctly? Line numbers are `include/gal/graphics_abstraction_layer.h`.

#### (a) Primitive drawing — record these

| Line | Signature | Req. |
|---|---|---|
| 147 | `virtual void DrawLine( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint )` | **yes** |
| 158 | `virtual void DrawSegment( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint, double aWidth )` | **yes** |
| 167 | `virtual void DrawSegmentChain( const std::vector<VECTOR2D>& aPointList, double aWidth )` | yes |
| 168 | `virtual void DrawSegmentChain( const SHAPE_LINE_CHAIN& aLineChain, double aWidth )` | yes |
| 175 | `virtual void DrawPolyline( const std::deque<VECTOR2D>& aPointList )` | **yes** |
| 176 | `virtual void DrawPolyline( const std::vector<VECTOR2D>& aPointList )` | **yes** |
| 177 | `virtual void DrawPolyline( const VECTOR2D aPointList[], int aListSize )` | **yes** |
| 178 | `virtual void DrawPolyline( const SHAPE_LINE_CHAIN& aLineChain )` | **yes** |
| 185 | `virtual void DrawPolylines( const std::vector<std::vector<VECTOR2D>>& aPointLists )` | yes |
| 193 | `virtual void DrawCircle( const VECTOR2D& aCenterPoint, double aRadius )` | **yes** |
| 202 | `virtual void DrawHoleWall( const VECTOR2D& aCenterPoint, double aHoleRadius, double aWallWidth )` | no (pcbnew only) |
| 213 | `virtual void DrawArc( const VECTOR2D& aCenterPoint, double aRadius, const EDA_ANGLE& aStartAngle, const EDA_ANGLE& aAngle )` | **yes** |
| 236 | `virtual void DrawArcSegment( const VECTOR2D& aCenterPoint, double aRadius, const EDA_ANGLE& aStartAngle, const EDA_ANGLE& aAngle, double aWidth, double aMaxError )` | yes |
| 248 | `virtual void DrawEllipse( const VECTOR2D& aCenterPoint, double aMajorRadius, double aMinorRadius, const EDA_ANGLE& aRotation )` | yes (fmt v20260508) |
| 261 | `virtual void DrawEllipseArc( const VECTOR2D& aCenterPoint, double aMajorRadius, double aMinorRadius, const EDA_ANGLE& aRotation, const EDA_ANGLE& aStartAngle, const EDA_ANGLE& aEndAngle )` | yes |
| 270 | `virtual void DrawRectangle( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint )` | **yes** |
| 280 | `virtual void DrawGlyph( const KIFONT::GLYPH& aGlyph, int aNth = 0, int aTotal = 1 )` | **yes — see §5.3** |
| 285 | `virtual void DrawGlyphs( const std::vector<std::unique_ptr<KIFONT::GLYPH>>& aGlyphs )` | optional (base loops to `DrawGlyph`, handling hover colour) |
| 314 | `virtual void DrawPolygon( const std::deque<VECTOR2D>& aPointList )` | **yes** |
| 315 | `virtual void DrawPolygon( const VECTOR2D aPointList[], int aListSize )` | **yes** |
| 316 | `virtual void DrawPolygon( const SHAPE_POLY_SET& aPolySet, bool aStrokeTriangulation = false )` | **yes** |
| 318 | `virtual void DrawPolygon( const SHAPE_LINE_CHAIN& aPolySet )` | **yes** |
| 331 | `virtual void DrawCurve( const VECTOR2D& startPoint, const VECTOR2D& controlPointA, const VECTOR2D& controlPointB, const VECTOR2D& endPoint, double aFilterValue = 0.0 )` | **yes** (beziers) |
| 338 | `virtual void DrawBitmap( const BITMAP_BASE& aBitmap, double alphaBlend = 1.0 )` | **yes — see §5.4 hazard 1** |

Note `DrawRectangle( const BOX2I& )` at `:273` is a **non-virtual** convenience overload that
forwards to the `VECTOR2D,VECTOR2D` form. Do not try to override it.

#### (b) State / attribute setting — almost all non-virtual

Virtual (override only if the backend wants to intercept):

| Line | Signature |
|---|---|
| 388 | `virtual void SetIsFill( bool )` |
| 408 | `virtual void SetIsStroke( bool )` |
| 428 | `virtual void SetFillColor( const COLOR4D& )` |
| 448 | `virtual void SetStrokeColor( const COLOR4D& )` |
| 453 | `virtual void SetHoverColor( const COLOR4D& )` |
| 473 | `virtual void SetLineWidth( float )` |
| 483 | `virtual void SetMinLineWidth( float )` |
| 516 | `virtual void SetLayerDepth( double )` — asserts against `m_depthRange` |

**Non-virtual, state lives in `GAL`, free for any backend:** `GetIsFill()`, `GetIsStroke()`,
`GetFillColor()`, `GetStrokeColor()`, `GetLineWidth()`, `GetMinLineWidth()`,
`AdvanceDepth()` (= `SetLayerDepth(m_layerDepth - 0.1)`, `:524`), `SetClearColor()`,
`GetClearColor()`, and the whole text-attribute block `SetGlyphSize/GetGlyphSize/
SetFontBold/IsFontBold/SetFontItalic/IsFontItalic/SetFontUnderlined/IsFontUnderlined/
SetTextMirrored/IsTextMirrored/SetHorizontalJustify/GetHorizontalJustify/
SetVerticalJustify/GetVerticalJustify` (`:557-593`), plus `ResetTextAttributes()`.

> **Design note — and what `RECORDING_GAL` actually chose.** There are two defensible
> designs here. (i) Let the base class keep the state and **snapshot it into every recorded
> draw command**, producing a stateless command stream. (ii) Override the setters and record
> them as their own commands, leaving the consumer to replay a state machine — which is what
> `CAIRO_GAL` does (`CMD_SET_FILLCOLOR` etc., `cairo_gal.h:351`) because cairo is itself
> stateful.
>
> **`RECORDING_GAL` took (ii)**, with dedicated opcodes `KGDS_OP_SET_IS_FILL`,
> `KGDS_OP_SET_IS_STROKE`, `KGDS_OP_SET_FILL_COLOR`, `KGDS_OP_SET_STROKE_COLOR`,
> `KGDS_OP_SET_HOVER_COLOR`, `KGDS_OP_SET_LINE_WIDTH`, `KGDS_OP_SET_MIN_LINE_WIDTH`,
> `KGDS_OP_SET_LAYER_DEPTH` (`draw_stream_abi.h:137-153`). This keeps the stream compact —
> `SCH_PAINTER` sets colour far less often than it draws — at the cost of the Rust consumer
> maintaining a small state machine. Either choice works; the point for implementers is that
> **it is already decided, and the ABI header is the contract.**
>
> Either way, the backend must **not stop the base class from tracking state**, because
> `SCH_PAINTER` reads it back (`GetFillColor()` at `graphics_abstraction_layer.h:286` inside
> `DrawGlyphs`, `GetLineWidth()` at `callback_gal.cpp:48`). Overrides must call up to `GAL::`
> as well as emitting.

#### (c) Transform stack

| Line | Signature | Req. |
|---|---|---|
| 600 | `virtual void Transform( const MATRIX3x3D& aTransformation )` | yes |
| 607 | `virtual void Rotate( double aAngle )` | yes |
| 614 | `virtual void Translate( const VECTOR2D& aTranslation )` | yes |
| 621 | `virtual void Scale( const VECTOR2D& aScale )` | yes |
| 624 | `virtual void Save()` | yes |
| 627 | `virtual void Restore()` | yes |

`SCH_PAINTER` uses these for symbol placement and bitmap placement. Keep a `MATRIX3x3D` stack
in the recording backend and **bake the current matrix into each command**, same argument as
above.

#### (d) Group / retained-geometry management — the caching contract

| Line | Signature | Req. |
|---|---|---|
| 641 | `virtual int BeginGroup()` — **returns a group id** | **yes** |
| 644 | `virtual void EndGroup()` | **yes** |
| 651 | `virtual void DrawGroup( int aGroupNumber )` | **yes** |
| 659 | `virtual void ChangeGroupColor( int aGroupNumber, const COLOR4D& aNewColor )` | **yes** |
| 667 | `virtual void ChangeGroupDepth( int aGroupNumber, int aDepth )` | **yes** |
| 674 | `virtual void DeleteGroup( int aGroupNumber )` | **yes** |
| 679 | `virtual void ClearCache()` | **yes** |

See §5.5 — `VIEW` genuinely depends on these.

#### (e) Render targets and compositing

| Line | Signature | Req. |
|---|---|---|
| 805 | `virtual void SetTarget( RENDER_TARGET aTarget )` | **yes** |
| 812 | `virtual RENDER_TARGET GetTarget() const` — **returns** | **yes** |
| 819 | `virtual void ClearTarget( RENDER_TARGET aTarget )` | **yes** |
| 826 | `virtual bool HasTarget( RENDER_TARGET aTarget )` — **returns**; base returns `true` | **yes** |
| 842 | `virtual void SetNegativeDrawMode( bool )` | no (gerbview) |
| 851 | `virtual void StartDiffLayer()` | no (gerbview) |
| 858 | `virtual void EndDiffLayer()` | no (gerbview) |
| 868 | `virtual void StartNegativesLayer()` | no (gerbview) |
| 874 | `virtual void EndNegativesLayer()` | no (gerbview) |

`RENDER_TARGET` ∈ `TARGET_CACHED`, `TARGET_NONCACHED`, `TARGET_OVERLAY` (see
`include/gal/definitions.h`). Eeschema uses all three (`eeschema/sch_draw_panel.cpp:152-177`).
`OPENGL_GAL` overrides `SetNegativeDrawMode` as a no-op (`opengl_gal.h:278`) — the recording
backend can do the same.

#### (f) Text and font

| Line | Signature | Req. |
|---|---|---|
| 550 | `virtual void BitmapText( const wxString& aText, const VECTOR2I& aPosition, const EDA_ANGLE& aAngle )` | **no — see §5.3** |

That is the entire text surface on `GAL`. **There is no `StrokeText`, no `GetStrokeFont`, and
no text-extent query on `GAL`.** Text metrics live on `KIFONT::FONT`, above the GAL. This is
the single most important finding for the recording approach — see §5.3 and §5.4.

#### (g) Grid

| Line | Signature | Req. |
|---|---|---|
| 1038 | `virtual void DrawGrid()` | **yes** |

Everything else grid-related is **non-virtual** state on `GAL`: `SetGridVisibility`,
`GetGridVisibility`, `GetGridSnapping` (`:955`), `SetGridOrigin`/`GetGridOrigin` (`:900,:913`),
`SetGridSize`/`GetGridSize` (`:917,:930`), `GetVisibleGridSize` (`:934` — contains the
auto-sparsening logic), `SetGridColor`/`GetGridColor`, `SetAxesColor`, `SetAxesEnabled`,
`SetCoarseGrid`/`GetCoarseGrid`, `GetGridLineWidth`, `GetGridStyle`, `SetGridSources`/
`GetGridSources`, and `GetGridPoint( const VECTOR2D& )` (`:1045`). A recording backend can
therefore implement `DrawGrid()` by reading its own inherited state, or simply record a single
`Grid { origin, size, tick, style, color, axes }` command and let Rust draw it natively —
which is much better for a wgpu renderer (an infinite grid is a shader, not geometry).

#### (h) Cursor

| Line | Signature | Req. |
|---|---|---|
| 1076 | `virtual bool SetNativeCursorStyle( KICURSOR aCursor, bool aHiDPI )` — **returns**; base impl at `common/gal/graphics_abstraction_layer.cpp:350` | yes (route to gpui cursor) |
| 1113 | `virtual void DrawCursor( const VECTOR2D& aCursorPosition )` | **yes** |
| 1115 | `virtual void EnableDepthTest( bool aEnabled = false )` | yes (no-op is fine) |

`SetCursorEnabled`/`IsCursorEnabled`/`SetCursorColor` are non-virtual state.

#### (i) Lifecycle, context and screen

| Line | Signature | Req. | Notes |
|---|---|---|---|
| 124 | `virtual bool IsInitialized() const` | **yes** | base returns `true`; `VIEW::UpdateItems` gates on it (`view.cpp:1588`) |
| 127 | `virtual bool IsVisible() const` | **yes** | base returns `true`; gated in `view.cpp:851,864,996,1588` |
| 130 | `virtual bool IsCairoEngine()` | yes → `false` | |
| 133 | `virtual bool IsOpenGlEngine()` | **yes → see §5.5 hazard** | `VIEW::draw` sorts layers when this is false (`view.cpp:1175`) |
| 345 | `virtual void ResizeScreen( int aWidth, int aHeight )` | **yes** | |
| 348 | `virtual bool Show( bool aShow )` | yes | |
| 357 | `virtual int GetSwapInterval() const` | no | |
| 360 | `virtual void Flush()` | yes | |
| 377 | `virtual void ClearScreen()` | **yes** | |
| 686 | `virtual void ComputeWorldScreenMatrix()` | no — base impl at `common/gal/graphics_abstraction_layer.cpp` is correct and `OPENGL_GAL` only overrides it for GL-specific depth setup | |
| 1121 | `virtual bool IsContextLocked()` | yes → `false` | |
| 1134 | `virtual bool IsContextValid() const` | yes → `true` | |
| 1141 | `virtual void LockContext( int aClientCookie )` | yes (no-op) | |
| 1143 | `virtual void UnlockContext( int aClientCookie )` | yes (no-op) | |
| 1152 | `virtual void BeginDrawing()` | **yes** | start a new frame's buffer |
| 1156 | `virtual void EndDrawing()` | **yes** | hand the buffer to Rust |
| 1161 | `virtual void beginUpdate()` (protected) | yes | `VIEW` wraps item updates in `GAL_UPDATE_CONTEXT` |
| 1164 | `virtual void endUpdate()` (protected) | yes | |
| 1220 | `virtual bool updatedGalDisplayOptions( const GAL_DISPLAY_OPTIONS& aOptions )` (protected) | yes | base impl exists |

**Non-virtual world/screen block, all free:** `GetScreenPixelSize()` (`:349`),
`GetWorldScreenMatrix()` (`:690`), `GetScreenWorldMatrix()` (`:700`), `SetWorldScreenMatrix()`,
`GetVisibleWorldExtents()` (`:707`), `SetWorldUnitLength`/`GetWorldUnitLength`,
`SetScreenSize`, `SetScreenDPI`/`GetScreenDPI`, `SetLookAtPoint`/`GetLookAtPoint`,
`SetZoomFactor`/`GetZoomFactor`, `SetRotation`/`GetRotation`, `SetDepthRange`/`GetMinDepth`/
`GetMaxDepth`, `GetWorldScale()` (`:773`), `SetFlip`/`IsFlippedX`/`IsFlippedY`,
`ToWorld()`/`ToScreen()` (`:1052,:1059`).

### 5.2 Prior art in-tree — read both before writing a line

**`CALLBACK_GAL`** (`include/callback_gal.h:26`, `common/callback_gal.cpp`, 78 lines total).
A `KIGFX::GAL` subclass that rasterises nothing. It overrides **exactly one** method,
`DrawGlyph` (`callback_gal.cpp:29`), and forwards to one of three `std::function`s
(`m_strokeCallback(pt1,pt2)`, `m_triangleCallback(pt1,pt2,pt3)`,
`m_outlineCallback(SHAPE_LINE_CHAIN)`). It is used in production by `SCH_PAINTER`
(`sch_painter.cpp:~600`, inside `drawText` for shadow generation) and by
`EDA_TEXT::GetEffectiveTextShape` (`common/eda_text.cpp:1125`). **This is a live,
shipping proof that a non-rasterising GAL is a supported thing to build.**

**`CAIRO_GAL_BASE` group recording** (`include/gal/cairo/cairo_gal.h:340-388`). Verbatim:

```cpp
static const int MAX_CAIRO_ARGUMENTS = 4;

enum GRAPHICS_COMMAND {
    CMD_SET_FILL, CMD_SET_STROKE, CMD_SET_FILLCOLOR, CMD_SET_STROKECOLOR,
    CMD_SET_LINE_WIDTH, CMD_STROKE_PATH, CMD_FILL_PATH,
    CMD_ROTATE, CMD_TRANSLATE, CMD_SCALE, CMD_SAVE, CMD_RESTORE, CMD_CALL_GROUP
};

struct GROUP_ELEMENT {
    GRAPHICS_COMMAND m_Command;
    union { double DblArg[MAX_CAIRO_ARGUMENTS]; bool BoolArg; int IntArg = 0; } m_Argument;
    cairo_path_t*    m_CairoPath = nullptr;
};

typedef std::deque<GROUP_ELEMENT> GROUP;

bool                 m_isGrouping;
bool                 m_isElementAdded;
std::map<int, GROUP> m_groups;
unsigned int         m_groupCounter;
GROUP*               m_currentGroup;
```

This is *already* a command recorder with a group table keyed by `int`, a counter for
`BeginGroup()` ids, and `CMD_CALL_GROUP` for nesting. The recording backend is the same
structure with FFI-friendly payloads instead of `cairo_path_t*`.

### 5.3 Text — the good news

**Text never reaches the GAL as a string.** The path is:

```
SCH_PAINTER::drawText            sch_painter.cpp:587
  -> KIFONT::FONT::Draw(GAL*, wxString, pos, cursor, TEXT_ATTRIBUTES, METRICS, ...)
                                  common/font/font.cpp:240
       -> getLinePositions(...)   (multi-line split; pure geometry)
       -> drawSingleLineText(...) common/font/font.cpp:~400
            -> GetTextAsGlyphs(...)  -> std::vector<std::unique_ptr<KIFONT::GLYPH>>
            -> aGal->DrawGlyphs( glyphs )    common/font/font.cpp:435
                 -> (base GAL loops) -> aGal->DrawGlyph( glyph, i, n )
```

`KIFONT::GLYPH` (`include/font/glyph.h:40`) has exactly two concrete forms:

| Class | Declared at | Really is | What the recorder gets |
|---|---|---|---|
| `KIFONT::STROKE_GLYPH` | `glyph.h:99` | `public GLYPH, public std::vector<std::vector<VECTOR2D>>` | **a list of polylines** — iterate the outer vector, each inner vector is a stroke path |
| `KIFONT::OUTLINE_GLYPH` | `glyph.h:59` | `public GLYPH, public SHAPE_POLY_SET` | **a filled polygon set**, possibly with holes; `Triangulate(cb)` and `Fracture()` available |

Discriminate with `GLYPH::IsStroke()` / `GLYPH::IsOutline()` (`glyph.h:46-47`).
`CALLBACK_GAL::DrawGlyph` (`common/callback_gal.cpp:29-77`) is a complete worked example of
consuming both, including the `TransformOvalToPolygon(..., strokeWidth, strokeWidth/180,
ERROR_INSIDE)` trick for turning a stroke glyph into outlines.

**Therefore:**

- **Rust never needs a font engine.** Freetype/harfbuzz/fontconfig stay on the C++ side and
  produce geometry. The recorder emits `GlyphStroke { polylines, width }` and
  `GlyphFill { poly_set }` commands, which wgpu renders like any other geometry.
- **`BitmapText` does not need overriding.** The base implementation
  (`common/gal/graphics_abstraction_layer.cpp:330-347`) falls back to
  `KIFONT::FONT::GetFont()->Draw( this, ... )` with `m_Size.y * 0.95` and
  `m_StrokeWidth = GetLineWidth() * 0.74`. `OPENGL_GAL` overrides it with a texture atlas for
  speed; a recording backend can simply inherit the geometric fallback and lose nothing but
  a small amount of LOD performance. (`SCH_PAINTER` calls `bitmapText` when
  `textSize * GetWorldScale() < 3.5`, `sch_painter.cpp:1163`.)
- **Glyph caching still works.** `EDA_TEXT` caches `std::vector<std::unique_ptr<GLYPH>>` per
  item; the expensive shaping is not repeated per frame.

### 5.4 Synchronous queries — the methods that cannot be recorded and deferred

This is the list requested. Every GAL method that **returns a value the caller acts on
immediately**, and therefore cannot be an async command.

#### Virtual, returning — all answerable from GAL-local state

| Line | Signature | Who asks | How the recorder answers |
|---|---|---|---|
| 641 | `virtual int BeginGroup()` | `VIEW::updateItemGeometry`, `view.cpp:1443` | Allocate from a local `m_groupCounter`, insert an empty `GROUP` into a local `std::map<int, GROUP>`. Exactly as `CAIRO_GAL`. **Synchronous, trivial, no round trip to Rust.** |
| 812 | `virtual RENDER_TARGET GetTarget() const` | rarely; base returns `TARGET_CACHED` | Return the locally tracked current target. |
| 826 | `virtual bool HasTarget( RENDER_TARGET )` | `EDA_DRAW_PANEL_GAL::DoRePaint`, `draw_panel_gal.cpp:387` — if `TARGET_OVERLAY` is dirty and `!HasTarget(OVERLAY)`, `VIEW::MarkDirty()` is forced | Return `true` for all three targets. |
| 124 | `virtual bool IsInitialized() const` | `VIEW::UpdateItems`, `view.cpp:1588`; `DoRePaint`, `draw_panel_gal.cpp:267` | Local bool, set once the Rust surface exists. |
| 127 | `virtual bool IsVisible() const` | `view.cpp:851, 864, 996, 1588`; `draw_panel_gal.cpp:267` | Local bool from the host. |
| 130/133 | `IsCairoEngine()` / `IsOpenGlEngine()` | `view.cpp:1175` | See the hazard below. |
| 348 | `virtual bool Show( bool )` | host | Local. |
| 357 | `virtual int GetSwapInterval() const` | diagnostics only | `0`. |
| 1076 | `virtual bool SetNativeCursorStyle( KICURSOR, bool aHiDPI )` | `VIEW_CONTROLS` / tools | Forward to gpui's cursor API; must answer **now** whether the style was applied. Base impl: `common/gal/graphics_abstraction_layer.cpp:350`. |
| 1121 | `virtual bool IsContextLocked()` | `draw_panel_gal.cpp:267` | `false`. |
| 1134 | `virtual bool IsContextValid() const` | GL recovery path | `true`. |
| 1220 | `virtual bool updatedGalDisplayOptions( const GAL_DISPLAY_OPTIONS& )` (protected) | options observer | Call the base, then mark dirty. |

#### Non-virtual, returning — inherited for free, *no work required*

These are the ones that would have been fatal if they were virtual and backend-specific. They
are not: they read `GAL`'s own protected members.

`GetScreenPixelSize()` · `GetWorldScreenMatrix()` · `GetScreenWorldMatrix()` ·
`GetVisibleWorldExtents()` · `GetWorldScale()` · `GetZoomFactor()` · `GetLookAtPoint()` ·
`GetRotation()` · `GetMinDepth()` / `GetMaxDepth()` · `IsFlippedX()` / `IsFlippedY()` ·
`ToWorld()` / `ToScreen()` · `GetGridPoint()` · `GetVisibleGridSize()` · `GetGridSize()` ·
`GetGridOrigin()` · `GetGridColor()` · `GetGridStyle()` · `GetCoarseGrid()` ·
`GetGridLineWidth()` · `GetGridSnapping()` · `GetGridVisibility()` · `GetFillColor()` ·
`GetStrokeColor()` · `GetIsFill()` · `GetIsStroke()` · `GetLineWidth()` · `GetMinLineWidth()` ·
`GetClearColor()` · `GetGlyphSize()` · `IsFontBold()` / `IsFontItalic()` / `IsFontUnderlined()` ·
`IsTextMirrored()` · `GetHorizontalJustify()` / `GetVerticalJustify()` · `IsCursorEnabled()` ·
`GetScreenDPI()` · `GetWorldUnitLength()` · `GetGridSources()`.

`SCH_PAINTER` leans on this set heavily — `GetScreenWorldMatrix()` for the zoom-relative
selection halo (`sch_painter.cpp:297`), the anchor cross (`:1602`) and operating-point text
size (`:568`); `GetWorldScale()` for the bitmap-text LOD threshold (`:1164`);
`GetFillColor()`/`GetStrokeColor()` inside `DrawGlyphs` (`graphics_abstraction_layer.h:286`).
**All of it works unchanged.**

#### Hazards — the genuinely awkward ones

**Hazard 1 — `DrawBitmap( const BITMAP_BASE& aBitmap, double alphaBlend )`, `:338`.**
This is the only primitive that carries an opaque, wx-typed payload.
`BITMAP_BASE::GetImageData()` returns a **`wxImage*`** (`include/bitmap_base.h:64`).
Usable accessors: `GetImageID() -> KIID` (`:72`), `GetSizePixels() -> VECTOR2I` (`:103`),
`GetPPI() -> int` (`:114`), `GetSize() -> VECTOR2I` (`:98`).
**Recommended handling:** record
`Bitmap { image_id: Kiid, w: u32, h: u32, alpha: f32, transform: Mat3 }` and, on first sight
of an unknown `image_id`, do one synchronous upload of RGBA bytes pulled out of the `wxImage`
(`wxImage::GetData()` gives packed RGB; `GetAlpha()` gives the alpha plane, when present).
Rust caches by `KIID` and never asks again. This works because `KIID` is stable per image and
`SCH_BITMAP` images are few (31 fixture files use `(image …)` at all).
**Do not** try to defer the pixel fetch — `BITMAP_BASE` may be freed before the command buffer
is consumed if the item is deleted mid-frame.

**Hazard 2 — `IsOpenGlEngine()`, `:133`.** `VIEW::draw( VIEW_ITEM*, bool )` at
`common/view/view.cpp:1175` reads:
```cpp
if( !m_gal || !m_gal->IsOpenGlEngine() )
    // sort layers, "needed for drawing order dependent GALs (like Cairo)"
```
Returning `false` makes `VIEW` sort each item's layers before drawing — correct but slower.
Returning `true` skips the sort and tells `VIEW` the backend honours `SetLayerDepth()` as a
real depth value. A wgpu renderer **can** honour depth (it is a z value), so
**return `true`** and implement depth ordering. There are 4 other `IsOpenGlEngine()` call
sites across the tree; audit them before flipping this.
Also note `EDA_DRAW_PANEL_GAL::DoRePaint` branches on `m_backend == GAL_TYPE_OPENGL` for the
clear-every-frame decision (`draw_panel_gal.cpp:400`) — that is a `GAL_TYPE` enum on the
panel, not on the GAL, and the panel is being replaced anyway.

**Hazard 3 — `SetLayerDepth` asserts.** `:516` does
`wxCHECK_MSG( aLayerDepth <= m_depthRange.y, ... )` and `>= m_depthRange.x`. The host must
call `SetDepthRange()` with a sane range before the first frame or every layer trips an
assertion. `OPENGL_GAL` sets it in its constructor.

**Hazard 4 — nothing reads pixels back.** Searched: there is **no** framebuffer-readback,
screenshot or `GetBitmap()` method on `GAL`. Printing and plotting go through a completely
separate `PLOTTER` hierarchy (`eeschema/sch_plotter.cpp`), not the GAL. So the command stream
is strictly write-only. **This is the single biggest reason the recording approach is viable.**

### 5.5 How `VIEW` drives the GAL — and whether it needs retained geometry

**Yes, it does.** This is the one place where a naive "flat command buffer per frame" design
breaks.

#### Frame loop

```
EDA_DRAW_PANEL_GAL::DoRePaint          common/draw_panel_gal.cpp:257
  guard: m_gal->IsInitialized() && IsVisible() && !IsContextLocked()   :267
  early-out: if !viewDirty && !cursorMoved && !hasPendingItemUpdates   :324
  m_view->UpdateItems()                                                :336
  early-out again if still not dirty                                   :360
  { GAL_DRAWING_CONTEXT ctx( m_gal );        // RAII BeginDrawing/EndDrawing  :372
    if TARGET_OVERLAY dirty && !m_gal->HasTarget(OVERLAY) -> m_view->MarkDirty()  :387
    m_gal->SetClearColor / SetGridColor / SetCursorColor                :394-396
    if backend == OPENGL: m_gal->ClearScreen()                          :400
    if m_view->IsDirty():
        m_gal->ClearScreen()  (non-GL, when NONCACHED dirty)            :408
        m_view->ClearTargets()                                          :411
        if IsTargetDirty(TARGET_NONCACHED): prepareGridSources(); m_gal->DrawGrid()  :414-418
        m_view->Redraw()                                                :421
    m_gal->DrawCursor( cursorPos )                                      :426
  }                                          // ctx dtor -> EndDrawing
```

#### `VIEW::Redraw` and `redrawRect`

`VIEW::Redraw()` (`common/view/view.cpp:1278`) computes the **whole visible world rect** from
`m_gal->GetScreenPixelSize()` and calls `redrawRect( recti )`, then `MarkClean()`.

> **There is no partial dirty-rectangle mechanism.** Dirtiness is tracked *per render target*
> (`IsTargetDirty(TARGET_CACHED|NONCACHED|OVERLAY)`, `include/view/view.h:648`), never per
> region. A Rust renderer does not need scissor/damage tracking to be correct.

`VIEW::redrawRect` (`view.cpp:1096`):
```cpp
for( VIEW_LAYER* l : m_orderedLayers ) {
    if( l->items->IsEmpty() ) continue;
    if( l->visible && IsTargetDirty(l->target) && areRequiredLayersEnabled(l->id) ) {
        m_gal->SetTarget( l->target );            // :1109
        m_gal->SetLayerDepth( l->renderingOrder );// :1110
        l->items->Query( aRect, drawFunc );       // :1119  VIEW_RTREE spatial query
        ...
    }
}
```
So: **iterate layers in `m_orderedLayers` order, set target + depth, spatially query that
layer's R-tree, draw each hit.** `m_orderedLayers` is sorted by `renderingOrder`, which
eeschema sets from `SCH_LAYER_ORDER` (`eeschema/sch_view.h:44`, see §9.3).

#### The caching contract — `VIEW` *does* rely on the GAL retaining geometry

`VIEW::draw( VIEW_ITEM*, int aLayer, bool aImmediate )` (`view.cpp:1145`):
```cpp
if( m_layerCachedFlagCache[ aLayer ] && !aImmediate ) {
    int group = viewData->getGroup( aLayer );
    if( group >= 0 ) m_gal->DrawGroup( group );   // <-- replay retained geometry
    else             Update( aItem );             // <-- build it
} else {
    if( !m_painter->Draw( aItem, aLayer ) )       // immediate mode
        aItem->ViewDraw( aLayer, this );
}
```
`m_layerCachedFlagCache[layer]` is `IsCached(layer)` == `layer.target == TARGET_CACHED`
(`include/view/view.h:666`).

`VIEW::updateItemGeometry` (`view.cpp:1420-1450`):
```cpp
if( l.target != TARGET_CACHED ) return;           // :1431
m_gal->SetTarget( l.target );
m_gal->SetLayerDepth( l.renderingOrder );
int group = viewData->getGroup( aLayer );
if( group >= 0 ) m_gal->DeleteGroup( group );     // :1441
group = m_gal->BeginGroup();                      // :1443
viewData->setGroup( aLayer, group );
m_painter->Draw( aItem, aLayer ) || aItem->ViewDraw( aLayer, this );
m_gal->EndGroup();                                // :1449
```

`VIEW` also calls `ChangeGroupColor` (`view.cpp:1395`, for highlight recolouring without
re-drawing), `ChangeGroupDepth` (`:1015`, when layer ordering changes), `DeleteGroup`
(`:484`, `:1441`, `:1522`) and `ClearCache` (`:1255`).

**Implication for the recording backend:**

- `BeginGroup()`/`EndGroup()` must capture commands into a **persistent, id-keyed buffer**, not
  the current frame's buffer.
- `DrawGroup(id)` during a frame must emit a **reference** to that buffer (a
  `CallGroup { id, target, depth }` command), not a copy. Rust keeps GPU buffers per group id
  and issues an instanced/indirect draw.
- `ChangeGroupColor(id, color)` must be cheap and must **not** require re-recording. So the
  Rust-side group buffer needs a per-group uniform colour slot that can be overwritten. This
  is exactly how `OPENGL_GAL` does it (`CACHED_CONTAINER` + `VERTEX_MANAGER::ChangeItemColor`).
- `ChangeGroupDepth(id, depth)` likewise.
- `ClearCache()` drops every group.
- Alternative escape hatch: set every eeschema layer to `TARGET_NONCACHED` via
  `VIEW::SetLayerTarget` (`include/view/view.h:509`) and take the immediate-mode path.
  **Do not do this for v1-final** — it re-runs `SCH_PAINTER` over every visible item every
  frame, which on `demos/vme-wren/` (36 sheets, very large symbols) is exactly the workload
  the cache exists for. It is, however, a perfectly good **bring-up shortcut**: ship
  immediate-mode first, add group caching second.

#### `VIEW::UpdateItems` and the R-tree

`VIEW::UpdateItems` (`view.cpp:1586`) walks `m_allItems`, counts items with
`m_requiredUpdate & (GEOMETRY | LAYERS)`, and if more than **5%** of items changed it clears
every layer R-tree and bulk-rebuilds (`:1628-1640`) rather than doing individual
remove+insert. This is pure C++ bookkeeping; the recording backend is not involved beyond
`beginUpdate()`/`endUpdate()`.

`VIEW_RTREE` is `include/view/view_rtree.h`; `VIEW_ITEM_DATA` holds the per-layer group ids
(`viewData->getGroup(layer)` / `setGroup(layer, group)`), the cached bbox and the
`m_requiredUpdate` flags.

### 5.6 The shipped command-stream shape

`RECORDING_GAL` records into `DRAW_STREAM`; the wire format is `draw_stream_abi.h`, a POD C
ABI with no pointers, designed to be read directly from Rust.

**Opcodes** (`kgds_op`, `include/gal/recording/draw_stream_abi.h:99-241`):

| Range | Opcodes |
|---|---|
| frame / structure | `KGDS_OP_NOP 0x00`, `BEGIN_FRAME 0x01`, `END_FRAME 0x02`, `CLEAR_SCREEN 0x03`, `DRAW_GROUP 0x04`, `SET_TARGET 0x05`, `CLEAR_TARGET 0x06`, `START_DIFF_LAYER 0x07`, `END_DIFF_LAYER 0x08`, `START_NEGATIVES_LAYER 0x09`, `END_NEGATIVES_LAYER 0x0A` |
| state | `SET_IS_FILL 0x10`, `SET_IS_STROKE 0x11`, `SET_FILL_COLOR 0x12`, `SET_STROKE_COLOR 0x13`, `SET_HOVER_COLOR 0x14`, `SET_LINE_WIDTH 0x15`, `SET_MIN_LINE_WIDTH 0x16`, `SET_LAYER_DEPTH 0x17`, `SET_NEGATIVE_DRAW_MODE 0x18`, `ENABLE_DEPTH_TEST 0x19` |
| transform | `TRANSFORM 0x20`, `ROTATE 0x21`, `TRANSLATE 0x22`, `SCALE 0x23`, `SAVE 0x24`, `RESTORE 0x25` |
| geometry | `LINE 0x30`, `SEGMENT 0x31`, `SEGMENT_CHAIN 0x32`, `POLYLINE 0x33`, `POLYGON 0x34`, `CIRCLE 0x35`, `ARC 0x36`, `ARC_SEGMENT 0x37`, `RECTANGLE 0x38`, `CURVE 0x39`, `ELLIPSE 0x3A`, `ELLIPSE_ARC 0x3B`, `HOLE_WALL 0x3C`, `BITMAP 0x3D` |
| chrome | `GRID 0x40`, `CURSOR 0x41` |

**Flags** (`:250-257`): `KGDS_FLAG_HOLE = 1<<0` (a polygon run is a hole in the preceding
outline), `KGDS_FLAG_CLOSED = 1<<1`, `KGDS_FLAG_GLYPH = 1<<2` (this geometry came from text —
lets a consumer batch or LOD glyph geometry separately).

**Targets** (`:263-266`): `KGDS_TARGET_CACHED 0`, `KGDS_TARGET_NONCACHED 1`,
`KGDS_TARGET_OVERLAY 2`, `KGDS_TARGET_TEMP 3`. The first three map 1:1 to
`KIGFX::RENDER_TARGET`.

**`DRAW_STREAM` API** (`include/gal/recording/draw_stream.h:65`):

| Line | Member | Purpose |
|---|---|---|
| 71 | `void Clear()` | |
| 80 / 83 / 85 | `BeginFrame(w,h)` / `EndFrame()` / `IsFrameOpen()` | frame bracket |
| 95 / 98 / 101 / 104 / 106 / 109 / 111 | `BeginGroup() -> int`, `EndGroup()`, `DrawGroup(id)`, `DeleteGroup(id)`, `HasGroup(id)`, `ClearGroups()`, `GroupCount()` | **retained geometry (§5.5)** |
| 122 / 125 | `Compact()`, `FragmentationRatio()` | arena maintenance after many `DeleteGroup`s |
| 136 | `Emit( kgds_op, flags, arg0, ... )` | |
| 141-159 | `PushCoords/PushCoord/PushPoint/PushPoints/PushString/PushImage` | the shared arena |
| 162 | `static PackColor( const COLOR4D& ) -> uint32_t` | RGBA8 |
| 172 | `kgds_stream_view Publish() const` | **the FFI handoff** — a view, not a copy |
| 175 / 178 / 181 | `GroupCommandCount()`, `FrameCommandCount()`, `MemoryUsage()` | |
| 192 / 195 | `Serialize( std::ostream& )` / `Deserialize( std::istream& )` | **golden-file testing without a GPU** |

Two implementation details worth knowing:

- **Group commands and frame commands are separate buffers** (`GroupCommandCount()` vs
  `FrameCommandCount()`, `draw_stream.h:175,178`). A frame emits `KGDS_OP_DRAW_GROUP` as a
  *reference*; the group's own commands persist across frames. This is exactly the contract
  §5.5 derives from `VIEW::updateItemGeometry`.
- **Bitmaps are interned by document id.** `RECORDING_GAL::internBitmap( const BITMAP_BASE& )`
  (`recording_gal.h:191`) interleaves the `wxImage` RGB and alpha planes into the arena, and
  `std::map<KIID, std::uint32_t> m_bitmapCache` (`:200`) keys them by `BITMAP_BASE::GetImageID()`
  "so that a schematic with a repeated logo stores its pixels once". This resolves Hazard 1
  of §5.4 exactly as predicted.
- `std::vector<VECTOR2D> m_scratch` (`:205`) is reused when converting integer point runs,
  "to keep a redraw from allocating once per polygon".

Coordinates in the stream are `double`s in **world** space. The world→screen matrix is
available non-virtually from `GAL` (§5.4), so projection happens on the GPU.

### 5.7 Verdict on the recording GAL

**It works, and it is built.** `RECORDING_GAL` overrides the primitives, state setters,
transforms, groups, targets, `DrawGlyph`, `DrawBitmap`, `DrawGrid`, `DrawCursor`,
`BeginDrawing`/`EndDrawing`, `ResizeScreen`, `ClearScreen` and the `IsInitialized`/`IsVisible`
predicates — i.e. substantially the "required" column of §5.1. It compiles clean with
`-Wall -Wextra` against unmodified KiCad headers and its logic tests pass under ASan/UBSan,
with no window, no GL context and no `wxFrame`.

Remaining risks, now that the core is proven:

| Risk | Severity | Mitigation |
|---|---|---|
| Group/retained caching must be real | **addressed** | `DRAW_STREAM` keeps group commands in a separate persistent buffer from frame commands (`GroupCommandCount()` vs `FrameCommandCount()`). Still needs end-to-end validation against `VIEW` under real editing churn, plus `Compact()` tuning. |
| `DrawBitmap` needs `wxImage` pixels synchronously | **resolved** | `RECORDING_GAL::internBitmap` interleaves RGB+alpha into the arena, cached in `m_bitmapCache` keyed by `BITMAP_BASE::GetImageID()`. |
| Silent no-op overrides (no compiler enforcement) | medium | The §5.1 table is the checklist. Add a debug build that `wxFAIL`s on any un-overridden virtual. |
| `IsOpenGlEngine()` semantics | low | Return `true` and implement depth; audit the 5 call sites. |
| `SetLayerDepth` assertion | low | Call `SetDepthRange()` at init. |
| Text | **none — confirmed** | Arrives as `STROKE_GLYPH` polylines / `OUTLINE_GLYPH` polygons, tagged `KGDS_FLAG_GLYPH`. No font work in Rust. |
| Pixel readback | **none** | Nothing reads back. Plot/print use `PLOTTER`, not `GAL`. |
| Text extents / metrics queries | **none** | Live on `KIFONT::FONT`, above the GAL. |

## 6. The input seam: a non-wx tool dispatcher

**Files:** `include/tool/tool_dispatcher.h` (declaration),
`common/tool/tool_dispatcher.cpp` (827 lines), `include/tool/tool_event.h`,
`include/view/view_controls.h`, `include/view/wx_view_controls.h`.

### 6.1 What `TOOL_DISPATCHER` is

`class TOOL_DISPATCHER : public wxEvtHandler` — `tool_dispatcher.h:49`. Its own docstring
(`:17-22`) states the job precisely:

> - takes wx events,
> - fixes all wx quirks (mouse warping, panning, ordering problems, etc)
> - translates coordinates to world space
> - low-level input conditioning (drag/click threshold), updating mouse position during
>   view auto-scroll/pan.
> - issues TOOL_EVENTS to the tool manager

It is **not** abstract and has **no** virtual factory: a Rust host cannot subclass it
meaningfully, it must **write a replacement** with the same output behaviour. The good news is
that the second bullet — "fixes all wx quirks" — is the majority of the 827 lines, and a gpui
host does not have those quirks.

It is registered on the canvas by `EDA_DRAW_PANEL_GAL::SetEventDispatcher()`
(`include/class_draw_panel_gal.h:201`, member `m_eventDispatcher` at `:359`).

### 6.2 Exact wx coupling

Event types consumed (`DispatchWxEvent`, `tool_dispatcher.cpp:548`):
`wxEVT_MOTION`, `wxEVT_MOUSEWHEEL`, `wxEVT_MAGNIFY`, `wxEVT_LEFT_DOWN/UP/DCLICK`,
`wxEVT_RIGHT_DOWN/UP/DCLICK`, `wxEVT_MIDDLE_DOWN/UP/DCLICK`, `wxEVT_AUX1_*`, `wxEVT_AUX2_*`,
`wxEVT_CHAR_HOOK`, `wxEVT_CHAR`, `wxEVT_MENU_OPEN`, `wxEVT_MENU_CLOSE`,
`wxEVT_MENU_HIGHLIGHT`, and the custom `KIGFX::WX_VIEW_CONTROLS::EVT_REFRESH_MOUSE`
(`wx_view_controls.h:123` — synthesised when the cursor keeps its screen position but moves in
world space, i.e. during autopan).

wx APIs it calls:

| Call | Where | Why | Rust replacement |
|---|---|---|---|
| `wxWindow::FindFocus()` | `:558` | decide whether a text control has focus | gpui focus handle |
| `holderWindow->SetFocus()` | `:586` | grab focus when nothing has it | gpui focus |
| `GetToolCanvas()->HasFocus() / SetFocus()` | `:593-596` | focus the canvas on click | gpui focus |
| `wxGetMouseState()` | `:100` (`BUTTON_STATE::GetState`) | **poll** actual button state to recover from lost button-up events | gpui gives reliable up events; keep a local bitmask |
| `wxGetKeyState( wxKeyCode )` | `:542`, `:566` | detect stale auto-repeat after key release | gpui key up/down state |
| `wxGetLocalTimeMillis()` | `:268`, `:544` | drag-time threshold, auto-repeat window | `std::time::Instant` |
| `wxSystemSettings::GetMetric( wxSYS_DRAG_X / wxSYS_DRAG_Y )` | `:132-133` | OS drag thresholds; fall back to `DragDistanceThreshold = 8` px | platform query or the constant |
| `KIPLATFORM::APP::ForceTimerMessagesToBeCreatedIfNecessary()` | `:563` | win32-only wxTimer starvation workaround | **delete** |
| `KIUI::IsInputControlFocused/IsInputControlEditable` | `:673-674` | never send keys to tools while a text field has focus | gpui focus role |
| `KIPLATFORM::UI::IsWindowActive()` | `:580` | avoid focus fighting with modals | gpui window activation |
| `wxMOD_CONTROL/ALT/SHIFT/META/ALTGR` | `decodeModifiers`, `:159-190` | modifier decode | see §6.4 |
| `WXK_*` key codes | throughout | **the hotkey vocabulary** | see §6.5 — **must be reproduced exactly** |

**Reaching back into a `wxWindow`:** only through `m_toolMgr->GetToolHolder()` (a
`TOOLS_HOLDER*`, `dynamic_cast`ed to `wxWindow*` at `:576`) and
`GetToolHolder()->GetToolCanvas()` (declared `virtual wxWindow* GetToolCanvas() const = 0;`,
`include/tool/tools_holder.h:165`). **That pure virtual is the single hard wx type in the
tool-holder contract** — see §7.1.

**Mouse position never comes from the wx event.** It comes from
`m_toolMgr->GetViewControls()->GetMousePosition()` (`:615-616`) — world coords — and
`GetMousePosition(false)` for screen coords. So a Rust dispatcher only has to feed
`VIEW_CONTROLS`; position flows from there.

### 6.3 The `TOOL_EVENT` construction it performs — the actual contract

This is what a Rust dispatcher must reproduce. Every construction site:

**Mouse** (`handleMouseButton`, `:219-325`):

| Condition | Emitted |
|---|---|
| button pressed, was not pressed | `TOOL_EVENT( TC_MOUSE, TA_MOUSE_DOWN, button\|mods )` — `:276` |
| button released **and** `st->dragging` | `TOOL_EVENT( TC_MOUSE, TA_MOUSE_UP, button\|mods )` — `:283` |
| button released and **not** dragging | `TOOL_EVENT( TC_MOUSE, TA_MOUSE_CLICK, button\|mods )` — `:288`, position = `st->downPosition` |
| double-click event | `TOOL_EVENT( TC_MOUSE, TA_MOUSE_DBLCLICK, button\|mods )` — `:294` |
| pressed **and** motion **and** past threshold | `TOOL_EVENT( TC_MOUSE, TA_MOUSE_DRAG, button\|mods )` — `:314`, plus `evt->setMouseDragOrigin( st->dragOrigin )` and `evt->setMouseDelta( m_lastMousePos - st->dragOrigin )` |
| motion with no button event | `TOOL_EVENT( TC_MOUSE, TA_MOUSE_MOTION, mods )` — `:632` |
| wheel with **more than one** modifier bit set | `TOOL_EVENT( TC_MOUSE, TA_MOUSE_WHEEL, mods )` + `evt->SetParameter<int>( wheelRotation )` — `:649-650` |

Every event then gets `evt->SetMousePosition( isClick ? st->downPosition : m_lastMousePos )`
(`:320`) before `m_toolMgr->ProcessEvent( *evt )` (`:323`).

**Wheel gating is subtle** (`:641-652`): wheel events with **zero or one** modifier are
reserved for `VIEW_CONTROLS` (pan/zoom) and are **not** turned into `TOOL_EVENT`s.
`std::popcount( mods & MD_MODIFIER_MASK ) > 1` is the exact test.

**Keyboard** (`GetToolEvent`, `:422-498`):

| Condition | Emitted |
|---|---|
| key is `WXK_ESCAPE` | `TOOL_EVENT( TC_COMMAND, TA_CANCEL_TOOL, WXK_ESCAPE )` — `:494` |
| otherwise | `TOOL_EVENT( TC_KEYBOARD, TA_KEY_PRESSED, key \| mods )` — `:496` |

Then, if not a cancel, `evt->SetMousePosition( m_lastMousePos )` and
`evt->SetHasPosition( true )` (`:710-711`) — **keyboard events carry the cursor position**,
which every hotkey-driven placement tool relies on.

**Deferred-click flush** (`flushPendingClicks`, `:501-521`): when Escape arrives, any button
that is `pressed` but whose polled OS state is up gets a synthetic
`TOOL_EVENT( TC_MOUSE, TA_MOUSE_CLICK, st->button )` at `st->downPosition` **before** the
cancel is processed. A gpui host with ordered event delivery does not need this, but must not
break the invariant it protects: **a click must be delivered before the Escape that follows it.**

### 6.4 Button and modifier bit values — reproduce exactly

`include/tool/tool_event.h:128-145`:

```c
BUT_LEFT   = 0x1     BUT_RIGHT  = 0x2     BUT_MIDDLE = 0x4
BUT_AUX1   = 0x8     BUT_AUX2   = 0x10
BUT_BUTTON_MASK = 0x1F

MD_SHIFT   = 0x1000  MD_CTRL    = 0x2000  MD_ALT     = 0x4000
MD_SUPER, MD_META  (see :142-143)          MD_ALTGR   = 0x20000
MD_MODIFIER_MASK = MD_SHIFT|MD_CTRL|MD_ALT|MD_SUPER|MD_META|MD_ALTGR
```

`TOOL_EVENT` splits its `aExtraParam` with `m_keyCode = param & ~MD_MODIFIER_MASK` and
`m_modifiers = param & MD_MODIFIER_MASK` (`tool_event.h:209,218`), and asserts
`(aMods & ~MD_MODIFIER_MASK) == 0` at `:574`.

`decodeModifiers` (`tool_dispatcher.cpp:159-190`) note: **AltGr is Ctrl+Alt on wx**, so when
`CAN_USE_ALTGR_KEY` it maps to `MD_ALTGR` *instead of* `MD_CTRL|MD_ALT`, else it falls through
to setting both. A gpui host should decide this explicitly rather than inherit the quirk.

Five `BUTTON_STATE` objects are created in the constructor (`:137-148`), one per button, each
holding `dragging`, `pressed`, `dragOrigin` (world), `dragOriginScreen` (screen),
`downPosition` (world), `downTimestamp`.

### 6.5 Drag thresholds and key quirks a Rust dispatcher must keep

Two `static` pure functions are deliberately exposed on the class **specifically so they can be
tested and reused without wx** — treat them as the portable core:

- `static bool IsPastDragThreshold( const VECTOR2D& aOffset, int aDragMinX, int aDragMinY )`
  — `tool_dispatcher.h:88`, impl `:213-216`: `abs(offset.x) > dragMinX || abs(offset.y) > dragMinY`.
- `static bool ShouldDropAutoRepeat( int aKeyCode, wxLongLong aNowMs, bool aKeyIsDown,
  int& aLastKey, wxLongLong& aLastTimeMs )` — `tool_dispatcher.h:76`, with
  `static const int AutoRepeatWindowMs = 250` (`:53`).

Other constants: `DragTimeThreshold = 300` ms (`:112`, **macOS only** — guarded by
`#ifdef __WXMAC__` at `:298`), `DragDistanceThreshold = 8` px (`:117`).

**Quirks worth keeping:**

- **Fast click-drag vs double-click** (`:232-247`): if the cursor moved past the drag threshold
  between the two clicks, the double-click is **demoted to a fresh mouse-down**, and
  `st->pressed`/`st->dragging` are reset. Without this, a quick drag registers as a double-click.
- **Missing button-up recovery** (`:253-258`): if `st->pressed` but the polled OS state says the
  button is up, synthesise an up. Deliberately *not* applied symmetrically to down events
  because "it kills touchpad tapping".

**Quirks to drop:** the `__APPLE__` `kVK_ANSI_*` raw-keycode remapping (`:468-488`), the
Ctrl+letter→1..26 remap (`:445-465`), `translateSpecialCode` numpad folding (`:405-419`),
the `wxEVT_CHAR_HOOK` + Shift skip for issue #1809 (`:659-663`), and
`ForceTimerMessagesToBeCreatedIfNecessary`. These exist only because of wx's key-event model.

**Quirk that is NOT droppable:** the hotkey vocabulary is `WXK_*` integers.
`TOOL_ACTION::GetDefaultHotKey()` returns an `int` built from `WXK_*` values OR'd with `MD_*`
(§8). A Rust dispatcher must **map gpui keys to the same `WXK_*` numbers**, or every stored
hotkey and every `.DefaultHotkey(...)` in `actions.cpp` / `sch_actions.cpp` breaks. `WXK_*`
values are in `wx/defs.h`; they are stable ABI and can be transcribed into a Rust constant
table once.

### 6.6 `VIEW_CONTROLS` — already abstract, already the right seam

`class VIEW_CONTROLS` (`include/view/view_controls.h`) is an **abstract base**;
`class WX_VIEW_CONTROLS : public VIEW_CONTROLS, public wxEvtHandler`
(`include/view/wx_view_controls.h:47`) is merely one implementation. A `GPUI_VIEW_CONTROLS`
is a first-class, already-anticipated extension point. This is the easiest part of the whole
migration.

Pure virtuals to implement (`view_controls.h`):

| Line | Signature |
|---|---|
| 212 | `virtual void PinCursorInsideNonAutoscrollArea( bool aWarpMouseCursor ) = 0` |
| 226 | `virtual VECTOR2D GetMousePosition( bool aWorldCoordinates = true ) const = 0` |
| 249 | `virtual VECTOR2D GetRawCursorPosition( bool aSnappingEnabled = true ) const = 0` |
| 262 | `virtual VECTOR2D GetCursorPosition( bool aEnableSnapping ) const = 0` |
| 289 | `virtual void SetCursorPosition( const VECTOR2D&, bool aWarpView, bool aTriggeredByArrows, long aArrowCommand ) = 0` |
| 301 | `virtual void SetCrossHairCursorPosition( const VECTOR2D&, bool aWarpView ) = 0` |
| 340 | `virtual void WarpMouseCursor( const VECTOR2D&, bool aWorldCoordinates, bool aWarpView ) = 0` |
| 365 | `virtual void CenterOnCursor() = 0` |

Virtuals with usable base implementations (override as needed): `SetGrabMouse` (`:156`),
`SetAutoPan` (`:167`), `EnableAutoPan` (`:177`), `SetAutoPanSpeed` (`:187`),
`SetAutoPanAcceleration` (`:197`), `SetAutoPanMargin` (`:207`), `ForceCursorPosition` (`:272`),
`ShowCursor` (`:308`), `CaptureCursor` (`:322`), `Reset` (`:370`), `LoadSettings` (`:382`).

`WarpMouseCursor` is the one that genuinely needs a platform capability (moving the OS
pointer). gpui may not expose it; if not, `ForceCursorPosition` plus a drawn crosshair covers
most uses, and `CaptureCursor`/autopan behaviour degrades gracefully.

### 6.7 Verdict on the input seam

**Low risk.** `VIEW_CONTROLS` is already abstract. `TOOL_DISPATCHER` must be rewritten, but
its *output* contract is only ~10 distinct `TOOL_EVENT` constructions (§6.3) and two pure
static helpers are already carved out for reuse. The one genuine constraint is that the Rust
side must speak `WXK_*` key codes so the hotkey registry keeps working.

## 7. The host seam: what `SCH_EDIT_FRAME` must be replaced with

This is the largest and riskiest part of the cut.

### 7.1 The frame inheritance chain, and where wx actually enters

```
wxFrame
  ^
EDA_BASE_FRAME   include/eda_base_frame.h:115
    : public wxFrame, public TOOLS_HOLDER, public KIWAY_HOLDER, ...
    OWNS: UNDO_REDO_CONTAINER m_undoList (:856), m_redoList (:857)
  ^
KIWAY_PLAYER
  ^
EDA_DRAW_FRAME   include/eda_draw_frame.h:81
    OWNS: EDA_DRAW_PANEL_GAL* m_canvas (:617), GAL_TYPE m_canvasType (:610),
          BASE_SCREEN* m_currentScreen (:199)
    IMPLEMENTS: wxWindow* GetToolCanvas() const override { return GetCanvas(); }  (:458)
  ^
SCH_BASE_FRAME
    OWNS: SCHEMATIC_SETTINGS m_base_frame_defaults (:321),
          DIALOG_SCH_FIND* m_findReplaceDialog (:318),
          PANEL_SCH_SELECTION_FILTER* m_selectionFilterPanel (:317),
          wxFileSystemWatcher m_watcher (:326) + debounce timer (:330),
          SPNAV_2D_PLUGIN / NL_SCHEMATIC_PLUGIN m_spaceMouse (:334/:336)
  ^
SCH_EDIT_FRAME   eeschema/sch_edit_frame.h:138   (202 public methods)
    OWNS: SCHEMATIC* m_schematic (:1130)
          SCH_SHEET_PATH m_sheetPath (:130)
          SCH_CONNECTIVITY::SUBSCRIPTION m_connectivitySubscription (:1129)
          highlighted-net strings (:1131-1132)
          std::vector<std::unique_ptr<SCH_ITEM>> m_items_to_repeat (:1135)
          API_HANDLER_SCH / _COMMON / _LIBRARIES (:1175-1177)
          ~12 dialog pointers, a hierarchy pane, a net navigator tree, panes, timers
```

**The one structural blocker in the tool framework:**

```cpp
virtual wxWindow* GetToolCanvas() const = 0;   // include/tool/tools_holder.h:165
```

Pure virtual on `TOOLS_HOLDER`, which `TOOL_MANAGER` and `TOOL_DISPATCHER` both call. A non-wx
host must either return a dummy `wxWindow` or this signature must be changed. Changing it is a
tree-wide edit (every frame in every KiCad program implements it) but is mechanically simple
and is the right fix — it is the only wx type in the `TOOLS_HOLDER` contract.

**Everything else in the tool framework is already clean:**

| Header | `wx` occurrences | Verdict |
|---|---|---|
| `include/tool/tool_manager.h` | **0** | fully portable |
| `include/tool/action_manager.h` | 1 | fully portable |
| `include/tool/tool_base.h` | 4 | portable |
| `include/tool/tool_event.h` | 5 | portable |
| `include/tool/tools_holder.h` | 1 (`GetToolCanvas`) + `wxString` in `DisplayToolMsg` | **one pure virtual to fix** |
| `include/tool/tool_action.h` | `wxString` labels + `BITMAPS` icon enum | data only, see §8 |
| `include/tool/tool_dispatcher.h` | `wxEvtHandler` base | **rewrite (§6)** |
| `include/tool/action_menu.h:42` | `class ACTION_MENU : public wxMenu` | **rewrite** — context menus |

`TOOL_INTERACTIVE` (`include/tool/tool_interactive.h`) touches wx only through
`SetContextMenu( ACTION_MENU*, CONTEXT_MENU_TRIGGER )` (`:81`). Its coroutine machinery
(`include/tool/coroutine.h:40`, `#include <libcontext.h>`) is wx-free and works unchanged in
any host — it is the thing that lets `SCH_DRAWING_TOOLS::PlaceSymbol` block on
`Wait()` mid-placement.

### 7.2 `TOOLS_HOLDER` — the interface a Rust host must satisfy

`include/tool/tools_holder.h`. This, not `SCH_EDIT_FRAME`, is the real contract
`TOOL_MANAGER` needs:

| Line | Member | Notes for a non-wx host |
|---|---|---|
| 51 | `TOOL_MANAGER* GetToolManager() const` | non-virtual, `m_toolManager` at `:171` |
| 62 | `virtual void RegisterUIUpdateHandler( const TOOL_ACTION&, const ACTION_CONDITIONS& )` | drives greying-out of menus/toolbars |
| 70 | `virtual void RegisterUIUpdateHandler( int aID, const ACTION_CONDITIONS& )` | id-keyed variant |
| 78/85 | `virtual void UnregisterUIUpdateHandler(...)` | |
| 93 | `virtual SELECTION& GetCurrentSelection()` | **must be overridden** — eeschema returns the selection tool's selection |
| 111 | `virtual void PushTool( const TOOL_EVENT& )` | the tool stack (status-bar hint + restore-on-cancel) |
| 118 | `virtual void PopTool( const TOOL_EVENT& )` | |
| 125 | `virtual void DisplayToolMsg( const wxString& )` | status text |
| 130 | `virtual void SelectToolbarAction( const TOOL_ACTION& )` | toolbar radio state |
| 132 | `virtual void ShowChangedLanguage()` | |
| 160 | `virtual void CommonSettingsChanged( int aFlags = 0 )` | |
| **165** | **`virtual wxWindow* GetToolCanvas() const = 0`** | **the blocker** |
| 166 | `virtual void RefreshCanvas()` | request a repaint |
| 168 | `virtual wxString ConfigBaseName()` | |
| 172 | `ACTIONS* m_actions` | |

`TOOL_HOLDER_LOCK`-style RAII at `:202-207` calls `PushTool`/`PopTool`.

### 7.3 What the tools actually ask the frame for

Measured across `eeschema/tools/*.cpp`. Total `m_frame->` call sites, by file:

| Tool | `m_frame->` sites | file lines |
|---|---|---|
| `sch_editor_control.cpp` | 180 | 3,868 |
| `sch_edit_tool.cpp` | 164 | 4,159 |
| `sch_drawing_tools.cpp` | 134 | 3,517 |
| `sch_selection_tool.cpp` | 59 | 4,402 |
| `sch_move_tool.cpp` | 57 | 3,024 |
| `sch_line_wire_bus_tool.cpp` | 46 | 1,550 |
| `sch_point_editor.cpp` | 14 | 1,800 |

**The frame API surface the tools actually use is small and heavily skewed.** Top calls across
all of `eeschema/tools/`:

| Count | Call | Category |
|---|---|---|
| 172 | `m_frame->GetScreen()` | **model** — trivially re-providable |
| 101 | `m_frame->GetCanvas()` | **canvas** — returns `EDA_DRAW_PANEL_GAL*`; needs an abstraction |
| 96 | `m_frame->Schematic()` | **model** |
| 61 | `m_frame->GetCurrentSheet()` | **model** |
| 54 | `m_frame->eeconfig()` | **settings** — `EESCHEMA_SETTINGS*` from the kiface |
| 28 | `m_frame->ShowInfoBarError()` | **UI feedback** → Rust toast |
| 25 | `m_frame->Prj()` | **project** |
| 25 | `m_frame->GetCurSymbol()` | symbol editor only |
| 23 | `m_frame->UpdateItem()` | **view invalidation** |
| 22 | `m_frame->OnModify()` | **dirty flag** |
| 18 | `m_frame->AddToScreen()` | **model mutation** |
| 15 | `m_frame->ShowInfoBarMsg()` | UI feedback |
| 13 | `m_frame->Kiway()` | cross-frame IPC (open symbol editor, cross-probe to pcbnew) |
| 13 | `m_frame->GetToolManager()` | |
| 11 | `m_frame->SetMsgPanel()` | status bar |
| 11 | `m_frame->IsType()` | frame-type test |
| 11 | `m_frame->GetNearestHalfGridPosition()` | **grid** |
| 11 | `m_frame->GetInfoBar()` | **returns `WX_INFOBAR*`** — needs an abstraction |
| 10 | `m_frame->GetRenderSettings()` | |
| 9 | `m_frame->RemoveFromScreen()` | model mutation |
| 8 | `m_frame->SynchronizePins()`, `SaveCopyForRepeatItem()`, `RebuildView()` | |
| 7 | `m_frame->RecalculateConnections()` | **connectivity** |
| 5 | `m_frame->AnnotateSymbols()`, `GetRunMenuCommandDescription()` | |
| 4 | `m_frame->UpdateHierarchyNavigator()`, `SyncView()`, `ToolStackIsEmpty()` | |

**Classification for bridging:**

- **Model / project / settings** (`GetScreen`, `Schematic`, `GetCurrentSheet`, `Prj`,
  `eeconfig`, `GetRenderSettings`, `AddToScreen`, `RemoveFromScreen`, `OnModify`,
  `SaveCopyForRepeatItem`, `RecalculateConnections`, `AnnotateSymbols`, `SynchronizePins`)
  ≈ **470 of ~600 sites.** These have **nothing to do with wx**. Move them onto a plain
  `SCH_HOST` class that a non-wx host implements. This is the bulk of the work but it is
  mechanical.
- **View / canvas** (`GetCanvas`, `UpdateItem`, `RebuildView`, `SyncView`,
  `GetNearestHalfGridPosition`) ≈ 145 sites. `GetCanvas()` returns
  `EDA_DRAW_PANEL_GAL*`; tools mostly use it for `GetView()`, `GetViewControls()`,
  `GetGAL()`, `Refresh()`, `SetCurrentCursor()`. Introduce a narrow `SCH_CANVAS` interface
  exposing exactly those five and reroute.
- **UI feedback** (`ShowInfoBarError`, `ShowInfoBarMsg`, `GetInfoBar`, `SetMsgPanel`,
  `DisplayToolMsg`) ≈ 65 sites. Pure notification — route to gpui toasts/status bar. Easy.
- **Cross-frame** (`Kiway`) 13 sites. Opening the symbol editor, cross-probing to pcbnew.
  Stub for v1.

### 7.4 Dialogs invoked from the tools — the hard bridging cases

~70 dialog invocations plus ~40 blocking `wxMessageBox` / `IsOK()` / `DisplayErrorMessage`
prompts. Counts of blocking prompts per file:

```
symbol_editor_control.cpp 11   sch_edit_tool.cpp        6
simulator_control.cpp      6   sch_editor_control.cpp   6
sch_drawing_tools.cpp      4   sch_edit_table_tool.cpp  2
sch_inspection_tool.cpp    2   assign_footprints.cpp    1
backannotate.cpp           1   ee_graphic_tool.cpp      1
```

Dialog classes reached from `eeschema/tools/` (occurrence counts):

```
DIALOG_CHANGE_SYMBOLS 10   DIALOG_KICAD_DIFF 8   DIALOG_TEXT_PROPERTIES 6   DIALOG_ERC 6
DIALOG_TABLE_PROPERTIES 4  DIALOG_SYNC_SHEET_PINS 3  DIALOG_SHAPE_PROPERTIES 3
DIALOG_LIB_SYMBOL_PROPERTIES 3  DIALOG_LABEL_PROPERTIES 3  DIALOG_WIRE_BUS_PROPERTIES 2
DIALOG_TABLECELL_PROPERTIES 2  DIALOG_SYMBOL_FIELDS_TABLE 2  DIALOG_JUNCTION_PROPS 2
DIALOG_FIELD_PROPERTIES 2  DIALOG_CREATE_NET_CHAIN 2  DIALOG_SHIM 2
+ 1 each: DIALOG_USER_DEFINED_SIGNALS, DIALOG_UPDATE_SYMBOL_FIELDS, DIALOG_UPDATE_FROM_PCB,
          DIALOG_SYMBOL_REMAP, DIALOG_SYMBOL_PROPERTIES, DIALOG_STYLE, DIALOG_SIM_COMMAND,
          DIALOG_SHEET_PIN_PROPERTIES, DIALOG_PRINT
```

**Why these are hard:** they are **modal and synchronous**. A tool coroutine does
`DIALOG_LABEL_PROPERTIES dlg( m_frame, label ); if( dlg.ShowModal() == wxID_OK ) { ... }` and
resumes inline. A gpui host is async. Three options, in order of preference:

1. **Keep them, for now.** `eeschema_kiface_objects` links wx anyway (§2.1). If a real
   `wxApp` is running on a secondary thread (or the Rust host pumps the wx event loop),
   `ShowModal()` still works. Ugly, but it means **v1 need not rewrite 124 dialogs**.
2. **Async bridge.** Replace `ShowModal()` call sites with a `HOST::RequestModal(descriptor)`
   that suspends the tool coroutine (`COROUTINE::Yield`) and resumes when Rust posts the
   result. The coroutine machinery already supports suspension — this is the *right* answer
   and it is feasible precisely because tools are coroutines.
3. **Rewrite each dialog in gpui.** Correct end state, enormous. 124 dialog source files.

**Recommendation:** option 1 for bring-up, option 2 as the migration path, option 3
opportunistically for the ~15 high-traffic property dialogs.

### 7.5 What a non-wx host (`SCH_HOST`) must re-provide

Enumerated, with separability:

| Responsibility | Currently | Separable? |
|---|---|---|
| Own the `SCHEMATIC` | `SCH_EDIT_FRAME::m_schematic` (`:1130`) | **yes, trivially** — it is already a heap pointer with `Schematic()` accessor (`:148`) |
| Current sheet path | `m_sheetPath` (`:130`), `GetCurrentSheet()`/`SetCurrentSheet()` (`:451,:453`) | **yes** |
| Current `SCH_SCREEN` | `GetScreen()` override (`:144`), backed by `EDA_DRAW_FRAME::m_currentScreen` | **yes** |
| **Undo / redo stacks** | `UNDO_REDO_CONTAINER m_undoList`, `m_redoList` — **members of `EDA_BASE_FRAME`** (`eda_base_frame.h:856-857`), with `PushCommandToUndoList` (`:588`), `GetUndoCommandCount`/`GetRedoCommandCount` (`:607-608`), and eeschema's `SaveCopyInUndoList` (`sch_edit_frame.h:721,:731`) + `eeschema/schematic_undo_redo.cpp` | **yes, but it is a real lift** — the containers are `PICKED_ITEMS_LIST` based and wx-free, but they live on a `wxFrame` subclass. Hoist into `SCH_HOST`. |
| `SCH_COMMIT` transactions | `eeschema/sch_commit.{h,cpp}`; `SCH_COMMIT::pushSchEdit` drives connectivity recalculation | **already separable** — `SCH_COMMIT` takes a `TOOLS_HOLDER*`/`SCH_EDIT_FRAME*`, not a window |
| Settings | `eeconfig()` → `EESCHEMA_SETTINGS*` from `Kiface().KifaceSettings()`; `SCHEMATIC_SETTINGS m_base_frame_defaults` (`sch_base_frame.h:321`) | **yes** — `EESCHEMA_SETTINGS` is a JSON-backed `APP_SETTINGS_BASE`, wx-free apart from `wxString` |
| The `VIEW` + `GAL` + `PAINTER` + `VIEW_CONTROLS` quartet | `EDA_DRAW_PANEL_GAL` (`class_draw_panel_gal.h:343,346,349,352`) | **replaced wholesale** (§5, §6) |
| `TOOL_MANAGER` + `ACTION_MANAGER` | `EDA_BASE_FRAME`/`TOOLS_HOLDER::m_toolManager` | **yes** — `tool_manager.h` has zero wx |
| Tool stack (`PushTool`/`PopTool`/`ToolStackIsEmpty`) | `TOOLS_HOLDER` (`:111,:118`) | **yes** |
| Info bar / status bar / message panel | `WX_INFOBAR*`, `SetMsgPanel`, `DisplayToolMsg` | **yes** — narrow, notification-only |
| Modal dialogs | 124 wxFormBuilder dialogs | **no** — see §7.4 |
| Hierarchy navigator pane | `HIERARCHY_PANE* m_hierarchy` (`:1144`) | rewrite in gpui |
| Net navigator tree | `wxGenericTreeCtrl* m_netNavigator` (`:1149`) + filter (`:1150`) | rewrite in gpui |
| Selection-filter panel | `PANEL_SCH_SELECTION_FILTER*` (`sch_base_frame.h:317`) | rewrite in gpui |
| Design-block / remote-symbol panes | `SCH_DESIGN_BLOCK_PANE*` (`:1170`), `PANEL_REMOTE_SYMBOL*` (`:1171`) | rewrite or drop for v1 |
| File-change watcher | `std::unique_ptr<wxFileSystemWatcher> m_watcher` (`sch_base_frame.h:326`) + debounce timer | **replace** with `notify` crate |
| Cross-probe flash timer | `wxTimer m_crossProbeFlashTimer` (`:1160`) | replace with a gpui timer |
| SpaceMouse / 3Dconnexion | `SPNAV_2D_PLUGIN` / `NL_SCHEMATIC_PLUGIN` (`sch_base_frame.h:334/:336`) | drop for v1 |
| KiCad IPC API handlers | `API_HANDLER_SCH/_COMMON/_LIBRARIES` (`:1175-1177`) | **yes** — nng-based, wx-free |
| `KIWAY` cross-frame messaging | `KIWAY_HOLDER` base | keep or stub |
| Page-setup / print | `wxPageSetupDialogData m_pageSetupData` (`:1134`), `DIALOG_PRINT` | drop for v1; plotting is a separate `PLOTTER` path |

### 7.6 Verdict on the host seam

**High effort, low conceptual risk, one true blocker.**

> **This verdict was wrong about which blocker, and the record is kept as written
> rather than edited.** `GetToolCanvas()` is largely a red herring: returning
> `nullptr` from it is already a production state in three implementations, and
> `TOOL_DISPATCHER` null-checks it. The real hazard was sixteen unchecked
> downcasts of `TOOL_MANAGER::GetToolHolder()` plus `TOOL_BASE::getEditFrame<T>()`,
> which is how every tool's `m_frame` is set — undefined behaviour for any
> non-frame host, and silent. That is fixed; see `05-porting-guide.md` §4.8 and
> `06-what-is-missing.md` Stage 3. The second bullet below — the ~600 `m_frame->`
> sites — is what the real blocker turns out to be, so the survey named it; it
> just ranked it second.

- The blocker is `TOOLS_HOLDER::GetToolCanvas() const = 0` returning `wxWindow*`. Fix it
  upstream-style (introduce an opaque `TOOL_CANVAS*` or drop the method) before anything else.
- ~470 of the ~600 `m_frame->` call sites are model/settings access with no wx involvement;
  moving them to a `SCH_HOST` base that `SCH_EDIT_FRAME` also implements is a large but purely
  mechanical refactor that can land in the C++ tree **independently of any Rust work**, and is
  the highest-value preparatory commit.
- Undo/redo living on `EDA_BASE_FRAME` is the one piece of genuine architecture debt in the
  way. It is wx-free data on a wx class; hoisting it is straightforward.
- Modal dialogs are the long tail. Do not attempt them in v1.

## 8. The action / hotkey / icon registry

**Short answer: yes, the Rust UI can enumerate every action at runtime and build menus,
toolbars and a hotkey editor from the result, with no wx involved — except that the
label/tooltip strings are `wxString` and the icon is an enum that must be resolved to a file.**

### 8.1 How actions are declared

`TOOL_ACTION` (`include/tool/tool_action.h:299`) is a **singleton per action**, declared as a
static data member and defined at namespace scope with a fluent builder. Real example
(`eeschema/tools/sch_actions.cpp:413`):

```cpp
TOOL_ACTION SCH_ACTIONS::placeSymbolPin( TOOL_ACTION_ARGS()
        .Name( "eeschema.SymbolDrawing.placeSymbolPin" )
        .Scope( AS_GLOBAL )
        .DefaultHotkey( 'P' )
        .LegacyHotkeyName( "Create Pin" )
        .FriendlyName( _( "Draw Pins" ) )
        .ToolbarState( TOOLBAR_STATE::TOGGLE )
        .Icon( BITMAPS::pin )
        .Flags( AF_ACTIVATE )
        .Parameter( SCH_PIN_T ) );
```

Full builder surface, `TOOL_ACTION_ARGS`, `include/tool/tool_action.h`:

| Line | Builder |
|---|---|
| 123 | `.Name( std::string_view )` — must be `[appName.]toolName.actionName`, dotted; uniqueness asserted (`common/tool/action_manager.cpp:76-80`) |
| 129 | `.FriendlyName( std::string_view )` |
| 138 | `.Scope( TOOL_ACTION_SCOPE )` — `AS_CONTEXT` / `AS_ACTIVE` / `AS_GLOBAL` |
| 147 | `.DefaultHotkey( int )` — a `WXK_*` code or ASCII, OR'd with `MD_*` |
| 156 | `.DefaultHotkeyAlt( int )` |
| 167 | `.LegacyHotkeyName( std::string_view )` — for migrating pre-6.0 configs |
| 176 | `.MenuText( std::string_view )` |
| 185 | `.Tooltip( std::string_view )` |
| 194 | `.Description( std::string_view )` |
| 203 | `.Icon( BITMAPS )` |
| 212 | `.Flags( TOOL_ACTION_FLAGS )` — `AF_NONE` / `AF_ACTIVATE` / `AF_NOTIFY` |
| 222 | `.Parameter<T>( T )` — stored as `ki::any` |
| 231 | `.UIId( int )` |
| 237 | `.Group( const TOOL_ACTION_GROUP& )` — radio grouping (`tool_action.h:74`) |
| 243/250 | `.ToolbarState( TOOLBAR_STATE )` / `( std::initializer_list<TOOLBAR_STATE> )` |

Counts in this tree: **191** `TOOL_ACTION` members in `include/tool/actions.h` (the shared
`ACTIONS` set) and **254** in `eeschema/tools/sch_actions.h` (`SCH_ACTIONS`). So roughly
**445 actions** are relevant to a schematic UI.

### 8.2 Runtime enumeration — two registries

**(a) The global list, populated by static initialisation.** `TOOL_ACTION`'s constructor does
`ACTION_MANAGER::GetActionList().push_back( this )` (`common/tool/tool_action.cpp:52` and
`:110`), and the destructor removes it (`:116`). The list itself is a function-local static:

```cpp
static std::list<TOOL_ACTION*>& GetActionList()   // include/tool/action_manager.h:194
{
    static std::list<TOOL_ACTION*> actionList;
    return actionList;
}
```

**This is callable before any frame, any window, or any `wxApp` exists.** It is how a
headless host discovers actions.

**(b) The per-manager index.** `ACTION_MANAGER`'s constructor walks `GetActionList()` and calls
`RegisterAction()` on each (`common/tool/action_manager.cpp:62`), building:

```cpp
const std::map<std::string, TOOL_ACTION*>& GetActions() const;  // action_manager.h:139, impl :313
TOOL_ACTION* FindAction( const std::string& aActionName ) const; // :152
std::map<std::string, TOOL_ACTION*> m_actionNameIndex;           // :228
std::map<int, TOOL_ACTION*>         m_customUIIdIndex;           // :231
std::map<int, int>                  m_hotkeys;                   // :238  actionId -> keycode
std::map<int, ACTION_CONDITIONS>    m_uiConditions;              // :242  enable/check state
```

### 8.3 What the Rust UI can read off each action

All of this is available from a `TOOL_ACTION*` with no window:

| Getter | Line | Type | Use |
|---|---|---|---|
| `GetName()` | 320 | `const std::string&` | **stable id** — dotted, ASCII. Use this as the Rust-side key, not the numeric id. |
| `GetId()` | 328 | `int` | runtime-assigned, **not stable across builds** |
| `GetUIId()` | 330 | `int` | `m_uiid.value_or( m_id + ACTION_BASE_UI_ID )` |
| `GetFriendlyName()` | 403 | `wxString` | **the label for menus and toolbars** |
| `GetMenuLabel()` | 394 | `wxString` | |
| `GetMenuItem()` | 395 | `wxString` | label + accelerator |
| `GetTooltip( bool aIncludeHotkey = true )` | 396 | `wxString` | |
| `GetButtonTooltip()` | 397 | `wxString` | |
| `GetDescription()` | 398 | `wxString` | |
| `GetIcon()` | ~445 | `BITMAPS` | enum; see §8.5 |
| `GetDefaultHotKey()` / `GetDefaultHotKeyAlt()` | 321/322 | `int` | `WXK_*` \| `MD_*` |
| `GetHotKey()` / `GetHotKeyAlt()` | 323/324 | `int` | current, after user config |
| `SetHotKey( int, int )` | 326 | | rebinding |
| `IsHotKeyUserBound( int )` | 327 | `bool` | |
| `GetScope()` | 404 | `TOOL_ACTION_SCOPE` | |
| `GetActionGroup()` | ~430 | `std::optional<TOOL_ACTION_GROUP>` | radio grouping |
| `GetToolName()` | ~432 | `std::string` | the owning tool, derived from the dotted name |
| `IsActivation()` / `IsNotification()` | ~435/~441 | `bool` | `AF_ACTIVATE` / `AF_NOTIFY` |
| `MakeEvent()` | 393 | `TOOL_EVENT` | **how the Rust UI fires the action**: build it and hand it to `TOOL_MANAGER::ProcessEvent` |
| `GetParam<T>()` | 406 | `T` | `ki::any_cast`; asserts on mismatch |

**`wxString` at this boundary is not a problem** — `wxString::utf8_str()` / `ToUTF8()` give
clean UTF-8 for the FFI. It does mean translation still happens on the C++ side, via `_()`
at declaration, which is arguably the right place for it.

**Enable / check state** comes from `ACTION_CONDITIONS` in `m_uiConditions`
(`action_manager.h:242`), registered through
`TOOLS_HOLDER::RegisterUIUpdateHandler( const TOOL_ACTION&, const ACTION_CONDITIONS& )`
(`tools_holder.h:62`). A Rust host implements that method and evaluates the conditions per
frame to grey out / tick menu items. This is the mechanism; it is not wx-specific.

### 8.4 Hotkeys — persistence and the `WXK_*` constraint

- Defaults live in the `.DefaultHotkey(...)` calls.
- User overrides are read/written by `ReadHotKeyConfig( const wxString& aFileName,
  std::map<std::string, std::pair<int,int>>& )` (`include/hotkeys_basic.h:103`),
  `ReadHotKeyConfigIntoActions( const wxString&, std::vector<TOOL_ACTION*>& )` (`:111`) and
  `WriteHotKeyConfig( const std::vector<TOOL_ACTION*>& )` (`:116`). Keyed by the action's
  **dotted string name**, so the file survives id churn.
- Legacy migration: `ReadLegacyHotkeyConfigFile` / `ReadLegacyHotkeyConfig` (`:125`, `:133`),
  matched by `.LegacyHotkeyName(...)`.
- Display/parse: `KeyNameFromKeyCode( int, bool* )` (`:64`) and
  `KeyCodeFromKeyName( const wxString& )` (`:53`) — a Rust hotkey editor can call these
  rather than reimplement the naming.
- `HOTKEY_STORE` (`common/hotkey_store.cpp`) is the editor-facing model:
  `Init( std::vector<TOOL_ACTION*>, bool aIncludeReadOnlyCmds )` (`:94`),
  `GetSections()` (`:163`), `SaveAllHotkeys()` (`:169`),
  `ResetAllHotkeysToDefault()`/`ResetAllHotkeysToOriginal()` (`:182`,`:195`),
  `CheckKeyConflicts( TOOL_ACTION*, long aKey, HOTKEY** )` (`:208`).

> **Constraint, repeated from §6.5 because it is the one that will bite:** hotkey integers
> are `WXK_*` values. A gpui key event must be mapped to the same numbers or every default
> and every saved binding breaks. Transcribe `WXK_*` from `wx/defs.h` into a Rust table once
> and test it against `KeyNameFromKeyCode`.

### 8.5 Icons — what they actually are, and how Rust gets them

`TOOL_ACTION::GetIcon()` returns a value of `enum class BITMAPS : unsigned int`
(`include/bitmaps/bitmaps_list.h:28`), with `INVALID_BITMAP = 0` reserved so the enum can be
forward-declared and zero-initialised. There are **~726 enumerators**.

**The source of truth is SVG.** `resources/bitmaps_png/sources/` contains **1,431 `.svg`
files**, organised as:

```
resources/bitmaps_png/sources/light/<name>.svg
resources/bitmaps_png/sources/dark/<name>.svg
resources/bitmaps_png/sources/constraints/...
resources/bitmaps_png/CREDITS
```

**The enumerator name *is* the SVG basename.** From `resources/bitmaps_png/CMakeLists.txt:940-970`:

```cmake
set( svgFile "${CMAKE_CURRENT_SOURCE_DIR}/sources/${theme}/${bmn}${heightTag}.svg" )
if ( NOT EXISTS ${svgFile} )
    set( svgFile "${CMAKE_CURRENT_SOURCE_DIR}/sources/${theme}/${bmn}.svg" )
endif()
set( pngFile "${bmn}${themeTag}${heightTag}.png" )
...
set( bitmapInfo "aBitmapInfoCache[BITMAPS::${bmn}].emplace_back( BITMAPS::${bmn}, wxT( \"${pngFile}\" ), ${pngHeight}, wxT( \"${theme}\" ) );\n" )
```

So `BITMAPS::pin` ⇒ `sources/light/pin.svg` and `sources/dark/pin.svg`, optionally with a
`_<height>` suffix for hand-tuned sizes.

**The build-time pipeline** rasterises each SVG at several heights into PNGs, packs them into
`images.tar.gz` (`common/bitmap_store.cpp:95`, `static const wxString IMAGE_ARCHIVE =
wxT( "images.tar.gz" )`), installed at
`PATHS::GetStockDataPath() + "/resources/images.tar.gz"` (`:110`). `BITMAP_STORE` reads that
archive through wx's virtual filesystem and caches `wxBitmap`s, with light/dark theme
selection at `:354-371` (`ICON_THEME::LIGHT|DARK|AUTO`, `AUTO` deferring to
`KIPLATFORM::UI::IsDarkTheme()`).

The generated lookup table is `common/bitmap_info.cpp` (**committed**, generated from
`include/bitmaps/bitmap_info.cpp.in` when `MAINTAIN_PNGS=ON`). Entries look like:

```cpp
aBitmapInfoCache[BITMAPS::e_24].emplace_back( BITMAPS::e_24, wxT( "e_24_16.png" ), 16, wxT( "light" ) );
```

`BITMAP_INFO` (`include/bitmaps/bitmap_info.h:32`) is `{ BITMAPS id; int16_t height;
THEME theme; std::string filename; }` where `THEME ∈ {LIGHT, DARK, NO_THEME}`.

**Recommendation for the Rust UI — load the SVGs directly.**

1. Export a tiny FFI that returns `BITMAPS` as its **enumerator name string** (a
   `switch`-generated table, or reuse `BuildBitmapInfo` and strip the `_<theme>_<h>.png`
   suffix).
2. Resolve `resources/bitmaps_png/sources/{light|dark}/<name>.svg`.
3. Render with `resvg`/`usvg` at the exact device pixel ratio.

This is strictly better than the PNG archive: vector, correct at any HiDPI factor, no
`images.tar.gz` dependency, no wx VFS, and it keeps KiCad's own light/dark split. Fall back to
the tar.gz PNGs only for the handful of icons that have a hand-tuned `_<height>` variant.

### 8.6 Menus and toolbars — the layout is already data

Toolbar layout is **not** hard-coded in wx; it is a serialisable configuration object.

- `TOOLBAR_CONFIGURATION` / `TOOLBAR_ITEM` / `TOOLBAR_ITEM_TYPE` /
  `TOOLBAR_GROUP_CONFIG` / `TOOLBAR_ITEM_REF` — `include/tool/ui/toolbar_configuration.h:33-131`.
  `TOOLBAR_ITEM_TYPE` covers `TOOL`, `CONTROL`, `SPACER`, `SEPARATOR`, groups.
- `TOOLBAR_CONFIGURATION::AppendAction( const std::string& )` and
  `AppendAction( const TOOL_ACTION& )` (`:121-122`) — items reference actions **by name**.
- Eeschema's defaults: `SCH_EDIT_TOOLBAR_SETTINGS : public TOOLBAR_SETTINGS`
  (`eeschema/toolbars_sch_editor.h:39`, settings name `"eeschema-toolbars"` at `:43`), with
  `std::optional<TOOLBAR_CONFIGURATION> DefaultToolbarConfig( TOOLBAR_LOC aToolbar ) override`
  (`:49`, impl `eeschema/toolbars_sch_editor.cpp:55`). `TOOLBAR_LOC ∈ {TOP_MAIN, TOP_AUX,
  LEFT, RIGHT}`.
- It is a `TOOLBAR_SETTINGS`, i.e. **JSON-persisted** through `SETTINGS_MANAGER`
  (`include/settings/settings_manager.h:173`), so user customisation already round-trips
  outside of wx.

`ACTION_TOOLBAR` (`include/tool/action_toolbar.h`) is the wx *renderer* of that configuration
and is replaced. **`TOOLBAR_CONFIGURATION` itself is kept** — the Rust UI reads it, resolves
each item name through `ACTION_MANAGER::FindAction()`, and draws it natively.

Menus are the weaker half: `class ACTION_MENU : public wxMenu`
(`include/tool/action_menu.h:42`), and `eeschema/menubar.cpp` builds the menu bar
imperatively against wx. There is **no** equivalent declarative `MENU_CONFIGURATION`. The
Rust UI must reconstruct the menu tree; the good news is that every leaf is a `TOOL_ACTION`
reachable by name, so only the *structure* has to be re-authored (once), not the content.
Context menus (`TOOL_INTERACTIVE::SetContextMenu( ACTION_MENU*, CONTEXT_MENU_TRIGGER )`,
`include/tool/tool_interactive.h:81`) are the same problem in miniature and will need a
non-wx `ACTION_MENU` equivalent.

### 8.7 Verdict on the registry

**Low risk, and the best-shaped seam in the whole project.**

| Concern | Status |
|---|---|
| Enumerate all ~445 actions at runtime, headless | **works today** — `ACTION_MANAGER::GetActionList()` is a static list populated before `main()` |
| id, label, tooltip, description, scope, flags | **available** as plain getters |
| Fire an action from the UI | **`TOOL_ACTION::MakeEvent()` → `TOOL_MANAGER::ProcessEvent()`** |
| Enable / check state | **`ACTION_CONDITIONS` via `RegisterUIUpdateHandler`** |
| Default + user hotkeys, conflict checking, persistence | **available**, keyed by stable dotted name |
| Icons | **1,431 SVGs on disk, enumerator name == filename** — load with `resvg` |
| Toolbar layout | **`TOOLBAR_CONFIGURATION`, JSON-persisted, action-name-referenced** |
| Menu layout | **must be re-authored** — `ACTION_MENU` is a `wxMenu` and `menubar.cpp` is imperative |

## 9. Rendering rules

*(What the recorded primitives actually mean. This is unchanged from the original brief's
section 7 and remains fully applicable: it tells a wgpu renderer what the `DRAW_STREAM`
commands are supposed to look like on screen, and it tells anyone debugging `SCH_PAINTER`
output what the correct values are.)*

Source of truth: `eeschema/sch_painter.cpp` (3,748 lines), `eeschema/sch_render_settings.{h,cpp}`,
`eeschema/default_values.h`, `common/settings/builtin_color_themes.h`, `eeschema/sch_view.h`,
`eeschema/sch_draw_panel.cpp`.

### 9.1 Coordinate system, units, scale

- **Model space:** `VECTOR2I` (i32), 1 IU = 100 nm, 10 000 IU/mm, 254 IU/mil (§5.1).
- **Y axis is DOWN** in schematic space. Symbol bodies are stored with Y up in the *file* and
  flipped on load; in memory everything is Y-down.
- **Symbol placement transform** is the 2×2 integer `TRANSFORM {x1,y1,x2,y2}`
  (`libs/kimath/include/transform.h:41`), default identity `{1,0,0,1}`.
  `SCH_SYMBOL::SetOrientation` composes it (`eeschema/sch_symbol.cpp:3453-3505`):

  | Op | matrix `{x1,y1,x2,y2}` |
  |---|---|
  | `SYM_ORIENT_0` | `{1,0,0,1}` (reset) |
  | rotate CCW step | `{0,1,-1,0}` |
  | rotate CW step | `{0,-1,1,0}` |
  | `SYM_MIRROR_Y` (incremental) | `{-1,0,0,1}` |
  | `SYM_MIRROR_X` (incremental) | `{1,0,0,-1}` |

  `SYM_ORIENT_90/180/270` are implemented as repeated applications of the CCW step.
  `GetOrientation()` canonicalises `SYM_MIRROR_Y` into `SYM_MIRROR_X` + 180° rotation
  (`sch_symbol.h:282-286`).
- **Zoom / screen scale:** obtained from `GAL::GetScreenWorldMatrix()` (a `MATRIX3x3D`) and
  `GAL::GetWorldScale()`. Several visual elements are deliberately **zoom-relative** (see
  §9.5, §9.6).

### 9.2 Default values (`eeschema/default_values.h`)

All in **mils** unless noted. `schIUScale.MilsToIU(x) = round(x * 254)`.

| Constant | Value (mils) | IU | Meaning |
|---|---|---|---|
| `DEFAULT_LINE_WIDTH_MILS` | 6 | 1524 | default graphic pen width |
| `DEFAULT_WIRE_WIDTH_MILS` | 6 | 1524 | wire width |
| `DEFAULT_BUS_WIDTH_MILS` | 12 | 3048 | bus width |
| `DEFAULT_TEXT_SIZE` | 50 | 12700 | default text height (= 1.27 mm) |
| `DEFAULT_JUNCTION_DIAM` | 36 | 9144 | junction dot **diameter** |
| `DEFAULT_NOCONNECT_SIZE` | 48 | 12192 | no-connect X full size |
| `DEFAULT_SCH_ENTRY_SIZE` | 100 | 25400 | bus/wire entry size |
| `DEFAULT_PIN_LENGTH` | 100 | 25400 | new-pin length |
| `DEFAULT_PINNUM_SIZE` | 50 | 12700 | pin number text height |
| `DEFAULT_PINNAME_SIZE` | 50 | 12700 | pin name text height |
| `DEFAULT_PIN_NAME_OFFSET` | 20 | 5080 | pin-name inset from pin end |
| `DANGLING_SYMBOL_SIZE` | 12 | 3048 | dangling-end box size |
| `UNSELECTED_END_SIZE` | 4 | 1016 | connected-but-unselected end box size |
| `TEXT_ANCHOR_SIZE` | 8 | 2032 | text/field anchor cross size |
| `DEFAULTSELECTIONTHICKNESS` | 3 | 762 | selection halo thickness |
| `SNAP_RANGE` | 55 | 13970 | snap "gravity well" radius |
| `DEFAULT_TEXT_OFFSET_RATIO` | 0.15 | — | text baseline offset above a wire, × font height |
| `DEFAULT_LABEL_SIZE_RATIO` | 0.375 | — | global-label box margin, × font height |
| `DEFAULT_IREF_PREFIX` / `_SUFFIX` | `[` / `]` | — | intersheet-reference brackets |

`SCH_RENDER_SETTINGS` constructor defaults (`eeschema/sch_render_settings.cpp:31-58`):
```
m_ShowPinsElectricalType = true    m_ShowHiddenPins   = true
m_ShowHiddenFields       = true    m_ShowVisibleFields= true
m_ShowPinNumbers = false           m_ShowPinNames     = false
m_ShowPinAltIcons= false           m_ShowRemappedPinNumbers = true
m_ShowDNPMarkers = true            m_ShowDisabled = false, m_ShowGraphicsDisabled = false
m_ShowConnectionPoints = false     m_OverrideItemColors   = false
m_LabelSizeRatio  = 0.375          m_TextOffsetRatio = 0.15
m_PinSymbolSize   = 50 mil / 2 = 25 mil = 6350 IU
m_SymbolLineWidth = 6 mil = 1524 IU
SetDefaultPenWidth( 6 mil )
SetDashLengthRatio( 12 )           // ISO 128-2
SetGapLengthRatio( 3 )             // ISO 128-2
m_minPenWidth = round( ADVANCED_CFG.m_MinPlotPenWidth * 1e4 )
```
Derived: **dangling-indicator line thickness = `m_defaultPenWidth / 3`**
(`sch_render_settings.h:60-63`).

### 9.3 Layers and draw order

`SCH_LAYER_ID` — `include/layer_ids.h:470-528` (50 layers):
`LAYER_WIRE, LAYER_BUS, LAYER_JUNCTION, LAYER_LOCLABEL, LAYER_GLOBLABEL, LAYER_HIERLABEL,
LAYER_PINNUM, LAYER_PINNAM, LAYER_REFERENCEPART, LAYER_VALUEPART, LAYER_FIELDS,
LAYER_INTERSHEET_REFS, LAYER_NETCLASS_REFS, LAYER_RULE_AREAS, LAYER_DEVICE, LAYER_NOTES,
LAYER_PRIVATE_NOTES, LAYER_NOTES_BACKGROUND, LAYER_PIN, LAYER_SHEET, LAYER_SHEETNAME,
LAYER_SHEETFILENAME, LAYER_SHEETFIELDS, LAYER_SHEETLABEL, LAYER_NOCONNECT, LAYER_DANGLING,
LAYER_DNP_MARKER, LAYER_ERC_WARN, LAYER_ERC_ERR, LAYER_ERC_EXCLUSION,
LAYER_EXCLUDED_FROM_SIM, LAYER_SHAPES_BACKGROUND, LAYER_DEVICE_BACKGROUND,
LAYER_SHEET_BACKGROUND, LAYER_SCHEMATIC_GRID, LAYER_SCHEMATIC_GRID_AXES,
LAYER_SCHEMATIC_BACKGROUND, LAYER_SCHEMATIC_CURSOR, LAYER_HOVERED, LAYER_BRIGHTENED,
LAYER_HIDDEN, LAYER_NET_COLOR_HIGHLIGHT, LAYER_DRAG_NET_COLLISION, LAYER_SELECTION_SHADOWS,
LAYER_SCHEMATIC_DRAWINGSHEET, LAYER_SCHEMATIC_PAGE_LIMITS, LAYER_BUS_JUNCTION,
LAYER_SCHEMATIC_AUX_ITEMS, LAYER_SCHEMATIC_ANCHOR, LAYER_OP_VOLTAGES, LAYER_OP_CURRENTS,
LAYER_GROUP`.

**Draw order** — `SCH_LAYER_ORDER[]` in `eeschema/sch_view.h:44-82`. The array is in
**decreasing priority**, i.e. `SetLayerOrder(layer, i)` assigns index `i` in array order
(`eeschema/sch_draw_panel.cpp:122-128`), and `VIEW` renders from the **last** entry to the
first. So the actual **painting order (bottom → top)** is the array **reversed**:

```
LAYER_DRAWINGSHEET
LAYER_NOTES_BACKGROUND
LAYER_SHEET_BACKGROUND
LAYER_DEVICE_BACKGROUND
LAYER_SHAPES_BACKGROUND
LAYER_DRAW_BITMAPS
LAYER_SELECTION_SHADOWS
LAYER_SHEET
LAYER_DEVICE
LAYER_BUS
LAYER_WIRE
LAYER_PRIVATE_NOTES
LAYER_NOTES
LAYER_SHEETFIELDS
LAYER_SHEETLABEL
LAYER_SHEETNAME
LAYER_SHEETFILENAME
LAYER_LOCLABEL
LAYER_GLOBLABEL
LAYER_HIERLABEL
LAYER_NOCONNECT
LAYER_JUNCTION
LAYER_BUS_JUNCTION
LAYER_RULE_AREAS
LAYER_NETCLASS_REFS
LAYER_INTERSHEET_REFS
LAYER_PINNAM
LAYER_PINNUM
LAYER_FIELDS
LAYER_VALUEPART
LAYER_REFERENCEPART
LAYER_OP_CURRENTS
LAYER_OP_VOLTAGES
LAYER_DANGLING
LAYER_ERC_EXCLUSION
LAYER_ERC_WARN
LAYER_ERC_ERR
LAYER_SELECT_OVERLAY
LAYER_GP_OVERLAY                (topmost)
```

> **Note the surprise:** `LAYER_SELECTION_SHADOWS` sits *below* the item layers in the order
> array, but it is rendered to a separate **overlay target**
> (`sch_draw_panel.cpp:173-174`: `SetLayerTarget(LAYER_SELECTION_SHADOWS, TARGET_OVERLAY)` +
> `SetLayerDisplayOnly`), so the halo composites on top. A wgpu renderer should model this as
> a separate pass, not as a z-position.

Other target/flag assignments (`sch_draw_panel.cpp:152-177`):
- `TARGET_NONCACHED`: `LAYER_SCHEMATIC_ANCHOR`, `LAYER_DRAW_BITMAPS`, `LAYER_DRAWINGSHEET`
- `TARGET_OVERLAY`: `LAYER_GP_OVERLAY`, `LAYER_SELECT_OVERLAY`, `LAYER_OP_VOLTAGES`,
  `LAYER_OP_CURRENTS`, `LAYER_SELECTION_SHADOWS`
- `SetLayerDisplayOnly` (not hit-tested / not part of the document):
  `LAYER_SCHEMATIC_ANCHOR`, `LAYER_GP_OVERLAY`, `LAYER_SELECT_OVERLAY`, `LAYER_DRAWINGSHEET`,
  `LAYER_OP_*`, `LAYER_SELECTION_SHADOWS`, `LAYER_NET_COLOR_HIGHLIGHT`, `LAYER_DANGLING`

### 9.4 Default colour theme

`common/settings/builtin_color_themes.h:28` `s_defaultTheme` (there is also
`s_classicTheme` at `:317`). Schematic entries, RGBA (0-255, alpha 0-1):

| Layer | R,G,B,A |
|---|---|
| `LAYER_SCHEMATIC_BACKGROUND` | 245, 244, 239, 1 |
| `LAYER_SCHEMATIC_GRID` | 181, 181, 181, 1 |
| `LAYER_SCHEMATIC_GRID_AXES` | 0, 0, 132, 1 |
| `LAYER_SCHEMATIC_CURSOR` | 15, 15, 15, 1 |
| `LAYER_SCHEMATIC_ANCHOR` | 0, 0, 255, 1 |
| `LAYER_SCHEMATIC_AUX_ITEMS` | 0, 0, 0, 1 |
| `LAYER_SCHEMATIC_DRAWINGSHEET` | 132, 0, 0, 1 |
| `LAYER_SCHEMATIC_PAGE_LIMITS` | 181, 181, 181, 1 |
| `LAYER_WIRE` | 0, 150, 0, 1 |
| `LAYER_BUS` | 0, 0, 132, 1 |
| `LAYER_JUNCTION` | 0, 150, 0, 1 |
| `LAYER_BUS_JUNCTION` | 0, 0, 132, 1 |
| `LAYER_NOCONNECT` | 0, 0, 132, 1 |
| `LAYER_DEVICE` | 132, 0, 0, 1 |
| `LAYER_DEVICE_BACKGROUND` | 255, 255, 194, 1 |
| `LAYER_PIN` | 132, 0, 0, 1 |
| `LAYER_PINNAM` | 0, 100, 100, 1 |
| `LAYER_PINNUM` | 169, 0, 0, 1 |
| `LAYER_REFERENCEPART` | 0, 100, 100, 1 |
| `LAYER_VALUEPART` | 0, 100, 100, 1 |
| `LAYER_FIELDS` | 132, 0, 132, 1 |
| `LAYER_LOCLABEL` | 15, 15, 15, 1 |
| `LAYER_GLOBLABEL` | 132, 0, 0, 1 |
| `LAYER_HIERLABEL` | 114, 86, 0, 1 |
| `LAYER_NETCLASS_REFS` | 72, 72, 72, 1 |
| `LAYER_RULE_AREAS` | 255, 0, 0, 1 |
| `LAYER_NOTES` | 0, 0, 194, 1 |
| `LAYER_PRIVATE_NOTES` | 72, 72, 255, 1 |
| `LAYER_NOTES_BACKGROUND` | 0, 0, 0, **0** (transparent) |
| `LAYER_SHEET` | 132, 0, 0, 1 |
| `LAYER_SHEET_BACKGROUND` | 255, 255, 255, **0** (transparent) |
| `LAYER_SHEETNAME` | 0, 100, 100, 1 |
| `LAYER_SHEETFILENAME` | 114, 86, 0, 1 |
| `LAYER_SHEETFIELDS` | 132, 0, 132, 1 |
| `LAYER_SHEETLABEL` | 0, 100, 100, 1 |
| `LAYER_HIDDEN` | 194, 194, 194, 1 |
| `LAYER_HOVERED` | 0, 0, 255, 1 |
| `LAYER_BRIGHTENED` | 255, 0, 255, 1 |
| `LAYER_SELECTION_SHADOWS` | `COLOR4D(0.4, 0.7, 1.0, 0.8)` (float; a `.3/.7/1.0/0.6` entry is overwritten) |
| `LAYER_DNP_MARKER` | 220, 9, 13, 0.85 |
| `LAYER_EXCLUDED_FROM_SIM` | 194, 194, 194, 0.95 |
| `LAYER_ERC_ERR` | 230, 9, 13, 0.8 |
| `LAYER_ERC_WARN` | 209, 146, 0, 0.8 |
| `LAYER_ERC_EXCLUSION` | 194, 194, 194, 0.8 |
| `LAYER_DRAG_NET_COLLISION` | 230, 9, 13, 0.8 |
| `LAYER_OP_VOLTAGES` | 132, 0, 50, 1 |
| `LAYER_OP_CURRENTS` | 224, 0, 12, 1 |

`SCH_RENDER_SETTINGS::LoadColors` (`sch_render_settings.cpp:63-77`) also aliases
`m_layerColors[LAYER_AUX_ITEMS] = m_layerColors[LAYER_SCHEMATIC_AUX_ITEMS]`.

### 9.5 Colour and width resolution

**`SCH_PAINTER::getRenderColor`** — `sch_painter.cpp:308`. Order of resolution:
1. Start with `m_schSettings.GetLayerColor(aLayer)`.
2. If **not** `m_OverrideItemColors`, item-specific colours win:
   `SCH_LINE::GetLineColor()`, `SCH_BUS_WIRE_ENTRY::GetBusEntryColor()`,
   `SCH_JUNCTION::GetJunctionColor()`, `SCH_SHEET` border/background, text colours, etc.
3. Background layers (`LAYER_DEVICE_BACKGROUND`, `LAYER_NOTES_BACKGROUND`,
   `LAYER_SHAPES_BACKGROUND`, `LAYER_SHEET_BACKGROUND`) are special-cased.
4. If `m_ShowDisabled` (or `m_ShowGraphicsDisabled` for non-field items): `color.Darken(0.5)`
   (`:480-485`).
5. If dimmed (and not a drawing-shadow pass for a selected item):
   `color.Desaturate(); color = color.Mix(backgroundColour, 0.5)` (`:487-493`).
6. If `GetForcedTransparency() > 0`: `color.WithAlpha(color.a * (1 - transparency))` (`:495`).

**`SCH_PAINTER::getLineWidth`** — `:500`:
```
width = item->GetEffectivePenWidth(&m_schSettings)          // 0 stroke width -> default pen
if (brightened || selected) and shadow-pass and item is in g_ScaledSelectionTypes:
    width += getShadowWidth(isBrightened)
if drawing wire-colour highlights:
    width += MilsToIU( eeconfig()->m_Selection.highlight_netclass_colors_thickness )  // default 15 mil
```

**`SCH_PAINTER::getShadowWidth`** — `:295` (this is the selection halo, and it is
**zoom-relative**):
```
milsWidth = forHighlight ? eeconfig()->m_Selection.highlight_thickness
                         : eeconfig()->m_Selection.selection_thickness   // default 3
return |screenWorldMatrix.GetScale().x * milsWidth| + MilsToIU(milsWidth)
```
`g_ScaledSelectionTypes` (`:86-107`) — types whose halo scales:
`SCH_MARKER_T, SCH_JUNCTION_T, SCH_NO_CONNECT_T, SCH_BUS_WIRE_ENTRY_T, SCH_BUS_BUS_ENTRY_T,
SCH_LINE_T, SCH_SHAPE_T, SCH_RULE_AREA_T, SCH_BITMAP_T, SCH_TEXT_T, SCH_GLOBAL_LABEL_T,
SCH_DIRECTIVE_LABEL_T, SCH_FIELD_T, SCH_HIER_LABEL_T, SCH_SHEET_PIN_T, LIB_SYMBOL_T,
SCH_SYMBOL_T, SCH_SHEET_T, SCH_PIN_T`.

**`SCH_PAINTER::getTextThickness`** — `:529`: per-type `GetEffectiveTextPenWidth(defaultPen)`.
Pin name/number thickness additionally goes through
`ClampTextPenSize(width, textSize, true)` (`:1142-1143`).

### 9.6 Per-item drawing rules

#### Junction (`draw(SCH_JUNCTION*)`, `:1646`)
- `junctionSize = GetEffectiveDiameter() / 2` — **the stored value is a diameter; draw radius
  is half.** Default diameter 36 mil ⇒ radius 18 mil = 4572 IU.
- Drawn only when `junctionSize > 1`.
- Normal pass: **filled** circle, no stroke. Shadow pass: stroked, not filled, line width
  `getLineWidth(item, true)`.
- Colour: `LAYER_JUNCTION` or `LAYER_BUS_JUNCTION` via `getRenderColor`, unless netclass
  colour highlighting is on and the layer matches the item's own layer, in which case the raw
  layer colour is used (`:1663-1667`).

#### No-connect (`draw(SCH_NO_CONNECT*)`, `:3458`)
```
delta = max( aNC->GetSize(), defaultPenWidth * 3 ) / 2      // default size 48 mil
draw line (p.x-delta, p.y-delta) -> (p.x+delta, p.y+delta)
draw line (p.x-delta, p.y+delta) -> (p.x+delta, p.y-delta)
```
Stroke only, colour `LAYER_NOCONNECT`, width `getLineWidth`.

#### Wire / bus (`draw(SCH_LINE*)`, `:1682`)
- `defaultLineWidth = MilsToIU(6)`, overridden by
  `Schematic()->Settings().m_DefaultLineWidth` when a schematic is attached (`:1690-1696`).
- Layers handled: `LAYER_WIRE`, `LAYER_BUS`, `LAYER_NOTES`, plus overlay passes
  `LAYER_SELECTION_SHADOWS`, `LAYER_NET_COLOR_HIGHLIGHT`, `LAYER_DANGLING`,
  `LAYER_OP_VOLTAGES`.
- **Hop-over** (wire crossings drawn as arcs): scale from
  `Schematic()->Settings().GetHopOverScale()`.
- Line style comes from `GetEffectiveLineStyle()`; dash/gap ratios are 12 / 3 (ISO 128-2).

#### Bus entry (`draw(SCH_BUS_ENTRY_BASE*)`, `:3481`)
Rendered by constructing a temporary `SCH_LINE` on `LAYER_WIRE` (wire entry) or `LAYER_BUS`
(bus entry) between `GetPosition()` and `GetEnd()`, then drawing that line. When the entry is
selected the temp line is marked selected **and** flagged `STARTPOINT|ENDPOINT` so
unselected-endpoint markers are never shown on bus entries (`:3513-3518`).

#### Shapes (`draw(SCH_SHAPE*)`, `:1917`)
- Skipped if `IsPrivate()` and not in the symbol editor (`:1921`).
- A `SCH_RULE_AREA` that only carries directive labels follows those labels' visibility
  (`:1927-1934`).
- Arcs: `SHAPE_ARC(start, mid, end, 0)` → centre/radius/start-angle/central-angle; if line
  endings shorten it, `ShortenArcForEndings()` adjusts start/sweep (`:1951-1960`).
- Rounded rectangles: built as a `ROUNDRECT` → `TransformToPolygon(poly, GetMaxError())` and
  drawn as a polygon (`:1971-1982`); plain rects use `DrawRectangle`.
- **Fill rules:** `FILL_T::NO_FILL` → stroke only. `FILLED_SHAPE` (`outline`) → fill with the
  *outline* colour. `FILLED_WITH_BG_BODYCOLOR` (`background`) → fill with the layer's
  background colour (`LAYER_DEVICE_BACKGROUND` for symbol bodies, `LAYER_NOTES_BACKGROUND`
  for notes, `LAYER_SHAPES_BACKGROUND` for schematic shapes, `LAYER_SHEET_BACKGROUND` for
  sheets). `FILLED_WITH_COLOR` → the explicit `(fill (color …))`. `HATCH` / `REVERSE_HATCH` /
  `CROSS_HATCH` → hatch pattern in the explicit colour (fmt v20250222).
  **Background fills are drawn on a separate, lower layer** than the outline — see §9.3.

#### Pins (`draw(SCH_PIN*)`, `:913`)

Pins are drawn **from the parent symbol's `LIB_SYMBOL`**, never from the `SCH_SYMBOL`'s own
pin list — the painter returns immediately if `aPin->GetParentSymbol()` is a `SCH_SYMBOL`
(`:916-919`).

Decoration sizes:
- `externalPinDecoSize(pin)` (`:776`) = `m_PinSymbolSize` if `> 0`, else
  `GetNumberTextSize() / 2`. Used as the **radius** of the inversion bubble, the polarity
  slope and the non-logic cross.
- `internalPinDecoSize(pin)` (`:765`) = `m_PinSymbolSize` if `> 0`, else
  `GetNameTextSize()/2` (falling back to `GetNumberTextSize()/2` when the name size is 0).
  Used for the clock wedge.
- Default `m_PinSymbolSize` = 25 mil (6350 IU).

Geometry: `p0 = pin.GetPinRoot()` (the body end), `pos = pin.GetPosition()` (the connection
end), `dir = (sign(pos.x - p0.x), sign(pos.y - p0.y))`, `len = pin.GetLength()`,
`radius = externalPinDecoSize`, `diam = 2 * radius`, `clock_size = internalPinDecoSize`.

**Graphic styles** (`:1024-1131`) — exact constructions:

| Style | Drawing |
|---|---|
| `PT_NC` electrical type (overrides shape) | line `p0→pos`, plus an X at `pos` of half-extent `TARGET_PIN_RADIUS` |
| `LINE` | line `p0 → pos` |
| `INVERTED` | circle centre `p0 + dir*radius`, radius `radius`; line `p0 + dir*diam → pos` |
| `INVERTED_CLOCK` | clock wedge: polyline `p0 + (dir.y,-dir.x)*clock_size → p0 - dir*clock_size → p0 + (-dir.y,dir.x)*clock_size`; plus the `INVERTED` bubble and shortened line |
| `CLOCK` | line `p0 → pos`; wedge **inside** the body: if horizontal, polyline `p0+(0,clock_size) → p0+(-dir.x*clock_size,0) → p0+(0,-clock_size)`; if vertical, `p0+(clock_size,0) → p0+(0,-dir.y*clock_size) → p0+(-clock_size,0)` |
| `CLOCK_LOW` / `FALLING_EDGE_CLOCK` | clock wedge (as `INVERTED_CLOCK`'s wedge) **plus** the low-polarity slope: horizontal → polyline `p0+(dir.x,0)*diam → p0+(dir.x,-1)*diam → p0`; vertical → `p0+(0,dir.y)*diam → p0+(-1,dir.y)*diam → p0`; then line `p0 → pos` |
| `INPUT_LOW` | line `p0 → pos` plus the same low-polarity slope triangle as above |
| `OUTPUT_LOW` | line `p0 → pos`; horizontal → line `p0-(0,diam) → p0+(dir.x,0)*diam`; vertical → line `p0-(diam,0) → p0+(0,dir.y)*diam` |
| `NONLOGIC` | line `p0 → pos` plus an X centred on `p0`: lines `p0 ∓ (dir.x+dir.y, dir.y-dir.x)*radius` and `p0 ∓ (dir.x-dir.y, dir.x+dir.y)*radius` |

(`triLine(a,b,c)` at `:713` is just two connected segments.)

**Pin visibility:** an invisible pin is drawn only if `eeconfig()->m_Appearance.show_hidden_pins`
(or `m_ShowHiddenPins` when there is no schematic), and then in `LAYER_HIDDEN` colour
(`:938-960`). Invisible **global power** pins still get a dangling indicator (`:952`).

**Pin name / number placement** is computed by `PIN_LAYOUT_CACHE`
(`eeschema/pin_layout_cache.{h,cpp}`) and returned as `TEXT_INFO { m_Text, m_TextPosition,
m_TextSize, m_Thickness, m_HAlign, m_VAlign, m_Angle }` (`sch_painter.cpp:1466-1489`). Rules
encoded there:
- If the symbol's `pin_names` offset is **> 0**, names are drawn **inside** the body, offset
  by that amount from the pin root, aligned along the pin, and numbers are drawn **above** the
  pin line, centred on the pin body.
- If the offset is **0**, names are drawn **outside**, above the pin line, and numbers below.
- `pin_numbers (hide yes)` / `pin_names (hide yes)` suppress them; `m_ShowPinNumbers` /
  `m_ShowPinNames` in render settings can force them on.
- Multi-line (stacked) pin names get **braces** drawn around them (`drawBrace`,
  `sch_painter.cpp:1168-1200`) — a 5-point polyline offset by `braceWidth` in X (horizontal
  text) or Y (vertical text), respecting the text angle.
- Alt-mode pins get a small icon (`drawAltPinModesIcon`, `:842`) when
  `m_ShowPinAltIcons`.
- `m_ShowPinsElectricalType` appends the electrical type token next to the pin.
- `m_ShowRemappedPinNumbers` appends the original pin number when remapped through a pin map.

**Bitmap-text optimisation** (`:1155-1165`): when
`textSize * gal.GetWorldScale() < 3.5`, text is rendered with the bitmap font instead of
stroke/outline glyphs, and the item is flagged `IS_SHOWN_AS_BITMAP`. A wgpu renderer should
adopt an equivalent LOD threshold.

**Local power icon** (`drawLocalPowerIcon`, `:809`): `lineWidth = size / 10`; shape list from
`SCH_SYMBOL::BuildLocalPowerIconShape()` — currently only BEZIER and CIRCLE primitives.

#### Dangling ends and anchors

**`drawDanglingIndicator`** — `:1620`:
```
size   = dangling ? DANGLING_SYMBOL_SIZE (12 mil) : UNSELECTED_END_SIZE (4 mil)
if !dangling: width /= 2
radius = width + MilsToIU(size / 2)                 // integer division of the mil value
strokeColor = color.Brightened(0.3)                 // so it stays visible over a junction dot
lineWidth   = shadowPass ? getShadowWidth(brightened) : defaultPenWidth / 3
draw an unfilled RECTANGLE from (pos - radius) to (pos + radius)
```
Skipped entirely when printing.

**`drawPinDanglingIndicator`** — `:786`: an **unfilled circle** from
`PIN_LAYOUT_CACHE::GetDanglingIndicator()` (a `CIRCLE`), same `Brightened(0.3)` colour and
`defaultPenWidth / 3` thickness.

**`drawAnchor`** — `:1596` (the text/field anchor cross). **Zoom-relative:**
```
radius = round( |screenWorldMatrix.GetScale().x * TEXT_ANCHOR_SIZE| / 25.0 ) + MilsToIU(TEXT_ANCHOR_SIZE)
colour = shadowPass ? LAYER_SELECTION_SHADOWS : LAYER_SCHEMATIC_ANCHOR
lineWidth = shadowPass ? getShadowWidth(false) : defaultPenWidth / 3
draw a '+' : horizontal and vertical segments of half-length `radius`
```
Skipped when printing.

#### Selection / highlight

- Selection is a **halo drawn on `LAYER_SELECTION_SHADOWS`** in the overlay target, using the
  item's own geometry with `lineWidth += getShadowWidth()` (§9.5) and the
  `LAYER_SELECTION_SHADOWS` colour `rgba(0.4, 0.7, 1.0, 0.8)`.
- Every `draw()` early-returns on the shadow layer unless the item `IsBrightened() ||
  IsSelected()` (e.g. `:1656`, `:3463`).
- Shadows are never drawn when printing (`m_schSettings.IsPrinting()`).
- **Brightened** items use `LAYER_BRIGHTENED` magenta `255,0,255` and
  `eeconfig()->m_Selection.highlight_thickness` for the halo.
- **Child selection:** `eeconfig()->m_Selection.draw_selected_children` gates whether a
  selected pin's name/number also get a halo (`:1135`).
- **Hover** uses `LAYER_HOVERED` blue `0,0,255`.
- `nonCached(item)` is simply `item->IsSelected()` (`:258`) — selected items are drawn into
  the non-cached/overlay path.

#### DNP marker (`:2809` for symbols, `:3396` for sheets)
```
bbox  = item body bbox ; pins = body+pins bbox
margins = ( max(bbox.x - pins.x, pins.right - bbox.right),
            max(bbox.y - pins.y, pins.bottom - bbox.bottom) )
margins.x = max( margins.x * 0.6, margins.y * 0.3 )
margins.y = max( margins.y * 0.6, margins.x * 0.3 )
bbox.Inflate( margins )
strokeWidth = 3 * MilsToIU(DEFAULT_LINE_WIDTH_MILS)      // 18 mil
draw two filled+stroked segments across the diagonals of bbox, colour LAYER_DNP_MARKER
(gal.AdvanceDepth() first, so it composites above the symbol)
```

#### Operating points (`LAYER_OP_VOLTAGES` / `LAYER_OP_CURRENTS`)
Text size is **zoom-relative** (`getOperatingPointTextSize`, `:564`):
```
docTextSize    = MilsToIU(50)
screenTextSize = |int(screenWorldMatrix.GetScale().y) * 7|
return round( (docTextSize + screenTextSize * 2) / 3.0 )    // "66% zoom-relative"
```
Pin currents: drawn at the pin midpoint, offset `round(textSize * 0.22) * 1.2` (X for vertical
pins, −Y for horizontal pins), left/centre-aligned, **always the stroke font** for
performance, `StrokeWidth = GetPenSizeForDemiBold(textSize)`, and rendered with
**`knockoutText`** (`:667`) so it stays legible over the wire. Only drawn when
`len > textSize` (`:986`).

#### Global label body (`SCH_GLOBALLABEL::CreateGraphicShape`, `eeschema/sch_label.cpp:2326`)
```
margin   = LabelBoxExpansion = round( m_LabelSizeRatio (0.375) * textSize.y )   // sch_label.cpp:1126
halfSize = textHeight/2 + margin
linewidth= GetPenWidth()
symb_len = GetTextBox().GetWidth() + 2*margin
x = symb_len + linewidth + 3
y = halfSize + linewidth + 3

points (6, before shaping):  (0,0) (0,-y) (-x,-y) (-x,0) (-x,y) (0,y)

shape adjustment:
  L_INPUT             : x_offset = -halfSize ; points[0].x += halfSize
  L_OUTPUT            : points[3].x -= halfSize
  L_BIDI / L_TRISTATE : x_offset = -halfSize ; points[0].x += halfSize ; points[3].x -= halfSize
  L_UNSPECIFIED       : (no change)

for each point: p.x += x_offset ; rotate by spin style ; p += aPos
  spin LEFT   -> no rotation
  spin UP     -> RotatePoint(-90°)
  spin RIGHT  -> RotatePoint(180°)
  spin BOTTOM -> RotatePoint(+90°)

finally push points[0] again to close
```

#### Hierarchical label / sheet pin body (`SCH_HIERLABEL::CreateGraphicShape`, `sch_label.cpp:2456`)

Template-driven. `halfSize = textHeight / 2`. Each template is
`{ nCorners, x0,y0, x1,y1, … }` in units of `halfSize`; corner = `(halfSize*tx + pos.x,
halfSize*ty + pos.y)`. Indexed `TemplateShape[shape][spin]` where shape ∈
`{L_INPUT, L_OUTPUT, L_BIDI, L_TRISTATE, L_UNSPECIFIED}` (index 0..4) and spin ∈
`{LEFT(0), UP(1), RIGHT(2), BOTTOM(3)}` — but note the array literal orders the columns
`{HN, UP, HI, BOTTOM}` (`sch_label.cpp:92-96`), i.e. `HN`=LEFT, `HI`=RIGHT.

Verbatim from `eeschema/sch_label.cpp:67-90`:
```c
TemplateIN_HN      = { 6,  0,0, -1,-1, -2,-1, -2,1, -1,1, 0,0 }
TemplateIN_HI      = { 6,  0,0,  1, 1,  2, 1,  2,-1, 1,-1, 0,0 }
TemplateIN_UP      = { 6,  0,0,  1,-1,  1,-2, -1,-2, -1,-1, 0,0 }
TemplateIN_BOTTOM  = { 6,  0,0,  1, 1,  1, 2, -1, 2, -1, 1, 0,0 }

TemplateOUT_HN     = { 6, -2,0, -1, 1,  0, 1,  0,-1, -1,-1, -2,0 }
TemplateOUT_HI     = { 6,  2,0,  1,-1,  0,-1,  0, 1,  1, 1,  2,0 }
TemplateOUT_UP     = { 6,  0,-2, 1,-1,  1, 0, -1, 0, -1,-1,  0,-2 }
TemplateOUT_BOTTOM = { 6,  0, 2, 1, 1,  1, 0, -1, 0, -1, 1,  0, 2 }

TemplateUNSPC_HN     = { 5, 0,-1, -2,-1, -2,1,  0,1,  0,-1 }
TemplateUNSPC_HI     = { 5, 0,-1,  2,-1,  2,1,  0,1,  0,-1 }
TemplateUNSPC_UP     = { 5, 1, 0,  1,-2, -1,-2, -1,0, 1, 0 }
TemplateUNSPC_BOTTOM = { 5, 1, 0,  1, 2, -1, 2, -1,0, 1, 0 }

TemplateBIDI_HN      = { 5, 0,0, -1,-1, -2,0, -1,1, 0,0 }
TemplateBIDI_HI      = { 5, 0,0,  1,-1,  2,0,  1,1, 0,0 }
TemplateBIDI_UP      = { 5, 0,0, -1,-1,  0,-2, 1,-1, 0,0 }
TemplateBIDI_BOTTOM  = { 5, 0,0, -1, 1,  0, 2, 1, 1, 0,0 }

Template3STATE_*  ==  TemplateBIDI_*   (identical coordinates)
```

Hier-label body bbox (`sch_label.cpp:2480`): `height = textHeight + penWidth + textOffset`;
`length = GetTextBox().GetWidth() + height` (the extra `height` accounts for the triangular
nose); the origin is shifted by `MilsToIU(DANGLING_SYMBOL_SIZE)` along the spin direction.

#### Label / text offset from a wire
`SCH_TEXT::GetTextOffset` (`eeschema/sch_text.cpp:305`):
`round( m_TextOffsetRatio (0.15) * GetTextSize().y )` — the gap between a label's baseline
and the wire it sits on.

#### Sheet (`draw(SCH_SHEET*)`, `:3326`)
Border stroke from the sheet's `(stroke …)` (`LAYER_SHEET` colour when unspecified);
background from `(fill (color …))` on `LAYER_SHEET_BACKGROUND` (default fully transparent).
Fields `Sheetname` / `Sheetfile` draw on `LAYER_SHEETNAME` / `LAYER_SHEETFILENAME`; other
sheet fields on `LAYER_SHEETFIELDS`; sheet pins on `LAYER_SHEETLABEL` using the hier-label
templates above.

#### Bitmap (`draw(SCH_BITMAP*)`, `:3572`)
Drawn on `LAYER_DRAW_BITMAPS` (a non-cached target). Position is the image **centre**;
extent = pixel dimensions × `GetImageScale()` × (PPI → IU). See
`common/bitmap_base.cpp` / `REFERENCE_IMAGE`.

#### Group (`draw(SCH_GROUP*)`, `:3656`)
Draws the group's bounding outline plus its name on `LAYER_GROUP`.

### 9.7 Text: justification, rotation, fonts

- `TEXT_ATTRIBUTES` carries `m_Font, m_Size (VECTOR2I h,w), m_StrokeWidth, m_Angle,
  m_Halign, m_Valign, m_Bold, m_Italic, m_Mirrored, m_Color, m_LineSpacing`.
- Horizontal justify ∈ `GR_TEXT_H_ALIGN_{LEFT, CENTER, RIGHT}`;
  vertical ∈ `GR_TEXT_V_ALIGN_{TOP, CENTER, BOTTOM}`. Centre is the default and is **not**
  serialised (§5.1).
- **Rotation is only ever 0° or 90°** for schematic text on disk; 180°/270° presentation comes
  from the label spin style (§5.3). `ANGLE_HORIZONTAL` = 0°, `ANGLE_VERTICAL` = 90°.
- **Multi-line layout** (`drawMultiLineText`, `sch_painter.cpp:~1380-1460`): for
  `ANGLE_VERTICAL`, lines advance in **+X**; for horizontal, in **+Y**. Start position is
  pre-adjusted by the total extent when the alignment is `CENTER` (half) or
  `BOTTOM`/`RIGHT` (full). Each line is `Trim()`ed both ends before drawing.
- Fonts: `SCH_PAINTER::getFont` (`:287`) prefers `EDA_TEXT::GetDrawFont(&m_schSettings)`, else
  `KIFONT::FONT::GetFont(m_schSettings.GetDefaultFont(), bold, italic)`. The stock font is
  KiCad's **stroke** font (`newstroke`); `(face "…")` selects an outline font resolved via
  fontconfig/freetype/harfbuzz, possibly from `(embedded_files)`.
- `knockoutText` (`:667`) renders text with a background-coloured halo — used for operating
  points.
- `bitmapText` (`:627`) is the low-LOD path (§9.6).

### 9.8 Grid and background

- Background is a full-viewport clear in `LAYER_SCHEMATIC_BACKGROUND` (245,244,239).
- Grid colour `LAYER_SCHEMATIC_GRID` (181,181,181), axes `LAYER_SCHEMATIC_GRID_AXES`
  (0,0,132), from `SCH_RENDER_SETTINGS::GetGridColor()` / `GetCursorColor()`
  (`sch_render_settings.h:85-86`). Grid rendering itself lives in
  `common/gal/graphics_abstraction_layer.cpp` (`GAL::DrawGrid`), driven by
  `GAL_DISPLAY_OPTIONS` (dots / lines / small-crosses, `m_gridStyle`), not by `SCH_PAINTER`.
- Page limits: `LAYER_SCHEMATIC_PAGE_LIMITS` (181,181,181), gated by
  `SCH_RENDER_SETTINGS::GetShowPageLimits()` → `eeconfig()->m_Appearance.show_page_limits &&
  !IsPrinting()` (`sch_render_settings.cpp:80-84`) — **an `eeconfig()` call in the render
  settings, worth noting as an FFI hazard.**
- Drawing sheet (title block frame) renders on `LAYER_SCHEMATIC_DRAWINGSHEET` /
  `LAYER_SCHEMATIC_PAGE_LIMITS` via `DS_PROXY_VIEW_ITEM` (`eeschema/sch_view.cpp:137-138`).

### 9.9 Bounding boxes / hit testing

`SCH_PAINTER::drawItemBoundingBox` (`:236`) draws debug bboxes when
`m_schSettings.GetDrawBoundingBoxes()`. Real hit testing is
`EDA_ITEM::HitTest(VECTOR2I, int accuracy)` / `HitTest(BOX2I, bool contained, int accuracy)`
per subclass, backed by `SCH_RTREE` (`eeschema/sch_rtree.h`) for broad phase.
`SNAP_RANGE` = 55 mil is the snapping gravity well.

---

## 10. Test fixture inventory

### 10.1 Counts

| Location | `.kicad_sch` | `.kicad_sym` |
|---|---|---|
| `qa/data/**` | **350** | **21** |
| `demos/**` | **116** | several (e.g. `demos/vme-wren/vme-wren.kicad_sym`) |
| **Total `.kicad_sch`** | **466** | — |

**Under the decided cut these are render fixtures, not parser fixtures.** The C++ reader
and writer are unchanged, so no round-trip testing is needed. What this corpus is for now:

- **golden draw streams.** `DRAW_STREAM::Serialize`/`Deserialize`
  (`include/gal/recording/draw_stream.h:192,195`) makes it possible to load each file
  headlessly, drive `KIGFX::VIEW` + `SCH_PAINTER` into a `RECORDING_GAL`, serialise the
  stream and diff it against a committed golden. That is a regression test for the *seam*
  which needs neither a GPU nor a window.
- **renderer comparison**, once wgpu output exists, against
  `kicad-cli sch export svg` / the `test_symbol_svg_export.cpp` references.
- **coverage**: the feature map below tells you which files exercise which primitives, so a
  renderer gap shows up as a specific missing opcode rather than "it looks wrong".

### 10.2 The single most important fixture

**`qa/data/eeschema/api_kitchen_sink.kicad_sch`** (2,725 lines, version `20260830`).
It is the only file in the tree that simultaneously exercises:
`table_cell` + `(column_count …)` tables, `bezier`, `start_shape`/`end_shape` line endings,
`custom_property` (the **only** file using it), `body_styles`, `jumper_pin_groups`,
`pin_maps` + `associated_footprints`, `text_box`, `netclass_flag`, `rule_area`, `group`,
`image`, `embedded_files`, `net_chain`.
**Make this the first file the Rust parser round-trips.**

### 10.3 Feature coverage map

Counts are "number of `.kicad_sch` files under `qa/data` + `demos` containing the token".

| Feature / token | files | Representative fixtures |
|---|---|---|
| `bus_entry` | 349 | ubiquitous |
| `pin_map` | 279 | ubiquitous (in `lib_symbols`) |
| `hatch` | 272 | ubiquitous (fill type tokens) |
| `net_chain` | 72 | `qa/data/eeschema/net_chains_four_nets.kicad_sch`, `qa/data/pcbnew/net_chain_bridging_tuning_profiles.kicad_sch`, `qa/data/pcbnew/drc_net_chain_tuning_profiles/test_drc_net_chain_tuning_profiles.kicad_sch`, `qa/data/pcbnew/issue25065/issue25065.kicad_sch` |
| `netclass_flag` | 47 | many |
| `embedded_files` | 46 | many |
| `text_box` | 41 | many |
| `(arc ` | 37 | many |
| `(bus_alias` | 37 | legacy files (< fmt 20250925) |
| `(group ` | 34 | many |
| `rule_area` | 31 | `qa/data/pcbnew/component_classes_drc.kicad_sch`, `qa/data/pcbnew/issue25065/issue25065.kicad_sch`, `qa/data/pcbnew/length_calculations.kicad_sch` |
| `(image` | 31 | 31 files — base64 bitmap round-trip |
| `variant` | 133 (broad match) | `qa/data/eeschema/variants/`, `qa/data/cli/variants/`, `qa/data/eeschema/variant_field_resolution/` |
| `symbol_instances` (legacy) | 51 | older-version files — migration path |
| `(bezier` | 8 | `qa/data/eeschema/api_kitchen_sink.kicad_sch`, `demos/vme-wren/fp_connectors.kicad_sch` |
| `table` / `(column_count` | 6 | `qa/data/eeschema/api_kitchen_sink.kicad_sch`, `qa/data/eeschema/issue23840/BusAndVectors.kicad_sch`, `demos/jetson-agx-thor-baseboard/power.kicad_sch` |
| `jumper_pin_groups` | 4 | `qa/data/eeschema/issue23058/issue23058.kicad_sch`, `qa/data/eeschema/netlists/jumpers/jumpers.kicad_sch`, `api_kitchen_sink` |
| `body_styles` | 4 | `qa/data/eeschema/variants/pic_sockets.kicad_sch`, `qa/data/cli/variants/pic_sockets.kicad_sch`, `demos/pic_programmer/pic_sockets.kicad_sch`, `api_kitchen_sink` |
| `start_shape` / `end_shape` | 4 | `api_kitchen_sink` (schematic); `qa/data/pcbnew/line_ending_*` (PCB) |
| `directive_label` (the token, vs `netclass_flag`) | 2 | `qa/data/eeschema/issue24403_legacy_de.kicad_sch` — **the cross-language field-name regression file; parse this to validate untranslated field names** |
| `custom_property` | **1** | `qa/data/eeschema/api_kitchen_sink.kicad_sch` only |
| `ellipse` / `ellipse_arc` | **0** | **no fixture exists.** `qa/tests/eeschema/test_sch_ellipse_roundtrip.cpp` builds them programmatically. **A Rust implementation must hand-author an ellipse fixture.** |

### 10.4 Demo projects worth using as realistic corpora

All under `demos/` (116 `.kicad_sch` total). Ordered by usefulness:

| Project | Sheets | Why |
|---|---|---|
| `demos/vme-wren/` | 36 | the largest hierarchy in the tree; big FPGA symbols, heavy bus usage, its own `.kicad_sym` with beziers |
| `demos/jetson-agx-thor-baseboard/` | 17 | modern, uses tables; realistic scale |
| `demos/video/` | 8 | classic complex hierarchy, multiple sheet instances of the same screen |
| `demos/cm5_minima/` | 8 | modern; buses |
| `demos/royalblue54L_feather/` | 5 | nested sub-project (`nfc_antenna/`), tests relative sheet paths |
| `demos/complex_hierarchy/` | 2 | the canonical complex-hierarchy test (same screen at multiple paths) |
| `demos/multichannel/` | 2 | repeated-channel hierarchy |
| `demos/pic_programmer/` | 2 | `pic_sockets.kicad_sch` has `body_styles` (DeMorgan) |
| `demos/simulation/**` | ~20 | spice model fields, `exclude_from_sim`, `.kicad_sym` with simulation fields |
| `demos/kit-dev-coldfire-xilinx_5213/` | 3 | older-format survivors |
| `demos/sonde xilinx/` | 1 | **a path with a space in it** — good filesystem edge case |
| `demos/ecc83/` | 2 | `ecc83-pp.kicad_sch` + `ecc83-pp_v2.kicad_sch` — two format generations of the same design |

### 10.5 `.kicad_sym` fixtures (21 under `qa/data`)

| Path | Notes |
|---|---|
| `qa/data/libraries/Device.kicad_sym` | the standard Device library subset |
| `qa/data/libraries/power.kicad_sym` | power symbols (global + local power flags) |
| `qa/data/libraries/test_project/Device.kicad_sym` | project-local copy |
| `qa/data/eeschema/libs/4xxx.kicad_sym` | multi-unit logic; DeMorgan body styles |
| `qa/data/cli/sym_lib_test/Amplifier_Video.v9.kicad_sym` | **explicit older format version** — migration test |
| `qa/data/fuzz/kicad_sym/TMS320LF2406PZ.kicad_sym` | **fuzz corpus seed** — large pin count |
| `qa/data/eeschema/issue22286/bugtest.kicad_sym` | pin alternates regression |
| `qa/data/eeschema/issue19646/Library.kicad_sym` | — |
| `qa/data/eeschema/remote_symbol_lib.kicad_sym` | remote/HTTP library payload |
| `qa/data/eeschema/variants/pic_programmer.kicad_sym`, `qa/data/cli/variants/pic_programmer.kicad_sym` | variant-capable symbols |
| `qa/data/eeschema/variant_field_resolution/variant_test_lib.kicad_sym` | variant field resolution |
| `qa/data/diff_merge/visual_diff/sym_a.kicad_sym`, `sym_b.kicad_sym` | minimal pair — good first parser targets |
| `qa/data/eeschema/spice_netlists/**/*.kicad_sym` (6 files) | simulation fields |
| `qa/data/eeschema/netlists/test_hier_no_connect/TEST_LIB.kicad_sym` | — |
| `demos/vme-wren/vme-wren.kicad_sym` | **contains beziers** — rare in symbol bodies |

### 10.6 Suggested render-test tiers

1. **Tier 0 (smoke):** `demos/ecc83/ecc83-pp_v2.kicad_sch` — small, one sheet, covers wires,
   junctions, labels, symbols. Load headlessly, record a `DRAW_STREAM`, assert it is non-empty
   and contains the expected opcodes.
2. **Tier 1 (opcode coverage):** `qa/data/eeschema/api_kitchen_sink.kicad_sch` — should
   exercise nearly every `kgds_op`. Any opcode never emitted across this file is either
   unreachable from eeschema or a gap in `RECORDING_GAL`.
3. **Tier 2 (golden streams):** serialise the stream for Tier 0/1 plus
   `demos/complex_hierarchy/`, `demos/pic_programmer/pic_sockets.kicad_sch` (DeMorgan body
   styles) and `qa/data/eeschema/libs/4xxx.kicad_sym` (multi-unit). Commit and diff.
4. **Tier 3 (scale / caching):** `demos/vme-wren/` (36 sheets), `demos/video/` (8 sheets with
   the same screen at multiple paths). These are the workloads that exercise
   `BeginGroup`/`DrawGroup`/`DeleteGroup` churn and `DRAW_STREAM::Compact()`.
5. **Tier 4 (visual differential):** compare wgpu output against
   `kicad-cli sch export svg` and the `test_symbol_svg_export.cpp` references.

---

## Appendix A — `.kicad_sch` grammar (reference only)

> **NOT PART OF THIS CHANGE.** The decided cut (Option 1) keeps all file I/O in C++; the
> `.kicad_sch` format is never reimplemented in Rust. This appendix was written before the
> scope was narrowed. It is preserved because it is accurate and will be useful if a later
> step ever needs a second reader/writer, and because it documents conventions (internal
> units, Y-axis direction, angle encoding) that §9 also depends on. **A reviewer of the UI
> change can skip it.**

**Normative. File format version `20260830`.**

### A.0 Lexical and layout conventions

**Encoding:** UTF-8, no BOM.

**Tokens:** `(`, `)`, bare symbols (`[A-Za-z0-9_+\-.*/$~{}\[\]#%&]`…), integers, decimals, and
double-quoted strings. The lexer is generated from `eeschema/schematic.keywords` into
`SCHEMATIC_LEXER` / enum `TSCHEMATIC_T::T_*`.

**String escapes** (`OUTPUTFORMATTER::Quotew` / `Quotes`): a value is quoted whenever it is
not a safe bare symbol. Inside quotes, `"` → `\"`, `\` → `\\`, newline → `\n`, tab → `\t`.
KiCad also uses `~{...}` for overbar and `${VAR}` for text variables — both are *content*, not
lexical syntax.

Note the version gate at `sch_io_kicad_sexpr_parser.cpp:3222`:
`SetKnowsBar( m_requiredVersion >= 20240620 )` — before v20240620 `|` was a valid bare-symbol
character.

**Bare-flag idiom:** several elements carry an optional un-parenthesised keyword immediately
after the head token, e.g. `(property private "Name" "Value" …)`, `(arc private …)`,
`(polyline private …)`, `(text private …)`, `(rectangle private …)`. The writer emits
`""` (nothing, plus a space) when the flag is absent — so `(arc  (start …)` with a double
space is normal output.

**Booleans:** `KICAD_FORMAT::FormatBool` (`common/io/kicad/kicad_io_utils.cpp:39`) emits
`(key yes)` / `(key no)`. `FormatOptBool` may emit `(key none)`.

**UUIDs:** `KICAD_FORMAT::FormatUuid` (`:53`) emits `(uuid "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx")`
— **always quoted**, lower-case RFC-4122 textual form. The nil UUID
(`00000000-0000-0000-0000-000000000000`) is suppressed in shape helpers.

**Custom properties:** `KICAD_FORMAT::FormatCustomProperties` (`:59`) emits, for any item
with custom props, zero or more `(custom_property "key" "value")` *as the last children*
before the closing paren. Added at fmt v20260830. Applies to virtually every schematic item.

**Numbers — coordinates and lengths.** `EDA_UNIT_UTILS::FormatInternalUnits`
(`common/eda_units.cpp:190`) with `schIUScale`:

```
engUnits = value_in_IU / IU_PER_MM          // IU_PER_MM = 1e4
if engUnits != 0 and |engUnits| <= 0.0001:
    s = format("{:.10f}", engUnits); strip trailing '0's; strip trailing '.'
else:
    s = format("{:.10g}", engUnits)
```
A `VECTOR2I` is `"<x> <y>"` (`:224`).

> **Internal units — CONFIRMED and IMPORTANT:**
> `include/base_units.h:70` `constexpr double SCH_IU_PER_MM = 1e4;  ///< Schematic internal units 1=100nm.`
> `include/base_units.h:123` `constexpr EDA_IU_SCALE schIUScale = EDA_IU_SCALE( SCH_IU_PER_MM );`
> **Schematic IU = 100 nanometres, i.e. 10 000 IU per mm — NOT 1 nm.**
> (Pcbnew is different: `PCB_IU_PER_MM = 1e6`, 1 IU = 1 nm, `base_units.h:68`.)
> Derived: `IU_PER_MILS = 1e4 * 0.0254 = 254` IU per mil (`base_units.h:83`).
> The classic 50 mil grid = 12700 IU = 1.27 mm.
> **The on-disk file is in millimetres**, as a `%.10g` decimal. Rust must therefore store
> `i32` IU and convert on write/read, not store floats.

**Angles.** `EDA_UNIT_UTILS::FormatAngle` (`common/eda_units.cpp:182`) → `fmt::format("{:.10g}", degrees)`.
One legacy exception: `.kicad_sym` `(text …)` writes **tenths of a degree as an integer**
(`sch_io_kicad_sexpr_lib_cache.cpp:820`, `aText->GetTextAngle().AsTenthsOfADegree()`).

**Colours.** Always 4 values: `r g b a` where `r,g,b` are `KiROUND(component*255.0)` ints
`0..255` and `a` is `FormatDouble2Str(alpha)` (a double). Absent ⇒ `COLOR4D::UNSPECIFIED`.

**Layout.** `PRETTIFIED_FILE_OUTPUTFORMATTER` reflows the output: tab indentation, one
sub-expression per line, `(pts (xy …) (xy …))` kept on one line. Writers emitting a
semantically identical but differently-laid-out file will **not** byte-match. To byte-match,
reimplement the prettifier (see `common/io/kicad/` + `OUTPUTFORMATTER`). Example of real
output (`qa/data/eeschema/api_kitchen_sink.kicad_sch:1-6`):

```
(kicad_sch
	(version 20260830)
	(generator "eeschema")
	(generator_version "10.99")
	(uuid "d6ee7e6a-6570-43fd-93e6-b98f9cea3e37")
	(paper "A4")
```

**Y axis.** In the **schematic** file, **+Y is DOWN** (screen convention); coordinates are
written verbatim (`formatIU(pt, aInvertY=false)`). In **`.kicad_sym` and inside the
`(lib_symbols)` block**, **+Y is UP**: every write negates Y (`aInvertY = true`, e.g.
`sch_io_kicad_sexpr_lib_cache.cpp:684-706` passes `true`; `savePin` writes `-aPin->GetPosition().y`
at `:772`; `saveField` writes `-aField->GetPosition().y` at `:745`). **A Rust implementation
must apply the same Y negation when crossing the symbol/schematic boundary.**
`SCH_IO_KICAD_SEXPR::saveShape` (`sch_io_kicad_sexpr.cpp:1456`) passes `aInvertY = false`
for schematic-level shapes; `SCH_IO_KICAD_SEXPR_LIB_CACHE::saveSymbolDrawItem`
(`:665`) passes `true` for symbol-body shapes.

### A.1 Shared sub-grammars

#### stroke

`STROKE_PARAMS::Format` (`common/stroke_params.cpp:460`):
```
(stroke (width <mm>) (type <line-style>))
(stroke (width <mm>) (type <line-style>) (color <r> <g> <b> <a>))   ; only if colour != UNSPECIFIED
```
`<line-style>` ∈ `solid | dash | dot | dash_dot | dash_dot_dot | default`
(`common/stroke_params.cpp:417-432`). `width 0` means "use the default pen width".
Parser: `STROKE_PARAMS_PARSER::ParseStroke` (`:483`); note it reads width as
`KiROUND(double * m_iuPerMM)`.

#### fill

`formatFill` (`sch_io_kicad_sexpr_common.cpp:32`):
```
(fill (type <fill-type>))
(fill (type <fill-type>) (color <r> <g> <b> <a>))   ; when type ∈ {color, hatch, reverse_hatch, cross_hatch}
```
`<fill-type>` ∈ `none | outline | background | color | hatch | reverse_hatch | cross_hatch`
mapping to `FILL_T::{NO_FILL, FILLED_SHAPE, FILLED_WITH_BG_BODYCOLOR, FILLED_WITH_COLOR,
HATCH, REVERSE_HATCH, CROSS_HATCH}`.

#### effects (text)

`EDA_TEXT::Format` (`common/eda_text.cpp:1063`). Children are emitted **only when
non-default**:
```
(effects
  (font
    [(face "<font name>")]                  ; only if a font is set and named
    (size <height-mm> <width-mm>)           ; ALWAYS emitted; note height first, then width
    [(line_spacing <double>)]               ; only if != 1.0
    [(thickness <mm>)]                      ; only if !GetAutoThickness()
    [(bold yes)]                            ; only if bold
    [(italic yes)]                          ; only if italic
    [(color <r> <g> <b> <a>)]               ; unless CTL_OMIT_COLOR or UNSPECIFIED
  )
  [(justify [left|right] [top|bottom] [mirror])]   ; only if non-centre or mirrored
  [(href "<url>")]                                 ; unless CTL_OMIT_HYPERLINK
)
```
Rules:
- `(size H W)` — **height is first**. `GetTextHeight()` then `GetTextWidth()` (`:1073`).
- `(justify …)` is emitted only if `IsMirrored() || halign != CENTER || valign != CENTER`
  (`:1102`). Within it, horizontal comes first (`left` or `right`; centre emits nothing),
  then vertical (`top` or `bottom`; centre emits nothing), then `mirror`. All three are
  **bare keywords**, space-separated, e.g. `(justify left bottom)`.
- **As of fmt v20260826, `bold` is a stroke-width multiplier and `thickness` stores the base
  width** (`sch_file_versions.h`). Do not derive thickness from bold.
- `(hide yes)` is **not** inside `effects` for fields — it sits directly in the `(property …)`
  body (see below). For pins it is also outside `(name …)` / `(number …)`.

#### line endings (fmt v20260818)

`LINE_ENDING::Format` (`common/line_ending.cpp:299`), token is `start_shape` or `end_shape`.
Omitted entirely when style is `NONE`:
```
(start_shape <style> [(length <mm>)] [(width <mm>)] [(stroke (width <mm>) (type <line-style>))])
(end_shape   <style> …)
```
`<style>` ∈ `arrow | circle | square | arrow_open` (`LINE_ENDING_STYLE`,
`include/line_ending.h:43`; `NONE` is never written). `length`/`width`/stroke-width are
emitted only when `> 0`.
Semantics (`include/line_ending.h:56-60`): closed arrows are **tip-anchored** at the endpoint
and extend back along the line; open arrows are **vertex-anchored** and do not shorten the
line body; circles and squares are **centred** on the endpoint.
Applies to: `(arc)`, `(bezier)`, `(polyline)`, and schematic `(polyline)`-as-line when the
layer is `LAYER_NOTES` (`sch_io_kicad_sexpr.cpp:1560`) — **not** to `(wire)` or `(bus)`.

#### shape primitives (shared by schematic and symbol)

All from `sch_io_kicad_sexpr_common.cpp`. `<P>` denotes the optional bare `private` flag
(symbol bodies only). `<uuid?>` and `(locked yes)` are emitted only in the schematic context.

```
(arc <P> (start <x> <y>) (mid <x> <y>) (end <x> <y>)
     <stroke> [<start_shape>] [<end_shape>] <fill> [(uuid "…")] [(locked yes)])      ; :229

(circle <P> (center <x> <y>) (radius <mm>)
     <stroke> <fill> [(uuid "…")] [(locked yes)])                                     ; :253

(rectangle <P> (start <x> <y>) (end <x> <y>) [(radius <mm>)]
     <stroke> <fill> [(uuid "…")] [(locked yes)])                                     ; :274
     ; (radius …) only when corner radius > 0  (rounded rects, fmt v20250829)

(bezier <P> (pts (xy <x> <y>) (xy <x> <y>) (xy <x> <y>) (xy <x> <y>))
     <stroke> [<start_shape>] [<end_shape>] <fill> [(uuid "…")] [(locked yes)])       ; :299
     ; EXACTLY 4 points: start, control1, control2, end

(polyline <P> (pts (xy <x> <y>) …)
     <stroke> [<start_shape>] [<end_shape>] <fill> [(uuid "…")] [(locked yes)])       ; :327
     ; points are Outline(0) of the SHAPE_POLY_SET; a closed polygon repeats nothing —
     ; closure is implied by the fill.  fmt v20260803 normalised the closing edge.

(ellipse <P> (center <x> <y>) (major_radius <mm>) (minor_radius <mm>) (rotation_angle <deg>)
     <stroke> <fill> [(uuid "…")] [(locked yes)])                                     ; :361  (fmt v20260508)

(ellipse_arc <P> (center <x> <y>) (major_radius <mm>) (minor_radius <mm>)
     (rotation_angle <deg>) (start_angle <deg>) (end_angle <deg>)
     <stroke> <fill> [(uuid "…")] [(locked yes)])                                     ; :382
```

Arc representation is **start / mid / end**, not centre+angles. Convert with the circumcircle
of the three points; `SCH_PAINTER` does exactly that via `SHAPE_ARC(start, mid, end, 0)`
(`sch_painter.cpp:1951`).

#### property (field)

Schematic-level: `SCH_IO_KICAD_SEXPR::saveField` (`sch_io_kicad_sexpr.cpp:1125`):
```
(property [private] "<name>" "<value>" (at <x> <y> <angle>)
    [(hide yes)]                 ; only when !IsVisible()
    (show_name <yes|no>)         ; ALWAYS
    (do_not_autoplace <yes|no>)  ; ALWAYS ; note the inversion: !CanAutoplace()
    [<effects>]                  ; only if !IsDefaultFormatting() OR height != 50 mil
    [(custom_property "k" "v")…]
)
```
Order is exactly: bare `private` flag, name, value, `at`, `hide`, `show_name`,
`do_not_autoplace`, `effects`, custom properties. (fmt v20251028 "Updated properties
formatting (do_not_autoplace, show_name)"; fmt v20241209 added `private`.)

The **name written is the untranslated name** — `SCH_FIELD::GetUntranslatedName()`
(`sch_io_kicad_sexpr.cpp:1131`, deliberately not `GetName()`; see issue #24403).
Mandatory names (`common/template_fieldnames.cpp:31-38`, "do not change without transitioning
the file format"):

| `FIELD_T` | on-disk name |
|---|---|
| `REFERENCE` | `Reference` |
| `VALUE` | `Value` |
| `FOOTPRINT` | `Footprint` |
| `DATASHEET` | `Datasheet` |
| `DESCRIPTION` | `Description` |
| `SHEET_NAME` | `Sheetname` |
| `SHEET_FILENAME` | `Sheetfile` |
| `INTERSHEET_REFS` | `Intersheetrefs` |
| user field *n* | `Field<n>` |

Directive-label netclass field is named `Netclass` (`eeschema/sch_field.cpp:1242`).

Reserved `ki_*` user field names written by the symbol library writer: `ki_keywords`,
`ki_fp_filters` (space-separated, each filter escaped with `ESCAPE_CONTEXT::CTX_NO_SPACE`),
`ki_locked` (`sch_io_kicad_sexpr_lib_cache.cpp:483, 576-610`).

#### pin_map_override (fmt v20260629)

`formatPinMapOverride` (`sch_io_kicad_sexpr.cpp:778`). Omitted entirely when default.
```
(pin_map_override (mode <library_default|named_map|identity|delegate>)
    [(map "<map name>")]          ; only when mode = named_map and name non-empty
    [(edit "<pin number>" "<pad number>")…]
)
```

### A.2 Top level

`SCH_IO_KICAD_SEXPR::Format( SCH_SHEET* )` — `sch_io_kicad_sexpr.cpp:442`.
Parser dispatch: `SCH_IO_KICAD_SEXPR_PARSER::ParseSchematic` — `…_parser.cpp:3183`.

```
(kicad_sch
  (version <int>)                 ; 20260830
  (generator "eeschema")
  (generator_version "<major.minor>")   ; e.g. "10.99"
  (uuid "<uuid>")                 ; the SCREEN uuid; for the root sheet also the root path uuid
  <paper>
  [<title_block>]
  (lib_symbols <lib-symbol>…)     ; ALWAYS emitted, possibly empty
  <item>…                         ; in canonical order (see below)
  [<net_chain>…]                  ; only written by the first top-level sheet
  [<sheet_instances>]             ; only when this sheet HasRootInstance()
  [(embedded_fonts <yes|no>)]     ; only from the first top-level sheet
  [<embedded_files>]              ; only from the first top-level sheet, if non-empty
)
```

**Item ordering is canonical and must be reproduced for byte-match**
(`sch_io_kicad_sexpr.cpp:478-487`): items are put into a `std::multiset` sorted by
`(KICAD_T type, then m_Uuid)`. `SCH_MARKER_T` items are **never saved**. The `KICAD_T`
enumeration order therefore determines element order in the file; within a type, UUID
lexicographic order.

`(lib_symbols)` entries iterate `screen->GetLibSymbols()`, a `std::map<wxString, LIB_SYMBOL*>`
— i.e. **sorted by lib-id string**.

**Parser accepts (superset of what the writer emits), `…_parser.cpp:3251-3560`:**
`group`, `generator`, `host` (legacy), `generator_version`, `uuid`, `paper`, `page`
(legacy — v≤20200506 `page` is rewritten to `paper` at `:3227`; also a modern
top-level-sniffing `(page "…" "…")`), `title_block`, `lib_symbols`, `symbol`, `image`,
`sheet`, `junction`, `no_connect`, `bus_entry`, `polyline`, `bus`, `wire`, `arc`, `circle`,
`rectangle`, `bezier`, `ellipse`, `ellipse_arc`, `rule_area`, `netclass_flag` (7.0-dev
alias), `text`, `label`, `global_label`, `hierarchical_label`, `directive_label`,
`net_chain`, `text_box`, `table`, `sheet_instances`, `symbol_instances` (legacy),
`bus_alias`, `embedded_fonts`, `embedded_files`.

**Version gate:** if `(version)` > `SEXPR_SCHEMATIC_FILE_VERSION` the parser throws
`FUTURE_FORMAT_ERROR` (`:3204`). Before v20231120 there is no `generator_version`, so the
check fires on the version alone (`:3230`).

**Format changelog** (from `sch_file_versions.h`, most recent first — use this as the
migration table):

| Version | Change |
|---|---|
| 20260830 | Custom user properties (`(custom_property …)`) |
| 20260826 | Bold is a stroke-width multiplier; `thickness` stores the base width |
| 20260818 | Line ending shapes (`start_shape` / `end_shape`) |
| 20260803 | Schematic polygons with normalized closing edge |
| 20260722 | Dedicated variant `symbol_override` token |
| 20260629 | Pin-to-pad maps |
| 20260623 | Migrate reference-image scale for PNG pixel-density fix |
| 20260622 | Escaped special chars in stacked-pin notation |
| 20260512 | Net chains |
| 20260508 | Native ellipse primitive |
| 20260326 | Locking properties (`(locked yes)`) |
| 20260306 | Variant `in_bom` semantics corrected |
| 20260101 | PCB variants |
| 20251028 | Updated properties formatting (`do_not_autoplace`, `show_name`) |
| 20251012 | Flat schematic hierarchy support |
| 20250922 | Schematic variants |
| 20250901 | Stacked-pin notation |
| 20250829 | Rounded rectangles |
| 20250827 | Custom body styles |
| 20250610 | DNP etc. flags for rule areas |
| 20250513 | Groups can have design-block `lib_id` |
| 20250425 | uuids for tables |
| 20250318 | `~` no longer means empty text |
| 20250227 | Local power symbols |
| 20250222 | Hatched fills for shapes |
| 20250114 | Full paths for text-variable cross references |
| 20241209 | Private flags for `SCH_FIELD`s |
| 20241004 | Booleans for `hide` in symbols |
| 20240819 | Embedded files — Murmur3 hash |
| 20240812 | Netclass colour highlighting |
| 20240716 | Multiple netclass assignments |
| 20240620 | Embedded files (also: `|` no longer a bare-symbol char) |
| 20240602 | Sheet attributes |
| 20240417 | Rule areas |
| 20240101 | Tables |
| 20231120 | `generator_version`; V8 cleanups |
| 20230819 | Multiple library-symbol inheritance depth |
| 20230808 | `Sim.Enable` field → `exclude_from_sim` attr |
| 20230620 | `ki_description` → `Description` field |
| 20230409 | `exclude_from_sim` markup |
| 20230221 | Modern power symbols (editable value = net) |
| 20230121 | `SCH_MARKER` sheet-path serialisation; also the image-PPI cutover (see `saveBitmap`) |
| 20221206 | Simulation model fields V6 → V7 |
| 20221126 | Remove value/footprint from instance data |
| 20221110 | Sheet instance data → sheet definition |
| 20221004 / 20221002 | Instance data back into symbol definition |
| 20220929 | Don't save property ID |
| 20220914 | DNP support |
| 20220904 | `do_not_autoplace` |
| 20220903 | Field name visibility |
| 20220822 | Hyperlinks in text objects |
| 20220820 | Fix broken default symbol instance data |
| 20220622 | New simulation model format |
| 20220404 | Default schematic symbol instance data |
| 20220331 | Text colors |
| 20220328 | Text box `start/end` → `at/size` |
| 20220126 | Text boxes |
| 20220124 | `netclass_flag` → `directive_label` |
| 20220104 | Fonts |
| 20220103 | Label fields |
| 20220102 | Dash-dot-dot |
| 20220101 | Circles, arcs, rects, polys & beziers |
| 20211123 | uuids for junctions |
| 20210621 / 20210615 / 20210606 | Overbar syntax `~...~` → `~{...}` (bus aliases / net names / text) |
| 20210406 | Schematic-level uuids |
| 20210126 / 20210125 | uuids for pins, labels, wires… |
| 20210123 | `unconnected` pintype → `no_connect` |
| 20201015 | Sheet instance properties |
| 20200828 | Footprint in `symbol_instances` (below this, `SetLegacySymbolInstanceData()`) |
| 20200827 | Remove `host` tag |
| 20200714 | Alternate pin definitions |
| 20200618 | Disallow duplicate field ids |
| 20200608 | Bus and junction properties |
| 20200602 | Exclude from board |
| 20200512 | Exclude from BOM |
| 20200506 | Used `page` instead of `paper` |
| 20200310 | Initial version |

#### paper

`PAGE_INFO::Format` (`common/page_info.cpp:234`):
```
(paper "<type>")                          ; standard size
(paper "<type>" portrait)                 ; standard size, portrait
(paper "User" <width-mm> <height-mm>)     ; custom
```
`<type>` is `magic_enum::enum_name(PAGE_SIZE_TYPE)` — `A0 A1 A2 A3 A4 A5 A B C D E
USLetter USLegal USLedger GERBER User` (check `common/page_info.cpp` `standardPageSizes`
for the authoritative list and the mils dimensions). Custom dimensions are written as
`FormatDouble2Str(mils * 25.4 / 1000.0)` — i.e. **millimetres**, page size being stored
internally in mils. `portrait` is emitted only for non-custom pages.

#### title_block

`TITLE_BLOCK::Format` (`common/title_block.cpp:28`). **The whole block is omitted if every
field is empty.** Each child is omitted when empty:
```
(title_block
  [(title "…")] [(date "…")] [(rev "…")] [(company "…")]
  [(comment 1 "…")] … [(comment 9 "…")]      ; index is 1-based
)
```

### A.3 Schematic items

#### junction — `saveJunction`, `sch_io_kicad_sexpr.cpp:1375`
```
(junction (at <x> <y>) (diameter <mm>) (color <r> <g> <b> <a>)
    (uuid "…") [(locked yes)] [(custom_property …)…])
```
`diameter 0` ⇒ use the schematic default (`DEFAULT_JUNCTION_DIAM` = 36 mil). `color`, `diameter`
and `uuid` are always written.

#### no_connect — `saveNoConnect`, `:1401`
```
(no_connect (at <x> <y>) (uuid "…") [(locked yes)] [(custom_property …)…])
```

#### bus_entry — `saveBusEntry`, `:1421`
```
(bus_entry (at <x> <y>) (size <dx> <dy>) <stroke> (uuid "…") [(locked yes)] [(custom_property …)…])
```
**Critical asymmetry:** only `SCH_BUS_WIRE_ENTRY` is written as `bus_entry`.
A **`SCH_BUS_BUS_ENTRY` is converted to a `(bus …)` line segment on save** (`:1427-1435`) —
the writer constructs a temporary `SCH_LINE(pos, LAYER_BUS)` with the entry's end point and
calls `saveLine`. A Rust writer must do the same or it will produce files KiCad reads
differently.

#### wire / bus / polyline (as line) — `saveLine`, `:1528`
```
(wire     (pts (xy <x1> <y1>) (xy <x2> <y2>)) <stroke> (uuid "…") [(locked yes)] […])
(bus      (pts (xy <x1> <y1>) (xy <x2> <y2>)) <stroke> (uuid "…") [(locked yes)] […])
(polyline (pts (xy <x1> <y1>) (xy <x2> <y2>)) <stroke>
          [<start_shape>] [<end_shape>] (uuid "…") [(locked yes)] […])
```
Layer → token (`:1536-1543`): `LAYER_BUS` → `bus`, `LAYER_WIRE` → `wire`,
`LAYER_NOTES` → `polyline`. **Wires and buses are always exactly two points.**
`start_shape`/`end_shape` are written **only** for `LAYER_NOTES` (`:1558-1563`).

**On read** (`…_parser.cpp:3389-3439`), `(polyline …)` is ambiguous: `parseSchPolyLine()`
returns a `SCH_SHAPE`; if it has **exactly 2 points it is downgraded to a `SCH_LINE` on
LAYER_NOTES** (preserving stroke, locked, both endings and the UUID); if it has **> 2 points
it stays a `SCH_SHAPE` polygon**; **< 2 points is a parse error** ("Schematic polyline has
too few points").

#### image (`SCH_BITMAP`) — `saveBitmap`, `:1163`
```
(image (at <x> <y>) [(scale <g-format double>)] (uuid "…") [(locked yes)]
    (data "<base64 line>" "<base64 line>" …)
    [(custom_property …)…])
```
`(scale …)` is emitted only when `!= 1.0`, using `fmt::format("(scale {:g})", …)`.
`(data …)` is `KICAD_FORMAT::FormatStreamData` (`common/io/kicad/kicad_io_utils.cpp:69`):
base64 of the raw image bytes, wrapped at **76 characters** per quoted chunk.
Note the v≤20230121 PPI compatibility branch at `:1187-1190` (scale × 300 / PPI) and
fmt v20260623's PNG pixel-density migration.

#### polyline / rectangle / circle / arc / bezier / ellipse / ellipse_arc (`SCH_SHAPE`)
`saveShape`, `:1456` — dispatches to the §A.1 shape primitives with `aIsPrivate = false`,
`aInvertY = false`, and passes the item UUID and `locked` (suppressed when the shape is
inside a `rule_area`, `:1462`).

#### rule_area (fmt v20240417) — `saveRuleArea`, `:1507`
```
(rule_area
    [(locked yes)]
    (exclude_from_sim <yes|no>) (in_bom <yes|no>) (on_board <yes|no>) (dnp <yes|no>)
    <shape>                      ; one nested polyline/rect/etc (locked suppressed inside)
    [(custom_property …)…])
```
The four booleans are **always** written (fmt v20250610).

#### text / label / global_label / hierarchical_label / netclass_flag — `saveText`, `:1574`

Head token from `getTextTypeToken` (`sch_io_kicad_sexpr_common.cpp:200`):
`SCH_TEXT_T` → `text`, `SCH_LABEL_T` → `label`, `SCH_GLOBAL_LABEL_T` → `global_label`,
`SCH_HIER_LABEL_T` → `hierarchical_label`, `SCH_DIRECTIVE_LABEL_T` → **`netclass_flag`**
(despite the v20220124 note — the *writer* emits `netclass_flag`; the parser accepts both
`netclass_flag` and `directive_label`).

```
(<type> "<text>"
    [(exclude_from_sim <yes|no>)]   ; SCH_TEXT_T only
    [(length <mm>)]                 ; netclass_flag only — the directive pin length
    [(shape <shape-token>)]         ; global_label | hierarchical_label | netclass_flag
    (at <x> <y> <angle>)
    [(fields_autoplaced yes)]       ; labels with fields, when AUTOPLACE_AUTO or _MANUAL
    <effects>
    (uuid "…")
    [(locked yes)]
    [<property>…]                   ; label fields (fmt v20220103)
    [(custom_property …)…])
```

**Angle encoding — important.** `saveText` (`:1605-1618`) writes
`angle = GetTextAngle()`, then for labels adds 180° when the spin style is `LEFT` or
`BOTTOM`. Text angle on disk is only ever 0 or 90 (readability); item rotation of 180/270 is
recovered from the spin style. Spin styles (`SPIN_STYLE`, `eeschema/sch_label.cpp:113-155`):
`LEFT=0, UP=1, RIGHT=2, BOTTOM=3`.

`<shape-token>` from `getSheetPinShapeToken` (`sch_io_kicad_sexpr_common.cpp:168`):
`input | output | bidirectional | tri_state | passive` for `L_*`, and
`dot | round | diamond | rectangle` for the `F_*` (directive-flag) shapes.
Note `L_UNSPECIFIED` → **`passive`**.

#### text_box / table_cell — `saveTextBox`, `:1652`
```
(text_box "<text>"
    (exclude_from_sim <yes|no>)
    (at <x> <y> <angle>) (size <dx> <dy>) (margins <left> <top> <right> <bottom>)
    [<stroke>]                    ; NOT written for table_cell
    <fill> <effects> (uuid "…") [(locked yes)] [(custom_property …)…])

(table_cell "<text>"
    (exclude_from_sim <yes|no>)
    (at <x> <y> <angle>) (size <dx> <dy>) (margins <l> <t> <r> <b>)
    (span <colspan> <rowspan>)    ; table_cell only
    <fill> <effects> (uuid "…") [(locked yes)] […])
```
`at` is `GetStart()`; `size` is `GetEnd() - GetStart()`.

#### table (fmt v20240101) — `saveTable`, `:1694`
```
(table (column_count <int>)
    (border (external <yes|no>) (header <yes|no>) [<stroke>])
        ; stroke only if external OR header
    (separators (rows <yes|no>) (cols <yes|no>) [<stroke>])
        ; stroke only if rows OR cols
    (column_widths <mm> <mm> …)
    (row_heights   <mm> <mm> …)
    (uuid "…")                    ; fmt v20250425
    [(locked yes)]
    (cells <table_cell>…)
    [(custom_property …)…])
```
Cells are emitted in `GetCells()` order (row-major).

#### symbol (instance) — `saveSymbol`, `:808`
```
(symbol
    [(lib_name "<screen-local symbol name>")]   ; only when !UseLibIdLookup()
    (lib_id "<Lib:Name>")
    (at <x> <y> <angle>)                        ; angle ∈ {0,90,180,270}
    [(mirror <x?> <y?>)]                        ; bare flags; e.g. (mirror x) / (mirror  y) / (mirror x y)
    (unit <int>)
    (body_style <int>)
    (exclude_from_sim <yes|no>)
    (in_bom <yes|no>)            ; NOTE: written as !GetExcludedFromBOM()
    (on_board <yes|no>)          ; !GetExcludedFromBoard()
    (in_pos_files <yes|no>)      ; !GetExcludedFromPosFiles()
    (dnp <yes|no>)
    [<pin_map_override>]
    [(passthrough <mode>)]       ; lower-cased magic_enum name; omitted when DEFAULT
    [(locked yes)]
    [(fields_autoplaced yes)]
    (uuid "…")
    <property>…                  ; in GetFields(ordered) order
    (pin "<number>" (uuid "…") [(alternate "<alt name>")])…
    [(instances (project "<name>" (path "<KIID_PATH>" (reference "<ref>") (unit <int>)
                                       [<variant>…])… )… )]
    [(custom_property …)…])
```

Details that matter:
- **Orientation split** (`:826-843`): `GetOrientation()` is a bitmask; the rotation part
  (`SYM_ORIENT_0/90/180/270`) becomes the `at` angle, the mirror bits (`SYM_MIRROR_X`,
  `SYM_MIRROR_Y`) become `(mirror …)`. `(mirror x)`/`(mirror y)`/`(mirror x y)` — the writer
  emits `""` for an unset axis, so `(mirror  y)` (double space) is valid output.
- **`unit` is the *ordinal* instance's unit, not the current sheet's** (`:864-895`) — to
  avoid file churn. Resolved via `Hierarchy().GetOrdinalPath(parentScreen)`.
- **`Reference` field text is temporarily swapped** to the ordinal instance's reference while
  the field is written, then restored (`:930-965`).
- **Pin UUIDs:** every raw pin gets a `(pin "<num>" (uuid …))`. An `(alternate "<name>")` is
  written only when the alt is non-empty **and differs from the base name** (`:968-985`).
- **Instances are grouped by project** (`:987-1100`): keyed on the first KIID of the
  normalized path. Within a project, instances are sorted by `KIID_PATH`. Instance paths that
  belong to this project but have no live sheet are **dropped** (orphan pruning) unless
  writing for the clipboard.
- **Variants** (fmt v20250922 / v20260101 / v20260306 / v20260722), `:1030-1085`:
```
(variant (name "<variant>")
    [(dnp <yes|no>)] [(exclude_from_sim <yes|no>)] [(in_bom <yes|no>)]
    [(on_board <yes|no>)] [(in_pos_files <yes|no>)]
    [(field (name "<n>") (value "<v>"))…]
    [(symbol_override "<Lib:Name>")]
    [<pin_map_override>]
)
```
  Each boolean is written **only when it differs from the symbol's own value**. A variant with
  no differentials is skipped entirely.

#### sheet — `saveSheet`, `:1205`
```
(sheet (at <x> <y>) (size <w> <h>)
    (exclude_from_sim <yes|no>) (in_bom <yes|no>) (on_board <yes|no>) (dnp <yes|no>)
    [(locked yes)]
    [(fields_autoplaced yes)]
    <stroke>                       ; border width + colour
    (fill (color <r> <g> <b> <a>)) ; NOTE: no (type …) — always a bare colour fill
    (uuid "…")
    <property>…                    ; includes the mandatory "Sheetname" and "Sheetfile"
    (pin "<name>" <shape-token> (at <x> <y> <angle>) (uuid "…") <effects>)…
    [(instances (project "<name>" (path "<KIID_PATH>" (page "<page number>")
                                       [<variant>…])… )… )]
    [(custom_property …)…])
```
- Sheet-pin `<shape-token>` uses the same `getSheetPinShapeToken` table.
- Sheet-pin angle comes from `getSheetPinAngle(SHEET_SIDE)`
  (`sch_io_kicad_sexpr_common.cpp:189`): `LEFT`/`UNDEFINED` → 180°, `RIGHT` → 0°,
  `TOP` → 90°, `BOTTOM` → 270°.
- Sheet-pin text is written with `EscapedUTF8()`, not `Quotew()` (`:1250`).
- Sheet `(fill …)` is **not** the generic fill grammar — it is always `(fill (color r g b a))`
  (`:1237`).
- Sheet instances with an empty path are dropped (`:1270-1277`). The `(project …)` grouping
  is emitted inline as the list is walked (`:1287-1367`), and entries whose path belongs to
  this project but has no live sheet are skipped.
- Sheet variants carry only `dnp`, `exclude_from_sim`, `in_bom` and `field` (`:1324-1352`).

#### sheet_instances — `saveInstances`, `:1838`
```
(sheet_instances (path "<KIID_PATH>" (page "<page number>"))…)
```
Written only when the sheet `HasRootInstance()` and only for the root instance (`:1620` in
`Format`). An empty path is written as `"/"`.

#### symbol_instances (legacy)
Not written by the current writer. **Parsed** (`…_parser.cpp:3495`,
`parseSchSymbolInstances`) for files ≤ fmt 20221004:
```
(symbol_instances (path "<KIID_PATH>" (reference "<ref>") (unit <int>) [(value …)] [(footprint …)])…)
```
For `m_requiredVersion < 20200828`, `screen->SetLegacySymbolInstanceData()` is called at the
end of `ParseSchematic` (`…_parser.cpp:3604`).

#### bus_alias — `parseBusAlias`, `…_parser.cpp:5976`
```
(bus_alias "<alias name>" (members "<net>" "<net>" …))
```
Parsed but **no longer written into `.kicad_sch`** — fmt v20250925 moved bus aliases to the
project file. Files at older versions still contain them; a Rust reader must accept them.
For `m_requiredVersion < 20210621` the alias name is passed through
`ConvertToNewOverbarNotation()`.

#### net_chain (fmt v20260512) — `sch_io_kicad_sexpr.cpp:1560` (inside `Format`)
```
(net_chain "<name>"
    (from "<terminal ref>" "<pin number>")
    (to   "<terminal ref>" "<pin number>")
    [(net_class "<name>")]
    [(color <r> <g> <b> <a>)]
    [(nets "<net>" "<net>" …)])
```
Written **only from the schematic's first top-level sheet** (`:1561`). Chains with an empty
`from`/`to` terminal ref are skipped. Only nets passing `SCH_NETCHAIN::IsPersistableNet()`
are listed (synthetic subgraph names are unstable and excluded).

#### group (fmt v20250513) — `saveGroup`, `:1803`
```
(group "<name>" (uuid "…") [(locked yes)] [(lib_id "<DesignBlockLib:Name>")]
    (members "<uuid>" "<uuid>" …)
    [(custom_property …)…])
```
**Empty groups are not written** (`:1805`). Member UUIDs are **sorted** as strings (`:1827`).
Note the `lib_id` here is printed with a raw `"%s"` and manual quotes, not `Quotew`.

#### embedded_fonts / embedded_files (fmt v20240620 / v20240819)
```
(embedded_fonts <yes|no>)
(embedded_files (file (name "<name>") (type <type>) (data …) (checksum "<murmur3>"))…)
```
Both are written **only from the schematic's first top-level sheet** (`:1626-1633`).
Implementation is `EMBEDDED_FILES::WriteEmbeddedFiles` / `EMBEDDED_FILES_PARSER::ParseEmbedded`
in `common/embedded_files.cpp` — read that file for the exact `(file …)` grammar and the
Murmur3 checksum. The parser is fault-tolerant: a `PARSE_ERROR` inside `embedded_files` is
recorded as a warning and the block is skipped by depth counting (`…_parser.cpp:3536-3554`).

---

## Appendix B — `.kicad_sym` grammar (reference only)

> **NOT PART OF THIS CHANGE.** Same status as Appendix A.

**Normative. File format version `20260830`.**
Writer: `eeschema/sch_io/kicad_sexpr/sch_io_kicad_sexpr_lib_cache.cpp`.
Parser: `SCH_IO_KICAD_SEXPR_PARSER::ParseLib` / `parseLibSymbol`, `…_parser.cpp:236+`.

> **Y-axis reminder:** everything below is written with **Y negated** relative to the
> in-memory model (`aInvertY = true`). In the file, **+Y is UP**.

### B.1 File header and layout

`formatLibraryHeader` (`…_lib_cache.cpp:943`):
```
(kicad_symbol_lib (version 20260830) (generator "kicad_symbol_editor") (generator_version "<maj.min>")
  <symbol>…
)
```
Symbols are ordered by **inheritance depth first, then name** (`Save`, `:236-247`) — a parent
must precede its children.

A `.kicad_sym` "library" may also be a **directory** of `.kicad_sym` files
(`Load`, `:56-205`); each file is parsed with `ParseLib` into a shared map, with source-file
tracking for per-symbol saves.

Inside a `.kicad_sch`, the `(lib_symbols …)` block contains the **same `(symbol …)` grammar**,
emitted by the same `SCH_IO_KICAD_SEXPR_LIB_CACHE::SaveSymbol` (called from
`sch_io_kicad_sexpr.cpp:473`), except the symbol name is the **full lib-id string**
(`"Device:Q_NPN"`) rather than the bare item name. Derived symbols are not allowed in the
schematic cache (`…_parser.cpp:3335` uses a dummy map).

### B.2 Root symbol

`SaveSymbol`, `…_lib_cache.cpp:366`:

```
(symbol "<name>"
    [(power global)|(power local)]          ; :397-400 ; local power = fmt v20250227
    [(body_styles demorgan)|(body_styles "<name>" "<name>" …)]   ; :406-422 ; fmt v20250827
    [(pin_numbers (hide yes))]              ; only when !GetShowPinNumbers()
    [(pin_names [(offset <mm>)] [(hide yes)])]
        ; emitted only when offset != DEFAULT_PIN_NAME_OFFSET (20 mil) OR !GetShowPinNames()
        ; (offset …) only when it differs from the default
    (exclude_from_sim <yes|no>)
    (in_bom <yes|no>)            ; !GetExcludedFromBOM()
    (on_board <yes|no>)          ; !GetExcludedFromBoard()
    (in_pos_files <yes|no>)      ; !GetExcludedFromPosFiles()
    (duplicate_pin_numbers_are_jumpers <yes|no>)
    [(jumper_pin_groups ( "<pad>" "<pad>" … ) ( … ) …)]          ; fmt v20250324
    <property>…                  ; GetFields(ordered) order
    [<property ki_locked>]       ; a USER field literally named "ki_locked", when UnitsLocked()
    [<property ki_keywords>]     ; when raw keywords non-empty
    [<property ki_fp_filters>]   ; space-joined, each escaped CTX_NO_SPACE
    [<associated_footprints>]    ; fmt v20260629
    [<pin_maps>]                 ; fmt v20260629
    (symbol "<name>_<unit>_<bodyStyle>"
        [(unit_name "<display name>")]
        <draw-item>…
    )…
    (embedded_fonts <yes|no>)
    [<embedded_files>]           ; only when EmbeddedFileMap() non-empty
)
```

**Unit sub-symbol naming** (`:494-500`): the child token is the symbol's *item* name with
`_<unit>_<bodyStyle>` appended, produced by taking `Quotes(unitName)`, popping the trailing
quote, and printing `"(symbol %s_%d_%d\""`. Unit 0 means "common to all units"; body style 0
means "common to all body styles"; body style 1 is the base, 2 is DeMorgan.

**Draw-item ordering** (`:513-525`): a `std::multiset` sorted by `SCH_ITEM::operator<`.

TODO markers in the writer that a Rust implementation should be aware of but must NOT emit:
`uuid`, anchor position, `atomic`, `required` (`:404, 406, 487, 489`).

### B.3 Derived (extends) symbol

`…_lib_cache.cpp:533-568`:
```
(symbol "<name>" (extends "<parent name>")
    <property>…
    [<property ki_keywords>] [<property ki_fp_filters>]
    [<associated_footprints>] [<pin_maps>]
    (embedded_fonts <yes|no>)
    [<embedded_files>]
)
```
No unit sub-symbols, no pins, no graphics — all inherited. Multi-level inheritance is allowed
(fmt v20230819).

### B.4 property (library field)

`…_lib_cache.cpp:734`:
```
(property [private] "<name>" "<value>" (at <x> <-y> <angle-degrees>)
    (show_name <yes|no>)
    (do_not_autoplace <yes|no>)
    [(hide yes)]
    <effects>
    [(custom_property …)…])
```
**Ordering differs from the schematic-level `property`**: here `show_name` and
`do_not_autoplace` come *before* `hide`, and `<effects>` is **always** written
(`aField->Format(&aFormatter, 0)` at `:759`, unconditional). Angle uses
`fmt::format("{:g}", degrees)` (`:745`), not `FormatAngle`. Y is negated.

### B.5 pin

`savePin`, `…_lib_cache.cpp:764`:
```
(pin <electrical-type> <graphic-style> (at <x> <-y> <angle>) (length <mm>)
    [(hide yes)]
    (name "<name>" (effects (font (size <h> <w>))))
    (number "<number>" (effects (font (size <h> <w>))))
    [(alternate "<alt name>" <electrical-type> <graphic-style>)…]
    [(custom_property …)…])
```
- Both `<electrical-type>` and `<graphic-style>` are **bare tokens**, in that order,
  immediately after `pin`.
- `(name …)` and `(number …)` **always** carry an `(effects (font (size H W)))` with H=W=the
  respective text size — deliberately, "follows the EDA_TEXT effects formatting for future
  expansion" (`:781`).
- `(hide yes)` only when the pin is invisible.
- Alternates with an empty name are silently dropped (`:797-802`).
- Angle from `getPinAngle(PIN_ORIENTATION)` (`sch_io_kicad_sexpr_common.cpp:156`):
  `PIN_RIGHT` → 0°, `PIN_LEFT` → 180°, `PIN_UP` → 90°, `PIN_DOWN` → 270°.
  **The angle is the direction the pin's *tail* points away from the body.**

**Electrical types** (`getPinElectricalTypeToken`, `sch_io_kicad_sexpr_common.cpp:68`):

| `ELECTRICAL_PINTYPE` | token |
|---|---|
| `PT_INPUT` | `input` |
| `PT_OUTPUT` | `output` |
| `PT_BIDI` | `bidirectional` |
| `PT_TRISTATE` | `tri_state` |
| `PT_PASSIVE` | `passive` |
| `PT_NIC` | `free` |
| `PT_UNSPECIFIED` | `unspecified` |
| `PT_POWER_IN` | `power_in` |
| `PT_POWER_OUT` | `power_out` |
| `PT_OPENCOLLECTOR` | `open_collector` |
| `PT_OPENEMITTER` | `open_emitter` |
| `PT_NC` | `no_connect` (was `unconnected` before fmt 20210123) |

**Graphic styles** (`getPinShapeToken`, `:113`):

| `GRAPHIC_PINSHAPE` | token |
|---|---|
| `LINE` | `line` |
| `INVERTED` | `inverted` |
| `CLOCK` | `clock` |
| `INVERTED_CLOCK` | `inverted_clock` |
| `INPUT_LOW` | `input_low` |
| `CLOCK_LOW` | `clock_low` |
| `OUTPUT_LOW` | `output_low` |
| `FALLING_EDGE_CLOCK` | **`edge_clock_high`** (note the mismatch) |
| `NONLOGIC` | `non_logic` |

**Stacked pins** (fmt v20250901, escaping v20260622): pin *numbers* may use a stacked
notation; see `qa/tests/eeschema/test_stacked_pin_nomenclature.cpp` and
`test_stacked_pin_conversion.cpp` and `eeschema/multiline_pin_text.cpp`.

### B.6 associated_footprints / pin_maps (fmt v20260629)

`savePinMapData`, `…_lib_cache.cpp:614`:
```
(associated_footprints
    (footprint "<Lib:Footprint>" [(map "<map name>")])…
)
(pin_maps
    (pin_map "<name>" (entry "<pin number>" "<pad number>")…)…
)
```
A derived symbol writes **nothing** here when it inherits the parent's maps, preserving
inheritance on round-trip (`:618-620`).

### B.7 Symbol-body draw items

`saveSymbolDrawItem`, `…_lib_cache.cpp:665`. Accepted types: `SCH_SHAPE_T`, `SCH_PIN_T`,
`SCH_TEXT_T`, `SCH_TEXTBOX_T`. Anything else → `UNIMPLEMENTED_FOR`.

Shapes use the §A.1 primitives with `aIsPrivate = shape->IsPrivate()`, `aInvertY = true`,
and **no uuid, no locked** (both default to nil/false at `:684-706`).

**text** (`saveText`, `:812`):
```
(text [private] "<text>" (at <x> <-y> <tenths-of-a-degree-int>) <effects> [(custom_property …)…])
```
**The angle here is an integer in tenths of a degree** — the one place in the format that is
not decimal degrees.

**text_box** (`saveTextBox`, `:829`):
```
(text_box [private] "<text>" (at <x> <-y> <angle>) (size <dx> <-dy>)
    (margins <l> <t> <r> <b>) <stroke> <fill> <effects> [(custom_property …)…])
```
Note **`size` has its Y negated too** (`:853`) — `-(GetEnd().y - GetStart().y)`.

### B.8 `.kicad_sym` version changelog

From `sch_file_versions.h` (most recent first): 20260830 custom user properties ·
20260826 bold as stroke-width multiplier · 20260710 line ending shapes ·
20260629 pin-to-pad maps · 20260622 escaped stacked-pin chars · 20260508 native ellipse ·
20251024 updated properties formatting · 20250925 bus alias moved to project file ·
20250901 stacked pin notation · 20250829 rounded rectangles · 20250324 jumper pin groups ·
20250318 `~` no longer empty text · 20241209 private SCH_FIELD flags ·
20240819 Murmur3 embedded-file hash · 20240529 embedded files ·
20231120 `generator_version` / V8 cleanups · 20230620 `ki_description` → Description ·
20220914 unit display names & don't-save-property-ID · 20220331 text colors ·
20220328 text box `start/end` → `at/size` · 20220126 text boxes · 20220102 fonts ·
20220101 class flags · 20211014 arc formatting · 20210619 overbar `~{}` ·
20201005 fp filters space-separated · 20200908 in-BOM/on-board · 20200827 remove host ·
20200126 initial (alternate pin definitions).

---

## Appendix C — Quick file index

| Concern | Primary file(s) |
|---|---|
| Format version constants | `eeschema/sch_file_versions.h` |
| Token list | `eeschema/schematic.keywords` (217 tokens) → generated `schematic_lexer.h` |
| `.kicad_sch` writer | `eeschema/sch_io/kicad_sexpr/sch_io_kicad_sexpr.cpp:426-1860` |
| `.kicad_sch` / `.kicad_sym` parser | `eeschema/sch_io/kicad_sexpr/sch_io_kicad_sexpr_parser.cpp` (`ParseSchematic` at `:3183`, `parseLibSymbol` at `:236`) |
| `.kicad_sym` writer | `eeschema/sch_io/kicad_sexpr/sch_io_kicad_sexpr_lib_cache.cpp:366-958` |
| Shared shape/enum formatters | `eeschema/sch_io/kicad_sexpr/sch_io_kicad_sexpr_common.cpp` |
| `FormatBool` / `FormatUuid` / `FormatCustomProperties` / `FormatStreamData` | `common/io/kicad/kicad_io_utils.cpp:39,53,59,69` |
| Number & angle formatting | `common/eda_units.cpp:182,190,224` |
| Internal-unit scale | `include/base_units.h:68,70,123` |
| Stroke grammar | `common/stroke_params.cpp:417,460,483` |
| Line endings | `include/line_ending.h:43,61`; `common/line_ending.cpp:299` |
| Text effects grammar | `common/eda_text.cpp:1063` |
| Paper / title block | `common/page_info.cpp:234`; `common/title_block.cpp:28` |
| Mandatory field names | `common/template_fieldnames.cpp:31-38,52-65`; `include/template_fieldnames.h:42-65` |
| Renderer | `eeschema/sch_painter.cpp` |
| Render settings & defaults | `eeschema/sch_render_settings.{h,cpp}`; `eeschema/default_values.h` |
| Layer ids | `include/layer_ids.h:470-532` |
| Layer draw order | `eeschema/sch_view.h:44-82`; `eeschema/sch_draw_panel.cpp:122-177` |
| Default colours | `common/settings/builtin_color_themes.h:28` (`s_defaultTheme`), `:317` (`s_classicTheme`) |
| Label geometry | `eeschema/sch_label.cpp:67-96` (templates), `:1126` (box expansion), `:2326` (global), `:2456` (hier) |
| Symbol transform | `libs/kimath/include/transform.h:41`; `eeschema/sch_symbol.cpp:3453-3505` |
| Connectivity engine docs | `eeschema/connectivity/conn_overview.h` |
| QA build glue | `cmake/KiCadQABuildUtils.cmake:52-77`; `qa/tests/eeschema/CMakeLists.txt` |
| Top-level dependency list | `CMakeLists.txt:777-1050`; `install-deps.sh` |
| **GAL interface** | `include/gal/graphics_abstraction_layer.h` (1,460 lines; class at `:110`) |
| **Recording backend (new)** | `include/gal/recording/{recording_gal.h, draw_stream.h, draw_stream_abi.h}`; `common/gal/recording/{recording_gal.cpp, draw_stream.cpp}` |
| Non-rasterising GAL prior art | `include/callback_gal.h:26`; `common/callback_gal.cpp:29` |
| Command-recording GAL prior art | `include/gal/cairo/cairo_gal.h:347-382` |
| Reference GL backend | `include/gal/opengl/opengl_gal.h` (60 overrides) |
| `VIEW` render loop | `common/view/view.cpp:1096` (`redrawRect`), `:1145` (`draw`), `:1278` (`Redraw`), `:1420` (`updateItemGeometry`), `:1586` (`UpdateItems`) |
| Frame paint loop | `common/draw_panel_gal.cpp:257` (`DoRePaint`) |
| Canvas ownership | `include/class_draw_panel_gal.h:343,346,349,352,359` |
| Glyph types | `include/font/glyph.h:40,59,99`; `common/font/font.cpp:240,435` |
| Input dispatcher | `include/tool/tool_dispatcher.h:49`; `common/tool/tool_dispatcher.cpp:219,422,548` |
| View controls abstraction | `include/view/view_controls.h`; `include/view/wx_view_controls.h:47` |
| Tool-holder contract | `include/tool/tools_holder.h` (`GetToolCanvas` pure virtual at `:165`) |
| Frame chain | `include/eda_base_frame.h:115,856-857`; `include/eda_draw_frame.h:81,458,617`; `eeschema/sch_edit_frame.h:138,1130` |
| Action registry | `include/tool/tool_action.h:299`; `include/tool/action_manager.h:139,194`; `common/tool/tool_action.cpp:52` |
| Action definitions | `include/tool/actions.h` (191); `eeschema/tools/sch_actions.h` (254), `sch_actions.cpp` |
| Hotkey persistence | `include/hotkeys_basic.h:53,64,103,111,116`; `common/hotkey_store.cpp:94,163,169,208` |
| Icons | `include/bitmaps/bitmaps_list.h:28`; `include/bitmaps/bitmap_info.h:32`; `common/bitmap_store.cpp:95,110,354`; `common/bitmap_info.cpp`; `resources/bitmaps_png/sources/{light,dark}/*.svg` (1,431 files); `resources/bitmaps_png/CMakeLists.txt:940-970` |
| Toolbar config | `include/tool/ui/toolbar_configuration.h:33-131`; `eeschema/toolbars_sch_editor.{h,cpp}` |
| Menus | `include/tool/action_menu.h:42` (`: public wxMenu`); `eeschema/menubar.cpp` |
| Build recipe | `docs/rust-migration/03-build-notes.md` |
