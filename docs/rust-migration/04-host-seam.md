# 04 — The host seam: `SCH_HOST`, the C ABI, and what is not done yet

> **Current planning:** [10-remaining-stages.md](10-remaining-stages.md) defines
> Stages 6–19 and their execution order. This document records an earlier survey,
> implementation milestone or reuse guidance; historical gaps and proposed
> approaches must be checked against the Stage 5 coverage and
> [completed Stage 6 verification](11-stage6-editing-correctness.md) before use.
> Stage 7 (real document sidebars) is next.


This document covers the C++ side of the boundary between eeschema's document
model and the Rust UI: what was built and what it is verified to do.

**If you are picking this up, read §9.** It is the editing seam — what a schematic
tool needs from whatever is editing the schematic, which of that is a window and
which is not — and it is what converting tool number five is against. §6 is the
older analysis of getting input *to* `TOOL_MANAGER`, kept as written because what it
got wrong is instructive.

Companion documents: `00-architecture-survey.md` (the reconnaissance this is
built on, especially §5–§8), `01-plan.md` (the architecture), `03-build-notes.md`
(how to build).

---

## 1. What was added

| Path | What it is |
|---|---|
| `eeschema/host/sch_host.h` / `.cpp` | `SCH_HOST` — a schematic editor session with no `wxFrame`; a `TOOLS_HOLDER`, a `SCHEMATIC_HOLDER` and an `UNDO_REDO_HOLDER` |
| `eeschema/host/sch_host_control.{h,cpp}` | `SCH_HOST_CONTROL` — undo, redo and save as *actions*, so a hotkey works |
| `eeschema/schematic_holder.h` / `.cpp` | Grown from upstream's four-virtual bridge into what a schematic tool needs from whatever is editing the schematic |
| `include/undo_redo_holder.h`, `common/undo_redo_holder.cpp` | `UNDO_REDO_HOLDER` — the undo and redo stacks, off `EDA_BASE_FRAME` |
| `eeschema/schematic_undo_redo.h` | `SCH_UNDO_REDO` — eeschema's undo, off `SCH_EDIT_FRAME` and shared with it |
| `include/view/host_view_controls.h`, `common/view/host_view_controls.cpp` | `HOST_VIEW_CONTROLS` — a `VIEW_CONTROLS` that is told where the pointer is |
| `include/tool/host_tool_dispatcher.h`, `common/tool/host_tool_dispatcher.cpp` | `HOST_TOOL_DISPATCHER` — host input to `TOOL_EVENT`, wx-free |
| `qa/tests/common/test_host_input.cpp` | QA coverage for both of those, with no eeschema and no GUI |
| `include/sch_host/sch_host_abi.h` | The plain-C ABI a Rust UI drives it through |
| `eeschema/host/sch_host_abi.cpp` | Its implementation, plus the action-registry export |
| `eeschema/host/sch_host_runtime.cpp` | The process singletons, and `ksch_runtime_init` |
| `eeschema/host/sch_host_abi.exports` / `.map` | The shared library's export list, for ld64 and ELF |
| `qa/tools/sch_dump/sch_dump.cpp` | `kicad-sch-dump`, the end-to-end proof and fixture generator |
| `qa/tests/eeschema/test_sch_host.cpp` | QA coverage for all of the above |
| `qa/data/draw_streams/` | Checked-in golden draw streams |

`sch_host.cpp` and `sch_host_abi.cpp` are compiled into
`eeschema_kiface_objects`, so anything that already links eeschema — the kiface,
`qa_eeschema`, `kicad-sch-dump` — gets them for free.

`sch_host_runtime.cpp` is the exception, and deliberately so: it *defines* the
process singletons, and a program that has its own must keep them. It is compiled
only into `libkicad_sch_host`, the shared library a non-C++ UI links
(`-DKICAD_BUILD_RUST_SCH_UI=ON`, off by default because it is a second full
eeschema link). The export list beside it keeps that library's surface to the 33
`ksch_*` symbols and nothing else — worth doing because the kiface objects bring
vendored C libraries whose symbols are not hidden by the tree's
`-fvisibility=hidden`, that flag being C++-only.

---

## 2. `SCH_HOST`

`SCH_EDIT_FRAME` owns a `SCHEMATIC`, a `SCH_SHEET_PATH`, an
`EDA_DRAW_PANEL_GAL` (which owns the `VIEW`, the `GAL`, the `PAINTER` and the
`VIEW_CONTROLS`), a `TOOL_MANAGER`, undo and redo stacks, about a dozen dialog
pointers, three docked panes, a file-system watcher, a SpaceMouse plugin and
202 public methods.

`SCH_HOST` owns the first three of those and nothing else:

```
SCH_HOST
  SCHEMATIC*                 the document, loaded through the existing reader
  SCH_SHEET_PATH             the current sheet
  KIGFX::SCH_VIEW            the real one, constructed with a null frame
  KIGFX::RECORDING_GAL       records instead of rasterising
  KIGFX::SCH_PAINTER         the real one, unmodified
  SCH_RENDER_SETTINGS        via the painter
```

**It is not a refactor of `SCH_EDIT_FRAME`.** Survey §7.3 measured ~600
`m_frame->` call sites in `eeschema/tools/`, of which ~470 are plain model or
settings access. Rerouting those is a large, mechanical, separately-landable
change. `SCH_HOST` is a new parallel owner that proves the rendering path works
before any of that is attempted, so that the refactor — when it happens — is
done against a known-good target rather than on faith.

### 2.1 Loading

`SCH_HOST::LoadFile()` calls `EESCHEMA_HELPERS::LoadSchematic()`, which is the
same entry point `kicad-cli` uses. No file-format code is duplicated:
`SCH_IO_KICAD_SEXPR` does the parsing, and symbol-link resolution, instance-data
migration for pre-2022 files, page numbering, junction repair and connectivity
all happen exactly as they do for any other headless consumer.

### 2.2 Two process globals the host must stand up, or it crashes

Both of these were found by running the tool on real files, and both are null
dereferences rather than errors, so they are worth stating plainly. `SCH_HOST`
handles each itself — a C ABI that segfaults because the embedder forgot an
undocumented global is not an ABI — but anyone embedding this differently needs
to know they exist.

**1. `Kiface().KifaceSettings()` must be live before anything draws.**
`SCH_PAINTER` reads `eeconfig()`, which is that pointer cast to
`EESCHEMA_SETTINGS`, and dereferences it with no null check at
`sch_painter.cpp:594` and six other sites. In the GUI the eeschema kiface module
installs it during `OnKifaceStart`. A process that never loaded that module — a
test binary, a CLI tool, a Rust host — has nothing to install it, and the first
piece of *text* drawn is a null dereference several frames deep inside KIFONT,
which reads as a font-subsystem failure and is not one. Since text is most of a
schematic's geometry, this fires almost immediately.
`SCH_HOST::ensureKifaceSettings()` installs a fallback when the slot is empty.

**2. A schematic with no sibling `.kicad_pro` needs a project loaded first.**
`EESCHEMA_HELPERS::LoadSchematic` falls back to `SETTINGS_MANAGER::Prj()`, which
with nothing loaded returns a **static `PROJECT` whose `PROJECT_FILE` is null**
(`settings_manager.cpp:1225`). `SCHEMATIC::Settings()` then dereferences that
file unconditionally. `SCH_HOST::LoadFile()` loads the empty project when the
settings manager has none, which gives the fallback something real behind it.

