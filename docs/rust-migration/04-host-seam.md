# 04 — The host seam: `SCH_HOST`, the C ABI, and what is not done yet

This document covers the C++ side of the boundary between eeschema's document
model and the Rust UI: what was built, what it is verified to do, and — in
§6, which is the part to read if you are picking this up — exactly what stands
between here and driving `TOOL_MANAGER` from Rust.

Companion documents: `00-architecture-survey.md` (the reconnaissance this is
built on, especially §5–§8), `01-plan.md` (the architecture), `03-build-notes.md`
(how to build).

---

## 1. What was added

| Path | What it is |
|---|---|
| `eeschema/host/sch_host.h` / `.cpp` | `SCH_HOST` — a schematic editor session with no `wxFrame` |
| `include/sch_host/sch_host_abi.h` | The plain-C ABI a Rust UI drives it through |
| `eeschema/host/sch_host_abi.cpp` | Its implementation, plus the action-registry export |
| `qa/tools/sch_dump/sch_dump.cpp` | `kicad-sch-dump`, the end-to-end proof and fixture generator |
| `qa/tests/eeschema/test_sch_host.cpp` | QA coverage for all of the above |
| `qa/data/draw_streams/` | Checked-in golden draw streams |

Both `eeschema/host/` sources are compiled into `eeschema_kiface_objects`, so
anything that already links eeschema — the kiface, `qa_eeschema`, a future Rust
host library — gets them for free.

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

It has been verified to be `bindgen`-clean, not assumed to be: `bindgen` 0.73
parses it with no diagnostics and produces 23 `extern "C"` functions, and the
generated crate — including bindgen's compile-time layout assertions — compiles.
Those assertions are the useful part, because they are the Rust side agreeing
with the C side about every struct:

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

* Session-scoped — the error string and the three strings in `ksch_sheet_info`.
  Valid until the next call on that session. The UTF-8 conversions are parked on
  the session struct rather than on a temporary, which is the only reason those
  pointers are safe to return at all.
* Process-scoped — everything in `ksch_action`, and the global error string.

### 3.3 Coordinates

`double`, in KiCad internal units, matching the draw stream. A bounding box and
the geometry in a frame are therefore in the same space and need no conversion
on the Rust side.

---

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
  `InitSettings()` called on it is sufficient. **A Rust host library will need
  the same two objects**, and this is the worked example.

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

## 6. Not done: feeding `TOOL_MANAGER` from Rust

This is the next milestone and it is **not** started. Survey §6 and §7 name
`TOOLS_HOLDER::GetToolCanvas()` as the blocker. That is right, but it is not the
worst of it, and the ordering below reflects what the code actually says rather
than what the survey predicted.

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

There are **14** such `static_cast`s in eeschema (plus 5 `dynamic_cast`s, which
are fine — they yield null):

* `eeschema/sch_commit.cpp` — 6, at `:51`, `:163`, `:195`, `:606`, `:624`, `:637`
* `eeschema/tools/sch_editor_control.cpp` — 6, at `:1065`, `:1220`, `:1576`,
  `:1602`, `:1654`, `:1771`
* `eeschema/tools/symbol_editor_control.cpp` — 2, at `:974`, `:983`

`SCH_COMMIT` is the one that matters most, because **every edit goes through
it**. Its `TOOL_MANAGER*` constructor downcasts the holder on line 51 before it
has done anything else.

Note that today this is latent rather than live: `EESCHEMA_HELPERS::LoadSchematic`
constructs a `TOOL_MANAGER` and calls
`SetEnvironment( schematic, nullptr, nullptr, KifaceSettings(), nullptr )` —
a **null** holder — so `frame && ...` short-circuits and the headless CLI path is
safe. Installing a non-null, non-frame holder is what turns it into UB. That is
exactly what the next milestone must do, so these 19 sites have to be converted
to `dynamic_cast` (or to a virtual on `TOOLS_HOLDER`) **first**, as a standalone
commit, before a single `TOOL_EVENT` is sent.

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
| Zoom-dependent repaint | one line | `SCH_VIEW::SetScale()` routes through `SCH_BASE_FRAME::RefreshZoomDependentItems()`, which needs a frame and a selection tool. Once selection exists, `SCH_HOST` must provide the equivalent or cached text will not switch to its bitmap LOD. |
| `ACTION_MENU : public wxMenu` | rewrite | Context menus. `TOOL_INTERACTIVE::SetContextMenu` is the only coupling point. |
| Modal dialogs | ~124 files | Do not attempt. Survey §7.4: keep them for bring-up, async-bridge them through `COROUTINE::Yield` later. The coroutine machinery (`include/tool/coroutine.h`, `libcontext`) is wx-free and already supports the suspension this needs. |

### 6.5 Proposed staging

Each stage lands on its own and leaves the tree working.

1. **Make the downcasts safe.** Convert the 14 `static_cast`s in §6.2 to
   `dynamic_cast` with null handling, or add the two or three virtuals to
   `TOOLS_HOLDER` that would remove the need for a downcast at all. No behaviour
   change, no Rust involved, fully testable today. **This is the prerequisite for
   everything below and should land first.**
2. **Neutralise `GetToolCanvas()`.** Change the return type to an opaque handle,
   or give `TOOLS_HOLDER` a default implementation returning `nullptr` and drop
   the `= 0`. ~12 implementations, six call sites, all listed in §6.1.
3. **Hoist undo/redo** off `EDA_BASE_FRAME` into a container both it and
   `SCH_HOST` own (§6.3).
4. **`HOST_VIEW_CONTROLS`.** Implement the eight pure virtuals against
   host-supplied pointer state. Testable headlessly with no Rust: assert that
   cursor position, snapping and `ForceCursorPosition` behave.
5. **`HOST_TOOL_DISPATCHER`.** A non-wx event source producing the ~10
   `TOOL_EVENT` shapes of survey §6.3. Unit-test it by feeding synthetic input
   and asserting on the events, which is far easier than testing the wx one.
6. **Make `SCH_HOST` a `TOOLS_HOLDER`** and register the eeschema tools. At this
   point selection and move can be driven from a C++ test with no Rust at all —
   which is the right place to find out what else breaks.
7. **Extend the C ABI** with input events and action dispatch
   (`TOOL_ACTION::MakeEvent()` → `TOOL_MANAGER::ProcessEvent()`), and only then
   wire Rust to it.

Stages 1–3 are ordinary C++ refactors that improve the tree whether or not the
Rust work continues, which is a good property for them to have.

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