Neither is reachable from the GUI, because a frame always has both. They are
purely artefacts of running the document model without one, which is exactly
what this seam does.

### 2.3 The canvas, and one deliberate difference from `SCH_DRAW_PANEL`

The GAL/view/painter wiring in `SCH_HOST::buildCanvas()` is copied from
`SCH_DRAW_PANEL`'s constructor, including `SetWorldUnitLength( SCH_WORLD_UNIT )`
(eeschema's internal unit is 100 nm, not the 1 nm the `GAL` base class assumes),
the `SCH_LAYER_ORDER` layer ordering and the per-layer render targets.

One thing is different on purpose. `SCH_DRAW_PANEL::setDefaultLayerDeps()` uses
`TARGET_CACHED` only under OpenGL and `TARGET_NONCACHED` otherwise, because
"caching makes no sense for Cairo and other software renderers". `SCH_HOST`
caches everything cacheable. Retained groups are the entire point of the
recording backend — they are what lets a pan re-upload nothing — and running
non-cached would mean the recorded stream never exercised
`BeginGroup`/`DrawGroup` at all, so the property most worth testing would go
untested.

### 2.4 Rendering

`SCH_HOST::Render()` follows `EDA_DRAW_PANEL_GAL::DoRePaint()`
(`common/draw_panel_gal.cpp:257`):

```
m_view->UpdateItems();      // opens/closes retained groups — outside the frame bracket
m_view->MarkDirty();
m_gal->BeginDrawing();
m_gal->SetClearColor/SetGridColor/SetCursorColor
m_gal->ClearScreen();
m_view->ClearTargets();
m_view->Redraw();           // layer walk, R-tree query, SCH_PAINTER per item
m_gal->EndDrawing();
return m_gal->Publish();    // a borrowed kgds_stream_view — no copy
```

with one difference: `DoRePaint` early-outs when the view is clean, and
`Render()` does not. The caller asked for a frame, and damage tracking belongs to
the renderer on the far side of the boundary, which knows what it has already
drawn.

`SetDepthRange` (survey §5.4 hazard 3) needs no attention here: `GAL`'s own
constructor already sets it to `[MIN_DEPTH, MAX_DEPTH]`.

### 2.4.1 The viewport is load-bearing, and its scale is not VIEW's

`VIEW::Redraw()` builds its cull rectangle from the GAL's screen size and
world-screen matrix and queries the R-tree with it, so **the camera decides which
groups the frame body references**. That is what makes a live consumer's
per-frame `SetViewport` more than bookkeeping: the geometry is recorded in world
units and does not depend on the camera, but which of it is *drawn* does.

The scale that goes in is pixels per internal unit, which is deliberately not what
`KIGFX::VIEW` calls a scale. VIEW's is the GAL zoom factor, and

```
GAL::computeWorldScale():  worldScale = screenDPI * worldUnitLength * zoomFactor
eeschema's worldUnitLength = 1e-7 / 0.0254 inch per IU     (SCH_WORLD_UNIT)
```

so the two differ by about three orders of magnitude.
`SCH_HOST::PixelsPerIUAtUnitZoom()` converts, and reads the factor out of the GAL
rather than recomputing that formula, so the user's zoom-correction factor comes
along without this code knowing it exists.

Passing the ABI's scale straight to `VIEW::SetScale` was the bug this replaced,
and it was invisible for as long as nothing depended on the camera. Every request
came out ~2,700× too large, `VIEW::SetScale` clamped it to eeschema's zoom limit,
and the resulting cull rectangle was 52 metres wide — nothing was ever culled, and
`ZoomToFit` did not fit. See `06-what-is-missing.md`, Stage 2, for how it surfaced
and why fixing it changed no recorded output.

One consequence a consumer has to handle: `VIEW::SetScale` clamps to eeschema's
own zoom limits — the same ones the wx editor is bound by — so
`ksch_session_get_viewport` does not always report what was asked for. Read it
back and adopt it. A consumer showing a wider view than the session believes in
would find the geometry outside the session's viewport missing from the frame.

One hook is deliberately not wired. `SCH_VIEW::SetScale()` calls
`m_frame->RefreshZoomDependentItems()`, which is a no-op with a null frame. That
costs nothing today, because the method only re-paints *selected* items — the
bitmap-text LOD threshold and the scaled selection shadow — and `SCH_HOST` has no
selection. It becomes relevant the moment selection arrives, and is listed in
§6.4 for that reason.

### 2.5 Teardown order

`SCH_VIEW` registers an invalidation listener on the schematic's
`TEXT_VAR_TRACKER` and holds a `DS_PROXY_VIEW_ITEM` built from the screen's page
settings. Both must be released before the `SCHEMATIC` is freed, so
`SCH_HOST::Unload()` runs `SCH_VIEW::Cleanup()` and `DetachTextVarTracker()`,
then `ClearCache()` on the GAL, then `SetProject( nullptr )` on the schematic —
which is what `EESCHEMA_JOBS_HANDLER::ClearCachedSchematic()` does, for the same
reason — and only then deletes it.

---

## 3. The C ABI

`include/sch_host/sch_host_abi.h`, in the same spirit as
`draw_stream_abi.h`: plain C, POD structs, an opaque handle, no C++ type across
the boundary, `extern "C"`, and documented ownership on every pointer.

It is no longer merely `bindgen`-clean: `rust/crates/kicad-sch-sys` generates the
bindings in its build script on every build, so the header cannot drift from what
Rust believes it says. bindgen's compile-time layout assertions come along with
that, and they are the useful part — the two sides agreeing about every struct:

| Struct | Bytes |
|---|---|
| `kgds_cmd` | 24 |
| `kgds_group` | 16 |
| `kgds_image` | 32 |
| `kgds_stream_view` | 136 |
| `kgds_file_header` | 80 |
| `ksch_bbox` | 32 |
| `ksch_viewport` | 32 |
| `ksch_document_info` | 24 |
| `ksch_sheet_info` | 32 |
| `ksch_action` | 88 |
| `ksch_input_event` | 56 |
| `ksch_editor_state` | 48 |

### 3.0 The runtime, and why the ABI grew one

`KSCH_ABI_VERSION` is 5; this section describes what version 2 added, §3.4 what
version 3 did, and §3.5 version 4. Version 5 adds the typed dialog services
described in `09-dialog-workflows.md`; property descriptors and ERC marker IDs
change the ABI, so both host and Rust client must be rebuilt together. Version 2's addition is three calls — `ksch_runtime_init`,
`ksch_runtime_shutdown`, `ksch_runtime_is_ready` — and they exist because
everything else in this header is uncallable without them.

KiCad's document model reaches for two process singletons that a GUI build gets
from `main()` and a loaded kiface: a `PGM_BASE`, which owns the settings manager,
and a `KIFACE_BASE`, whose `KifaceSettings()` `SCH_PAINTER` dereferences with no
null check (§2.2). §4.2 called that out and offered `kicad-sch-dump`'s `main()` as
the worked example — which is fine for a C++ tool and useless to a Rust binary,
which has no C++ `main()` to put it in. So the twenty lines moved behind the ABI:
wx in console mode, the settings manager, eeschema's settings registered and
loaded, the kiface's settings installed.

It is idempotent, and it adopts rather than replaces: if the process already
installed a `PGM_BASE` — `qa_eeschema` and `kicad-sch-dump` both do — it changes
nothing and reports success, because clobbering a live singleton is worse than
doing nothing. `ksch_session_create` now fails with a message naming
`ksch_runtime_init` when no `PGM_BASE` is standing, rather than letting the first
load dereference null several frames deep.

`ksch_session_load_file` also holds a `LOCALE_IO` now. `kicad-sch-dump` held one
for its whole run, which meant a caller that did not know to do that would
misread every coordinate in a file under a decimal-comma locale. That is not a
thing an ABI should leave to its callers.

### 3.1 Error handling

Every entry point runs its body inside a `guard()` template that catches
`IO_ERROR`, `std::bad_alloc`, `std::exception` and `...`, records the message on
the session and returns a `ksch_status`. Nothing unwinds into C. This is not
defensive decoration: `SCH_IO_KICAD_SEXPR` throws `IO_ERROR` on any malformed
input, a Rust caller has no handler for it, and the process would abort.

Null handles and null out-parameters are `KSCH_ERR_INVALID_ARG`, never a
dereference. On any error the caller's out-parameter is left untouched.

### 3.2 String lifetimes

Two lifetimes, stated once in the header and honoured everywhere:

* Session-scoped — the error string, the three strings in `ksch_sheet_info` and
  the two in `ksch_editor_state`. Valid until the next call on that session. The
  UTF-8 conversions are parked on the session struct rather than on a temporary,
  which is the only reason those pointers are safe to return at all.
* Process-scoped — everything in `ksch_action`, and the global error string.

### 3.3 Coordinates

`double`, in KiCad internal units, matching the draw stream. A bounding box and
the geometry in a frame are therefore in the same space and need no conversion
on the Rust side.

`ksch_viewport::scale` follows the same rule and is pixels per internal unit, for
the same reason: it has to compose with a camera held over those coordinates. It
is not `KIGFX::VIEW`'s scale — see §2.4.1, which is also where the bug that
conflated them is recorded.

**Input positions are the one exception, and deliberately so.**
`ksch_input_event::x` / `::y` are **screen pixels from the top-left of the
canvas**, because the session has to derive the world position itself: it does so
through the same `KIGFX::VIEW_CONTROLS` a tool reads the cursor back from, so a
cursor a tool has forced or placed is the one the following events carry. A caller
handing world coordinates in would bypass that, and the tools would disagree with
the view about where the cursor is. `TOOL_DISPATCHER` takes exactly the same route
for exactly the same reason (`common/tool/tool_dispatcher.cpp:615`); it is not a
concession to the ABI's shape.

### 3.4 What version 3 added: input, actions and editor state

Four entry points, bringing the total to 30:

| | |
|---|---|
| `ksch_session_dispatch_input` | one `ksch_input_event` to `TOOL_MANAGER`, via `HOST_TOOL_DISPATCHER` |
| `ksch_session_reset_input` | forget which buttons are down, for a UI that lost focus |
| `ksch_session_run_action` | a registered action by its dotted name, as a menu does |
| `ksch_session_editor_state` | the cursor the tools see, the selection size, the tool name, the status text |

Three vocabularies are reconciled inside `toHostInput()`
(`eeschema/host/sch_host_abi.cpp`) and nowhere else: the ABI's button *ordinals*
become KiCad's `BUT_*` bits, the ABI's modifier bits become `MD_*` bits, and the
key's **name** becomes a `WXK_*` code through
`HOST_TOOL_DISPATCHER::KeyCodeFromName`.

The last two are the decisions worth arguing with, and they land the same way.

**Keys.** The obvious design is to send a `WXK_*` integer, and both this document
and `06-what-is-missing.md` used to say to transcribe `wx/defs.h` into a Rust
constant table. Don't: a transcribed table is one wrong entry per silently broken
shortcut, and nothing in the build would ever notice. Sending the name instead means
the numbers come out of `wx/defs.h` through a compiler, on the side of the boundary
that already includes it. `rust/` contains no `WXK_` constant at all.

**Modifiers, which is the same argument with a sharper edge.** On macOS KiCad's
`MD_CTRL` is **Command**: `wx/defs.h` defines `wxMOD_CMD == wxMOD_CONTROL` there,
physical Control arrives as `wxMOD_RAW_CONTROL`, and `decodeModifiers` does not look
at it. So every `.DefaultHotkey( MD_CTRL + 'Z' )` in the tree means ⌘Z on macOS and
nothing at all for ⌃Z. A UI that reported its modifiers already translated — "this
is the Ctrl modifier" — would invert that, and silently, because an unmatched hotkey
is indistinguishable from no hotkey. The ABI therefore carries the *physical* keys
(::ksch_modifier says so) and `toHostInput()` decides what they mean.

The two result flags a caller gets back matter more than they look:

* `KSCH_INPUT_HANDLED` — a tool or a hotkey claimed it. Note that this is *not*
  what `TOOL_MANAGER::RunAction( const std::string& )` reports: that overload
  discards `doRunAction()`'s result and answers "the name resolved", and
  `ACTION_MANAGER`'s constructor registers every `TOOL_ACTION` in the process — so
  it would call all ~440 of them handled on a host with no tools.
  `SCH_HOST::RunActionByName` looks the action up and uses the `TOOL_ACTION&`
  overload, whose result is `processEvent`'s.
* `KSCH_INPUT_REDRAW` — something called `TOOLS_HOLDER::RefreshCanvas()`, so the
  frame the caller is holding is stale. **This is the only notice a UI gets that
  the document or the view changed behind its back.** When this was written it was
  never set, because eeschema's tools said "the view changed" by calling
  `m_frame->GetCanvas()->ForceRefresh()`. Stage 4b routed those through
  `SCHEMATIC_HOLDER::ForceRefreshCanvas()`, which a frame implements as the
  synchronous repaint it always was and the host implements as this flag — so a
  selection, a move and an undo are now visible in a UI that has no idea a tool ran.

### 3.5 What version 4 added: undo, redo and save

Three entry points and two fields, bringing the total to 33:

| | |
|---|---|
| `ksch_session_undo` | undo the newest command; reports whether there was one |
| `ksch_session_redo` | the same, the other way |
| `ksch_session_save` | write every sheet back to the file it was loaded from |
| `ksch_editor_state::undo_count` / `::redo_count` | so a UI can grey out its menu items |

Two decisions worth arguing with.

**Save is deliberately narrower than the editor's.** It writes the `.kicad_sch`
files through `SCH_IO_KICAD_SEXPR` — the same writer — and stops. It does not write
the project file, the symbol library table, a backup archive or the embedded-file
cache, each of which is a decision about the *project* rather than about the
document, and each of which a UI that wants it should ask for separately. What comes
out reopens in KiCad; that is the property under test.

**Undo, redo and save are *also* tool actions, and that is not redundancy.** A
hotkey is resolved inside `TOOL_MANAGER`, so ⌘Z crosses this ABI as a key press and
a UI cannot intervene in what it means. If the only route were these three calls,
a menu item would work and the key would not. `SCH_HOST` therefore registers
`SCH_HOST_CONTROL`, whose three handlers delegate to the same shared code these
entry points do. The entry points remain for a caller that wants a status code
rather than a fire-and-forget action.

---

### 3.6 Host search data

`ksch_session_set_search_data` copies UTF-8 find/replacement strings and matching
options into `SCH_HOST`. `SCHEMATIC_HOLDER::GetHostSearchData()` exposes active
terms to `SCH_FIND_REPLACE_TOOL`; wx frames retain their existing dialog-owned
terms. Criteria changes restart navigation; replacement-text-only changes keep
the current match. Setting `active=0` clears highlighting and deactivates search.

The frontend runs the existing `common.Interactive.findNext`, `findPrevious`,
`replaceAndFindNext` and `replaceAll` actions, then calls
`ksch_session_search_result` for found/wrapped flags, replacement item count and
match-center coordinates in schematic internal units. Replacements use
`SCH_COMMIT` and the normal undo stack. The host clears cached matches before
undo, redo, item removal and document teardown.

Rust exposes these as `Session::set_search_data` and `Session::search_result`.
The GPUI shell has presentation-independent search data and operations on its
`InputSink`; the binary adapter maps them to the session. The canvas centers on
the returned match and requests a new host frame. Rebuild the C++ library and
Rust application together when using these additional ABI functions.

## 4. `kicad-sch-dump`

```
kicad-sch-dump [options] <file.kicad_sch>
  -o, --output <path>   stream file (default <input>.kgds)
  -W/-H <px>            viewport size (default 1920x1080)
  --sheet <n|all>       which sheet, by index into the page-ordered hierarchy
  --repeat <n>          record n frames; reports whether retained group data grew
  --no-write            statistics only
  --histogram           per-opcode command histogram
  --actions             print the action registry and exit
```

The tool drives the session **through its own C ABI**, not through `SCH_HOST`
directly. If the tool works, the ABI works, and there is no second code path to
keep in step.

### 4.1 Results on the whole corpus

Every `.kicad_sch` in the tree, root sheet only, 1920x1080 viewport, zoom to fit:

| | |
|---|---|
| Files recorded | **466 of 466**, no failures, no crashes |
| Items | 57,896 total, median 21, max 1,686 |
| Retained groups | 211,567 total, median 85, max 5,849 |
| Group commands | 1,669,381 total, median 714, max 43,695 |
| Coordinates | 12.4 M doubles total, median 7,062, max 326,872 |
| First-frame render | median 3.6 ms, max 93 ms |

Twenty-six files produce zero groups. All twenty-six have zero items: they are
blank root sheets whose children hold the content. They still emit ~400 frame
commands, which is the drawing sheet — it lives on `TARGET_NONCACHED` and so
records into the frame arena rather than into a group. That is correct, not a gap.

The heaviest sheets are `demos/jetson-agx-thor-baseboard/dcdc` (1,453 items,
5,849 groups, 93 ms) and `demos/tiny_tapeout/tinytapeout-demo` (43,695 group
commands, 326,872 coordinates). `demos/sonde xilinx/` — a path with a space in
it, which the survey flagged as a filesystem edge case — records fine.

Warm re-render is the number that matters for the 120 Hz target, because it is
what a pan costs. On `demos/video/video.kicad_sch` (363 items, 1,328 groups) the
first frame is 12.33 ms and the third is **1.75 ms**, with the retained group
arena byte-identical across all three. The geometry is recorded once; a pan
replays it.

### 4.2 Standing up the process

It supplies its own `PGM_BASE` and `KIFACE_BASE`, both minimal:

* `PGM_BASE` has exactly one pure virtual (`MacOpenFile`), so a standalone tool
  needs six lines to make `Pgm()` safe to dereference. Same approach as
  `qa/tools/drc_benchmark`.
* `eeschema_kiface_objects` references the global `Kiface()` but does not define
  it — `eeschema.cpp`, which does, is linked only into the kiface module. The
  loader touches `Kiface()` once, for `KifaceSettings()`, so a stub that has had
  `InitSettings()` called on it is sufficient.

This was the worked example for a Rust host library, and it has since been made
part of the ABI instead: `host/sch_host_runtime.cpp` does the same twenty lines
behind `ksch_runtime_init` (§3.0), because a Rust binary has no C++ `main()` to
put them in. This tool keeps its own — the runtime call detects that and leaves
them alone — which is why both code paths still exist.

---

## 5. The action registry

`ksch_action_count()` / `ksch_action_at()` / `ksch_action_find()` expose
KiCad's registry with no session, no document and no window, because
`TOOL_ACTION`'s constructor pushes each action onto
`ACTION_MANAGER::GetActionList()` — a function-local static — during static
initialisation, before `main()`.

Each entry carries the stable dotted name, the translated label, menu label,
tooltip and description, the tool name, the scope, the activation/notification
flags, the runtime and UI ids, and both default and current hotkeys as
`WXK_*`-OR-`MD_*` integers.

**Icons are exported as a name, not an enum.** `TOOL_ACTION::GetIcon()` returns
a `BITMAPS` enumerator, and the enumerator name *is* the SVG file name under
`resources/bitmaps_png/sources/{light,dark}/`. The enum carries no names at
runtime, but the generated table in `common/bitmap_info.cpp` does, indirectly:
for the light theme the CMake generator writes the file name as
`<name>[_<height>].png`, so stripping the extension and the height suffix
recovers the enumerator name exactly. Rust resolves
`sources/{light,dark}/<name>.svg` and renders it with `resvg` at the device
pixel ratio — vector, correct at any HiDPI factor, no `images.tar.gz`, no wx
virtual filesystem, and KiCad's own light/dark split preserved.

The QA suite asserts the identity holds, by taking the icon name a known action
reports and checking that the SVG exists at that path.

---

## 6. Feeding `TOOL_MANAGER` from Rust

> **This section was written when none of it was done, and is kept as written
> rather than rewritten, because what it got wrong is the useful part.** The
> current state, for a reader who wants that first:
>
> * **Done.** Input goes from a gpui window through the C ABI into
>   `TOOL_MANAGER::ProcessEvent`. `HOST_VIEW_CONTROLS` and `HOST_TOOL_DISPATCHER`
>   exist, `SCH_HOST` is a `TOOLS_HOLDER` that owns a `TOOL_MANAGER` and registers
>   eeschema's whole tool roster, and the ABI carries input, action dispatch and
>   editor state (§3.4). See `06-what-is-missing.md` Stage 4a.
> * **Also done, and this section did not see it coming.** Four tools now *receive*
>   the input: selection, move, wire and a small undo/redo/save control. What made
>   that possible is the subject of §7 below, and it is not on this section's list —
>   because the interface the tools needed already existed in the tree.
> * **Not done.** Seventeen tool classes still decline a holder that is not their
>   frame type, and every dialog is untouched. `06-what-is-missing.md` Stage 4b costs
>   each remaining tool.
> * **Wrong three times over below.** §6.1 calls `GetToolCanvas()` the lesser problem
>   and it is not a problem at all: it is still pure virtual and `SCH_HOST`
>   implements it in one line. §6.2's count and mechanism were wrong (Stage 3) *and*
>   its scope was: `common/`'s tools had the same hazard, crashed rather than
>   declined, and nothing in eeschema's cast list mentioned them. And §6.4's table
>   below is missing the row that turned out to matter — see §9.
>
> Stages 1 and 2 also removed the premise of the original opening: "the Rust UI
> cannot reach the document model" stopped being true before this was about input
> at all.

### 6.1 `GetToolCanvas()` is smaller than it looks

```cpp
virtual wxWindow* GetToolCanvas() const = 0;   // include/tool/tools_holder.h:165
```

27 mentions tree-wide. Of those:

* **1** is the pure-virtual declaration.
* **~12** are implementations. One of them, `EDA_DRAW_FRAME::GetToolCanvas()`
  (`include/eda_draw_frame.h:458`), covers every drawing frame in KiCad in a
  single line.
* The rest are consumers, and there are only six distinct ones:

| Site | What it does with the canvas |
|---|---|
| `common/tool/tool_dispatcher.cpp:593-596` | `HasFocus()` / `SetFocus()` |
| `common/dialog_shim.cpp:455,481` | `SetFocus()` when a dialog closes |
| `common/eda_base_frame.cpp:1331` | parent window for `WX_INFOBAR` |
| `pcbnew/pcb_edit_frame.cpp:327` | parent window for `WX_INFOBAR` |
| `3d-viewer/.../eda_3d_controller.cpp:95,97,139` | downcast to `EDA_3D_CANVAS` |
| `kicad/tools/kicad_manager_control.cpp:408` | downcast to `PROJECT_TREE_PANE` |

**None of the eeschema tool path is in that list.** Every eeschema consumer wants
focus or an info-bar parent, both of which a non-wx host routes elsewhere.

Better still, **three existing implementations already return `nullptr`** —
`SIMULATOR_FRAME` (`eeschema/sim/simulator_frame.h:200`), `MERGETOOL_FRAME`
(`kicad/mergetool_frame.h:72`) and a pcbnew test fixture
(`qa/tests/pcbnew/test_multichannel.cpp:58`). The contract already tolerates a
null canvas in production. `TOOL_DISPATCHER` even null-checks it before use.

So the fix is smaller than "a tree-wide edit": returning `nullptr` from a
non-wx holder is already a supported state, and the clean version — changing the
return type to an opaque `TOOL_CANVAS*` or dropping the method — touches ~12
one-line implementations and six call sites.

### 6.2 The real blocker: 14 unchecked downcasts from `TOOLS_HOLDER*`

> **Fixed. And the count below is wrong twice over — it is 16, and the casts were
> not the mechanism.** See `06-what-is-missing.md`, Stage 3. The short version:
> `TOOL_BASE::getEditFrame<T>()` (`include/tool/tool_base.h:182`) does the same
> `static_cast` for the whole tree and is how every tool's `m_frame` is set, so
> converting the list below would have left every tool holding a wild pointer
> anyway. What made it safe was checking at each tool's `Init()` and declining,
> which `TOOL_MANAGER::InitTools()` already knows how to handle. The consequence
> for §6.5 step 6 is that registering the eeschema tools on a non-frame holder now
> gives you *no tools*, visibly and testably, instead of memory corruption.
>
> **Wrong a third time, in scope.** "In eeschema" is doing a lot of work in the
> sentence below, and step 6 registers seven tool classes from `common/` as well:
> `COMMON_CONTROL`, `COMMON_TOOLS`, `ZOOM_TOOL`, `PICKER_TOOL`, `GROUP_TOOL`,
> `PROPERTIES_TOOL` and `EMBED_TOOL`. Every one had the same hazard, and because
> none of them declines, they *crash* rather than dropping out —
> `ZOOM_TOOL::Init()` calls a virtual through the wild pointer on its second line.
> All seven are fixed the same way; `06-what-is-missing.md` Stage 4a has the table
> and the reproduction.

This one the survey did not flag, and it is worse:

```cpp
// eeschema/sch_commit.cpp:51
SCH_BASE_FRAME* frame = static_cast<SCH_BASE_FRAME*>( m_toolMgr->GetToolHolder() );
```

`TOOL_MANAGER::GetToolHolder()` returns `TOOLS_HOLDER*`. `SCH_BASE_FRAME`
inherits `TOOLS_HOLDER` through `EDA_BASE_FRAME`, alongside `wxFrame` and
`KIWAY_HOLDER`, so the cast involves a non-trivial pointer adjustment **and**
assumes the holder really is a frame. Point a `TOOL_MANAGER` at a `SCH_HOST`
that is not one, and these are undefined behaviour — a wild `vtable` lookup on
the first virtual call, not a null check away.

There were **14** such `static_cast`s in eeschema found this way (plus 5
`dynamic_cast`s, which are fine — they yield null):

* `eeschema/sch_commit.cpp` — 6, at `:51`, `:163`, `:195`, `:606`, `:624`, `:637`
* `eeschema/tools/sch_editor_control.cpp` — 6, at `:1065`, `:1220`, `:1576`,
  `:1602`, `:1654`, `:1771`
* `eeschema/tools/symbol_editor_control.cpp` — 2, at `:974`, `:983`

Two more were missed here and in `06-what-is-missing.md`, at
`eeschema/tools/sch_selection_tool.cpp:191` and `:271`, for a true total of 16 —
which is the lesson about enumerating call sites by grep and treating the result
as a specification.

`SCH_COMMIT` is the one that matters most, because **every edit goes through
it**. Its `TOOL_MANAGER*` constructor downcasts the holder on line 51 before it
has done anything else.

Note that before the fix this was latent rather than live:
`EESCHEMA_HELPERS::LoadSchematic` constructs a `TOOL_MANAGER` and calls
`SetEnvironment( schematic, nullptr, nullptr, KifaceSettings(), nullptr )` —
a **null** holder — so `frame && ...` short-circuits and the headless CLI path was
always safe. Installing a non-null, non-frame holder is what would have turned it
into UB, and that is exactly what the next milestone must do, which is why these
sites were converted first, as their own commit, before a single `TOOL_EVENT` is
sent.

### 6.3 Undo/redo lives on `wxFrame`

`UNDO_REDO_CONTAINER m_undoList` / `m_redoList` are members of
`EDA_BASE_FRAME` (`include/eda_base_frame.h:856-857`), with
`PushCommandToUndoList()` (`:588`) and `GetUndoCommandCount()` (`:607`) as
virtuals on it. The data is `PICKED_ITEMS_LIST`-based and entirely wx-free; it
simply lives on a `wxFrame` subclass. `SCH_COMMIT::Push()` calls into it.

Hoisting the containers into a small owner that both `EDA_BASE_FRAME` and
`SCH_HOST` hold is mechanical, but it is a real edit to a base class every KiCad
program inherits, so it wants its own commit and its own review.

### 6.4 What else is needed

| Piece | State | Work |
|---|---|---|
| `VIEW_CONTROLS` | **abstract already** (`include/view/view_controls.h`) | Implement 8 pure virtuals in a `HOST_VIEW_CONTROLS`. `WarpMouseCursor` is the only one needing a platform capability; `ForceCursorPosition` plus a drawn crosshair degrades gracefully. Survey §6.6. |
| `TOOL_DISPATCHER` | `: public wxEvtHandler`, not abstract | Write a replacement. Its output contract is only ~10 distinct `TOOL_EVENT` constructions (survey §6.3), and the two subtle helpers — `IsPastDragThreshold` and `ShouldDropAutoRepeat` — are already `static` and wx-free, deliberately so they can be reused. |
| Key codes | constraint | The hotkey vocabulary is `WXK_*` integers. Rust must map its keys onto the same numbers or every default and every saved binding breaks. Transcribe once from `wx/defs.h`, test against `KeyNameFromKeyCode`. |
| `TOOLS_HOLDER` virtuals | straightforward | `GetCurrentSelection()` must be overridden; `PushTool`/`PopTool`/`DisplayToolMsg`/`RegisterUIUpdateHandler` are notification-only and route to the Rust shell. |
| Zoom-dependent repaint | one line | `SCH_VIEW::SetScale()` routes through `SCH_BASE_FRAME::RefreshZoomDependentItems()`, which needs a frame and a selection tool. Once selection exists, `SCH_HOST` must provide the equivalent or cached text will not switch to its bitmap LOD. More pressing than it was: until the §2.4.1 fix the host's scale was pinned at the zoom limit, so no zoom-dependent decision could ever have fired. |
| `ACTION_MENU : public wxMenu` | rewrite | Context menus. `TOOL_INTERACTIVE::SetContextMenu` is the only coupling point. |
| Modal dialogs | ~124 files | Do not attempt. Survey §7.4: keep them for bring-up, async-bridge them through `COROUTINE::Yield` later. The coroutine machinery (`include/tool/coroutine.h`, `libcontext`) is wx-free and already supports the suspension this needs. |

Status of that table, and where it was wrong:

* `VIEW_CONTROLS`, `TOOL_DISPATCHER` and the `TOOLS_HOLDER` virtuals are **done**.
  `SCH_HOST` overrides `GetCurrentSelection()` — it asks the selection tool, as
  `SCH_EDIT_FRAME` does, and falls back to the empty selection while there is no
  tool to ask — plus `RefreshCanvas()`, `DisplayToolMsg()`, `ConfigBaseName()` and
  `GetToolCanvas()`.
* **The key-codes row is wrong.** "Transcribe once from `wx/defs.h`" is the
  obvious design and the wrong one: a transcribed table is one wrong entry per
  silently broken shortcut, and no part of the build would notice. What was built
  sends the key's *name* over the ABI and resolves it in C++. §3.4 has the
  argument, and `grep -rn "WXK_" rust/crates/` is the check.
* **The zoom-dependent repaint row is now live and still pending.** It needed a
  selection to matter, and there is one: `SCH_VIEW::SetScale()` routes through
  `SCH_BASE_FRAME::RefreshZoomDependentItems()`, which is a no-op with a null frame,
  so cached text on the host does not switch to its bitmap LOD and a selected item's
  shadow is not rescaled on zoom. Small, visible, and not yet done.
* `ACTION_MENU` and the dialogs are untouched. Worth adding to the first row:
  `TOOL_INTERACTIVE`'s constructor only builds a `TOOL_MENU` when `Pgm().IsGUI()`,
  and the host runs wx in console mode — so in the host `m_menu` is **null**, and a
  tool converted for Stage 4b has to tolerate that. `SCH_SELECTION_TOOL::Init()`
  dereferences it unguarded today.

### 6.5 Proposed staging, and what each step actually cost

Each stage lands on its own and leaves the tree working. Six of the seven are
done; the order held up, and two of the seven descriptions did not.

1. ~~**Make the downcasts safe.**~~ **Done** — all 16 of §6.2 are `dynamic_cast`
   with a defined no-frame path, and the tools decline a holder that is not their
   frame rather than trusting one. No behaviour change with a real frame, no Rust
   involved, four tests in `qa_eeschema`. `06-what-is-missing.md` Stage 3 has what
   it actually took, which was not what this line predicted. **And it was not
   complete**: `common/`'s seven tool classes had the same hazard and were not in
   the list, because the list came from grepping eeschema. Step 6 tripped over them
   immediately.
2. ~~**Neutralise `GetToolCanvas()`.**~~ **Not done, and dropped.** It is still
   `= 0`, and that is the right answer: `SCH_HOST::GetToolCanvas()` returns
   `nullptr` in one line, exactly as step 1's test double does, and changing a base
   class every KiCad program inherits so that one new class can omit one line buys
   nothing. This step existed because the survey called it the blocker; it was not
   one.
3. ~~**Hoist undo/redo** off `EDA_BASE_FRAME` into a container both it and
   `SCH_HOST` own (§6.3).~~ **Done**, and it did belong with the first tool that
   mutates — it landed the day before the move tool. `UNDO_REDO_HOLDER` is the
   container; the schematic half is `SCH_UNDO_REDO`. See §9.3, and note that moving
   it is what gave eeschema's undo its first tests.
4. ~~**`HOST_VIEW_CONTROLS`.**~~ **Done** — the eight pure virtuals against
   host-supplied pointer state, in `common/view/host_view_controls.cpp`, testable
   headlessly with no Rust as predicted. `WarpMouseCursor` is the only one that
   cannot be honoured; it adopts the position, moves the view and records the
   request, and the survey's "degrades gracefully" holds.
5. ~~**`HOST_TOOL_DISPATCHER`.**~~ **Done** — the ten `TOOL_EVENT` shapes of survey
   §6.3, unit-tested by feeding synthetic input and asserting on what a recording
   tool receives. It was indeed far easier than testing the wx one. Two notes on
   §6.4's row for it: `IsPastDragThreshold` *is* reused, and
   `ShouldDropAutoRepeat` deliberately is not — its whole job is to notice a repeat
   that arrived after the key was released, which is a wx key-model artefact that an
   ordered event stream cannot produce.
6. ~~**Make `SCH_HOST` a `TOOLS_HOLDER`** and register the eeschema tools.~~
   **Done, and it is where the cost was.** This step is why the section above says
   "the right place to find out what else breaks": registering the roster
   segfaulted in the constructor, on `common/`'s tools, before a single
   `TOOL_EVENT` existed. Seven more entry points needed step 1's treatment. And the
   prediction that "selection and move can be driven from a C++ test at this point"
   is false: every tool declines, so what a C++ test can assert is that none of
   them survives. The frame hoist is `06-what-is-missing.md` Stage 4b — after which
   the prediction became true, and selection, move and wire *are* driven from a C++
   test.
7. ~~**Extend the C ABI** with input events and action dispatch.~~ **Done** —
   ABI version 3, four entry points, §3.4. The second half was as cheap as this
   said: the bindings come from the header on every build, so Rust got them by
   recompiling. The part that was *not* cheap to get right is where the key-code
   mapping lives; §3.4 has the argument.

Steps 1, 2 and 3 were described as ordinary C++ refactors that improve the tree
whether or not the Rust work continues, which was a good property for them to
have — and step 1's extension into `common/` has it too: `PROPERTIES_TOOL`'s
`if( editFrame )` guard could not fire before and can now, in every KiCad program.

Step 3 turned out to have the same property twice over. Hoisting the undo stacks
uncovered `EDA_DRAW_FRAME`'s shadowed `m_undoRedoCountMax`, so the "maximum undo
items" preference now works in eeschema, pcbnew, gerbview and the page-layout editor;
and moving eeschema's undo into shared functions gave it the first tests it has ever
had, which protect the wx editor and not only the host. §9.4 has both.

---

## 7. A bug found and fixed in `DRAW_STREAM::Compact()`

Compaction rewrote each relocated coordinate index into argument slot *n* for
run *n*, with one special case for `KGDS_OP_SEGMENT_CHAIN`. That mapping is not
the one the ABI actually uses. `kgds_coord_refs()` reads `KGDS_OP_BITMAP`'s runs
from **arg1 and arg2**, because arg0 holds an image-table index; and it reads
`KGDS_OP_DRAW_GROUP`'s optional depth override from **arg2**, because arg0 holds
a group id.

So compacting a group containing a bitmap did two things:

* wrote the transform's new offset over the image-table index, silently
  repointing the command at a different image;
* left both geometry indices pointing past the end of the compacted arena.

Reproduced with a standalone harness against the pre-fix code — one deleted
group, one retained bitmap group, two images:

```
old:    op=0x3d arg0(image)=0 arg1(xform)=6 arg2(alpha)=9 coords=7
        FAIL: image index clobbered (0, want 1)
        FAIL: xform out of range          <- reads 6 doubles from offset 6 of a 7-element arena
fixed:  op=0x3d arg0(image)=1 arg1(xform)=0 arg2(alpha)=6 coords=7
        => OK
```

The out-of-range index is the serious half: the consumer's job is to render what
the indices point at, and five of those six doubles are off the end of the
buffer. `KGDS_OP_SEGMENT`, `KGDS_OP_SEGMENT_CHAIN` and `KGDS_OP_ELLIPSE_ARC`
were all already correct, and still are.

**Why it had not shown up.** It needs a `KGDS_OP_BITMAP` inside a *retained*
group plus a compaction, and eeschema puts bitmaps on `LAYER_DRAW_BITMAPS`,
which `SCH_DRAW_PANEL` — and `SCH_HOST`, copying it — sets to
`TARGET_NONCACHED`. Bitmaps therefore land in the frame arena, which `Compact()`
does not touch. One changed layer target, or pcbnew's reference images later,
and it becomes live. The existing compaction test used a circle, whose single
run does live in arg0.

**The fix** (`common/gal/recording/draw_stream.cpp`) does not duplicate the
ABI's table locally, because two copies would drift and the symptom would
reappear far from the cause. It recovers the run-to-slot mapping *from the ABI
function itself*: poke a value that appears nowhere else in the command into one
argument slot at a time, and see which run's `start` follows it. The shared
header stays the single source of truth, and any opcode added later is handled
without touching this code.

While there, the out-of-range branch now reserves the space it could not copy,
so that "every index in a compacted stream is in range" holds unconditionally —
that is the invariant the consumer's bounds check rests on, and it should not
have an exception for already-damaged input.

Regression coverage is in
`qa/tests/common/gal/test_draw_stream.cpp::CompactionRelocatesNonArg0Indices`,
which checks `KGDS_OP_BITMAP` and `KGDS_OP_SEGMENT_CHAIN` through a compaction
and then asserts the in-range invariant across the whole compacted stream.

### 7.1 A second, smaller gap in the same file

`DRAW_STREAM::Deserialize()` validates the untrusted input it is given — every
coordinate index in range, every opcode known, every group body inside the
command array — but it did not check two things that a consumer dereferences
just as directly:

* a `kgds_image`'s `data_offset + data_length` against the image arena. A
  renderer uploads exactly that range to a texture on the strength of the table
  alone.
* `KGDS_OP_BITMAP`'s `arg0` against the image table. It is an image index rather
  than a coordinate, so `kgds_coord_refs()` does not report it and the
  coordinate bounds check never sees it.

Both are now checked. Confirmed against the pre-fix code:

```
old:    baseline decodes: yes   oversized length rejected: NO    bad offset rejected: NO
fixed:  baseline decodes: yes   oversized length rejected: yes   bad offset rejected: yes
```

Covered by `test_draw_stream.cpp::MalformedImageTablesAreRejected`.

---

## 8. Other known issues found while doing this

* **`EESCHEMA_HELPERS::LoadSchematic` leaks a `TOOL_MANAGER`.**
  `eeschema/eeschema_helpers.cpp:364` does `TOOL_MANAGER* toolManager = new TOOL_MANAGER;`
  and never deletes it. One per load. Pre-existing, unrelated to this work, and
  harmless for a CLI that loads once — but a host that loads repeatedly in one
  process will accumulate them. Not fixed here because it is upstream code on a
  path shared with `kicad-cli`.
* **The 14 unchecked downcasts of §6.2** are latent UB that the next milestone
  will trip over. Also pre-existing.
* **The connectivity engine is main-thread-only, and asserts it.**
  `SCH_CONNECTIVITY::ENGINE::Clear` (`eeschema/connectivity/conn_engine.cpp:117`)
  and `SCH_CONNECTIVITY::INPUT_STORE::Invalidate`
  (`eeschema/connectivity/conn_inputs.cpp:421`) both `wxASSERT( wxThread::IsMain() )`,
  and `SCHEMATIC`'s constructor reaches both through `Reset()` — so an ordinary
  load trips them off the main thread. wx's "main thread" is whichever thread
  called `wxInitialize`, i.e. whichever one called `ksch_runtime_init`, so a
  single worker thread that does everything is fine and a second one is not. The
  header's threading section used to say "one session per thread", which is
  wrong; it now says one thread. Found by running the Rust integration tests
  under libtest, which puts each test on a different worker thread.
* **A recorded stream's ordering is not canonical across standard libraries.**
  The same schematic, the same viewport, the same ABI, recorded on Linux and on
  macOS: identical group table, identical 2,587 group commands, identical
  coordinate multiset — and four of 222 group bodies holding those coordinates
  under different group ids. Group ids are assigned in the order `KIGFX::VIEW`
  visits items, and items with equal sort keys are left in whatever order an
  unstable sort produced, which libstdc++ and libc++ decide differently.

  Nothing renders differently, and nothing in the renderer cares: it replays the
  frame's `DRAW_GROUP` list in order. Two consequences, though. Regenerating
  `qa/data/draw_streams/` on a different platform produces a diff that is not a
  change in KiCad, so don't; and a test comparing a live render with a fixture
  has to compare the picture, not the bytes —
  `rust/crates/kicad-sch-sys/tests/live_session.rs` does, and explains how.

  One further wrinkle, added by §9: a host whose tools are running has overlay
  *targets* in use, because the tools put view items on them — the selection group,
  the entered-group overlay, the grid helper's axis cross and snap point — and
  `KIGFX::VIEW` brackets a layer it has items on with a `SetTarget` and a
  `SetLayerDepth` whether or not any of them is visible. So a live frame carries four
  state commands the pre-4b fixtures do not, and draws nothing extra. The live-host
  test states exactly that rather than tolerating a count: nothing that draws may
  appear or disappear, and the only new state may be an overlay bracket.

  Worth knowing beyond this project: it means a draw stream is not a canonical
  form of a schematic's geometry, so it cannot be used as a cross-platform
  rendering hash. Sorting items by a total order before recording would fix that
  if it were ever wanted.


---

## 9. The editing seam: what a tool needs that is not a window

§6 asked what it would take to *feed* `TOOL_MANAGER`, and answered it. What it did
not ask, because at the time no tool could run at all, is what a tool needs once the
events arrive. This section is that, and it is the part to read before converting
tool number five.

### 9.1 `SCHEMATIC_HOLDER`, which already existed

The decision §6 framed as two routes — give the host a `SCH_BASE_FRAME`, or reroute
`m_frame` onto an interface — was settled by finding that the interface was already
in the tree. `eeschema/schematic_holder.h` held four virtuals, introduced with this
comment:

> This is a bridge class to help the schematic be able to affect SCH_EDIT_FRAME
> without doing anything too wild in terms of passing callbacks constantly in
> numerous files
>
> The long term goal would be to fix the internal structure and make the
> relationship between frame and schematic less intertwined

`SCH_BASE_FRAME` already implemented it. What it grew is the rest of what a tool
asks its editor for, and the two rules that kept it from becoming `SCH_EDIT_FRAME`
again are the useful part:

**Anything inherently a window stays off it.** A dialog, an info bar, keyboard
focus, hypertext navigation, the hierarchy navigator pane, the variant selector, the
net-collision colour settings. A tool that wants one downcasts to the frame and does
nothing when the answer is null — with a comment at the site saying what is lost, so
that "this editor cannot do X" is discoverable from the code rather than from
running it.

**Anything the tool framework already answers stays off it too.** This is the one
that shrinks the work most:

| looks like it needs a frame | actually |
|---|---|
| `m_frame->GetCanvas()->GetView()` | `TOOL_BASE::getView()` |
| `m_frame->GetCanvas()->GetViewControls()` | `TOOL_BASE::getViewControls()` |
| `m_frame->ToolStackIsEmpty()`, `IsCurrentTool()`, `PushTool()`, `GetDragAction()`, `GetMoveWarpsCursor()` | `m_toolMgr->GetToolHolder()`, i.e. `TOOLS_HOLDER` |
| `m_frame->Schematic()`, `GetCurrentSheet()`, `SetSheetNumberAndCount()` | `GetScreen()->Schematic()`, which is the document asking itself |

Two things did move onto the interface that a reader might expect to be the frame's,
because nothing about them is: `AutoRotateItem` needs only the screen and the current
sheet, and is not even virtual; and `SCH_EDIT_FRAME::TrimWire` did its work by
calling back into `SCH_LINE_WIRE_BUS_TOOL`, so it is now that tool's method, where
both its callers already were.

### 9.2 The opt-in, and why it is off by default

`SCH_TOOL_BASE<T>` gained `m_editor` — a `SCHEMATIC_HOLDER*`, non-null whenever the
tool initialised at all — and a `runsWithoutAFrame()` that returns **false** unless
a tool overrides it. That default is load-bearing. A tool that has not been converted
still does `m_frame->GetScreen()` in fifty places, so letting it initialise with a
null `m_frame` would replace "this tool is absent" with "this tool crashes on the
first click". Declining is what `TOOL_MANAGER::InitTools()` already knows how to
handle.

A tool that answers true must tolerate two nulls: `m_frame`, and **`m_menu`** —
`TOOL_INTERACTIVE` only builds a `TOOL_MENU`, and therefore a `wxMenu`, when
`Pgm().IsGUI()`. Both converted tools moved their context-menu construction into a
`buildContextMenu()` that returns early, which is also a tidier shape than a
hundred-line `Init()`. `TOOL_INTERACTIVE::HasToolMenu()` is new and exists because a
tool that adds items to *another* tool's menu cannot see the other's `m_menu`.

### 9.3 Undo, in two pieces

`UNDO_REDO_CONTAINER m_undoList` / `m_redoList` were members of `EDA_BASE_FRAME`
(§6.3). They are now `UNDO_REDO_HOLDER`, a mixin `EDA_BASE_FRAME` inherits — so
every frame in KiCad keeps these methods and every derived override of
`ClearUndoORRedoList` keeps working — and which `SCH_HOST` inherits too.

The schematic-specific half went with them. `SaveCopyInUndoList`,
`PutDataInPreviousState` and `RollbackSchematicFromUndo` were `SCH_EDIT_FRAME`
members; they are now `SCH_UNDO_REDO` free functions over `SCHEMATIC_HOLDER`, with
the frame's methods of the same names forwarding, so the GUI runs this code rather
than a copy of it. `Undo()` and `Redo()` joined them, because `SCH_EDITOR_CONTROL`'s
versions were twenty lines of stack shuffling around one call each.

**Sharing rather than duplicating is what made it testable, and it needed to be.**
Nothing in the QA suite called either entry point — they needed a window — so
eeschema's undo had no coverage at all, which is both why the refactor was risky and
why it was the right call. The six cases that drive it through the host are the
first tests it has ever had, and they cover the frame by proxy.

Only one case still needs a window: page-settings undo goes through
`DS_PROXY_UNDO_ITEM`, which takes an `EDA_DRAW_FRAME`. An editor with no
page-settings dialog cannot have recorded one, and the code says so at the site.

### 9.4 The four defaults nobody had checked

Worth its own heading because it is the shape of bug this seam finds, and because
three of the four are in code every KiCad program runs.

| Where | What |
|---|---|
| `KIGFX::GAL` | Its constructor never sets `m_gridSize`. In a GUI `COMMON_TOOLS::Reset()` always fills it in from the window settings, and that tool declines a non-frame holder. A zero grid is not "no grid": `GRID_HELPER` divides the cursor position by it, so the first tool that snaps gets an infinity and `KiROUND` asserts several frames from the cause. `SCH_HOST::initGrid()` reads the user's own grid list the same way `COMMON_TOOLS` does. |
| `TOOLS_HOLDER` | Its constructor sets `m_dragAction = MOUSE_DRAG_ACTION::SELECT`, and every frame replaces it from the common settings in `CommonSettingsChanged()`. Until the host called that, a drag over a *selected* item drew a rubber band instead of moving it. The rubber-band test written the day before **passed because of the bug**. |
| `EDA_DRAW_FRAME` | It declared a second `m_undoRedoCountMax` that shadowed `EDA_BASE_FRAME`'s. `LoadSettings` wrote the user's `max_undo_items` into the derived one, which nothing read; the push methods read the base one, which was only ever the constructor's default. So the preference was ignored in four programs and, because `SaveSettings` writes it back, overwritten on every save. Fixed. |
| `SCH_HOST` | `SCH_EDIT_FRAME` has a `SCHEMATIC` and an empty `SCH_SCREEN` from its constructor on, so a tool may use `GetScreen()` without checking — and does. This host had neither until something was loaded, and `drawWires` on an empty session dereferenced null. It now refuses input and actions with no document; giving it an empty document at construction, as the frame has, is the better long-term answer. |

### 9.5 One duplication, deliberately

`SCH_HOST_CONTROL` handles `ACTIONS::undo`, `redo` and `save`, which
`SCH_EDITOR_CONTROL` also registers. It exists as a *tool* rather than as three C ABI
calls because a hotkey is resolved inside `TOOL_MANAGER`: a UI on the far side of the
boundary can forward ⌘Z but cannot intervene in what it means, so recognising the
three action *names* on the Rust side would have made the menu work and the key not.
The handlers are three lines each and delegate to the shared `SCH_UNDO_REDO`, so what
is duplicated is the dispatch and not the work — and `Init()` declines any holder that
is not a `SCH_HOST`, so it is inert in the wx editor.
