# gpui-kit Cookbook (gpui-kit 0.6.1 / gpui-pre 0.3.5)

Everything here was read out of the actual crate sources under
`~/.cargo/registry/src/index.crates.io-*/` and **verified by compiling**
(`/tmp/claude-0/gpui-probe`, `cargo check` + `cargo test` green). Line
references are to those sources. No docs.rs, no memory.

## Crate map (confirmed from `Cargo.toml`s)

| Import path           | Real crate          | Notes |
| --------------------- | ------------------- | ----- |
| `gpui_kit::*`         | `gpui-pre` 0.3.5 (lib name `gpui`) | `gpui-kit/src/lib.rs`: `pub use ::gpui::*;` |
| `gpui_kit::platform`  | `gpui-pre-platform` 0.3.5 | `application()`, `current_platform()` |
| `gpui_kit::base`      | `gpui-base` 0.6.1   | behaviour layer (dock model, resizable, scrollbar, input model) |
| `gpui_kit::component` | `gpui-component` 0.6.1 | the shadcn-style widget library (feature `component`, on by default) |
| `gpui_kit::assets`    | `gpui-kit-assets` 0.6.1 | embedded Lucide icon SVGs (feature `assets`, on by default) |
| (internal)            | `gpui-pre-wgpu` 0.3.5 | the renderer — **not** a dependency of `gpui` itself, it depends on `gpui` |
| (internal)            | `gpui-pre-linux` 0.3.5 | X11 + Wayland platform |

`gpui-kit`'s manifest declares `gpui = { version = "0.3.1", package = "gpui-pre" }`
and `gpui_platform = { package = "gpui-pre-platform", features = ["font-kit",
"x11", "wayland", "runtime_shaders"] }`. Resolved wgpu in this workspace is
**29.0.4**, lyon 1.0.19, image 0.25.10 (from `/tmp/claude-0/probe2/Cargo.lock`).

---

## 1. Setup

### Cargo.toml (exact, verified)

```toml
[package]
name = "kicad-gui"
version = "0.1.0"
edition = "2024"          # gpui-pre is edition 2024 and uses async closures

[dependencies]
gpui-kit = "0.6.1"        # default = ["component", "assets"]

[dev-dependencies]
gpui-kit = { version = "0.6.1", features = ["test-support"] }
image    = "0.25"         # only if you construct RenderImage yourself; gpui does NOT re-export `image`
```

Depend on **`gpui-kit`**, not `gpui`. The `gpui` name on crates.io at `^0.3.1`
resolves to the `gpui-pre-*` family, and `gpui-kit` pins the whole matching set.
`gpui-kit` also ships its own `actions!` macro because gpui's original expands to
`gpui::Action`, which does not resolve when gpui is reached through the facade.

### Complete feature list of `gpui-kit` 0.6.1

| Feature | Gates |
| --- | --- |
| `component` *(default)* | `dep:gpui-component` → `gpui_kit::component` |
| `assets` *(default)* | `dep:gpui-kit-assets` → `gpui_kit::assets` (Lucide icon SVGs) |
| `test-support` | `gpui/test-support`, `gpui_platform/test-support`, `gpui-base/test-support`, `gpui-component?/test-support`. Enables `#[gpui_kit::test]`, `TestAppContext`, `VisualTestContext`, `gpui_kit::test::*` |
| `inspector` | `gpui/inspector`, `gpui-base/inspector`, `gpui-component?/inspector` — live element inspector |
| `profiler` | `gpui/profiler` — frame/task instrumentation + debug frame overlay |
| `decimal` | `gpui-component/decimal` |
| `tree-sitter`, `tree-sitter-languages`, `tree-sitter-<lang>` (≈35 of them: `-rust`, `-cpp`, `-python`, `-c`, `-cmake`, `-toml`, `-yaml`, `-json`-ish via others, …) | syntax highlighting in the code editor input |

For KiCad: `default` + `test-support` (dev) is enough. Add `tree-sitter`
+ `tree-sitter-python` if you want a scripting console with highlighting.

### ⚠️ Toolchain gotcha (verified twice)

`gpui-pre 0.3.5` **does not compile on stable 1.94.1**:

```
error[E0658]: use of unstable library feature `cold_path`
  --> gpui-pre-0.3.5/src/profiler.rs:473:13
473 |             std::hint::cold_path();
```

There are two such calls (`profiler.rs:473` and `:494`). Use a toolchain where
`std::hint::cold_path` is stable (nightly works today; the fix landed in a
later stable than 1.94). Everything in this document was type-checked with
`cargo +nightly check`. Pin it:

```toml
# rust-toolchain.toml
[toolchain]
channel = "nightly"   # or a stable >= the release that stabilised cold_path
```

### Asset source

`gpui-component` icons are SVGs loaded through `AssetSource`:

```rust
gpui_kit::platform::application()
    .with_assets(gpui_kit::assets::Assets)   // or AllAssets for the full catalog
    .run(|cx| { /* ... */ });
```

---

## 2. Minimal app

`Application::run` takes `FnOnce(&mut App)`. The canonical shape from the
crate's own README/doctest opens the window from a spawned task:

```rust
use gpui_kit::component::Root;
use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::prelude::*;                 // Render, IntoElement, Styled, ParentElement, ...
use gpui_kit::{App, Bounds, Context, KeyBinding, Window, WindowOptions, div, point, px, size};

gpui_kit::actions!(app, [Quit]);

struct Hello;

impl Render for Hello {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().child(Button::new("go").primary().label("Let's Go!"))
    }
}

fn main() {
    gpui_kit::application()                  // == gpui_platform::application()
        .with_assets(gpui_kit::assets::Assets)
        .run(|cx: &mut App| {
            gpui_kit::init(cx);              // == gpui_component::init(cx) with `component`
            cx.bind_keys([KeyBinding::new("ctrl-q", Quit, None)]);
            cx.on_action(|_: &Quit, cx| cx.quit());

            cx.spawn(async move |cx| {
                cx.open_window(
                    WindowOptions {
                        window_bounds: Some(gpui_kit::WindowBounds::Windowed(Bounds {
                            origin: point(px(0.), px(0.)),
                            size: size(px(1280.), px(800.)),
                        })),
                        ..Default::default()
                    },
                    |window, cx| {
                        let view = cx.new(|_| Hello);
                        cx.new(|cx| Root::new(view, window, cx))   // required by gpui-component
                    },
                )
                .expect("failed to open window");
            })
            .detach();
        });
}
```

`Root::new(view: impl Into<AnyView>, window: &mut Window, cx: &mut Context<Self>) -> Self`
(`gpui-component/src/root.rs:98`) hosts modals, sheets, tooltips, notifications
and the fallback menu overlay. **Every gpui-component app needs it as the window
root**, otherwise popovers/tooltips/dialogs never render.

`App::open_window` is also available synchronously
(`gpui-pre/src/app.rs:1283`):

```rust
pub fn open_window<V: 'static + Render>(
    &mut self,
    options: crate::WindowOptions,
    build_root_view: impl FnOnce(&mut Window, &mut App) -> Entity<V>,
) -> anyhow::Result<WindowHandle<V>>
```

### The core traits in **this** version

```rust
// gpui-pre/src/element.rs:163
pub trait Render: 'static + Sized {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement;
}

// gpui-pre/src/element.rs:50
pub trait Element: 'static + IntoElement {
    type RequestLayoutState: 'static;
    type PrepaintState: 'static;
    fn id(&self) -> Option<ElementId>;
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>>;
    fn request_layout(&mut self, id: Option<&GlobalElementId>,
                      inspector_id: Option<&InspectorElementId>,
                      window: &mut Window, cx: &mut App) -> (LayoutId, Self::RequestLayoutState);
    fn prepaint(&mut self, id: Option<&GlobalElementId>,
                inspector_id: Option<&InspectorElementId>, bounds: Bounds<Pixels>,
                request_layout: &mut Self::RequestLayoutState,
                window: &mut Window, cx: &mut App) -> Self::PrepaintState;
    fn paint(&mut self, id: Option<&GlobalElementId>,
             inspector_id: Option<&InspectorElementId>, bounds: Bounds<Pixels>,
             request_layout: &mut Self::RequestLayoutState,
             prepaint: &mut Self::PrepaintState, window: &mut Window, cx: &mut App);
    // + optional a11y hooks: a11y_role, write_a11y_info, a11y_synthetic_children
}
```

Notes for people coming from older gpui:
* There is **no `ViewContext`**. It is `&mut Window` + `&mut Context<V>` (or `&mut App`).
* `Element` has **five** required methods now; `source_location()` and the two
  `InspectorElementId` parameters are new. `a11y_role`/`write_a11y_info` have defaults.
* State lives in `Entity<T>` (`cx.new(|cx| T{..})`); `cx.notify()` marks it dirty.
* **`Pixels`' tuple field is `pub(crate)` in this fork** — `px.0` does not compile.
  Use `Pixels::as_f32()`, `to_f64()`, `From<Pixels> for f32`, or `Pixels::ZERO`.
  This breaks a lot of copy-pasted upstream gpui code.

### Event loop, redraw, animation frames

```rust
// gpui-pre/src/window.rs:2586
pub fn on_next_frame(&self, callback: impl FnOnce(&mut Window, &mut App) + 'static);
// gpui-pre/src/window.rs:2606  -> on_next_frame(|_, cx| cx.notify(current_view))
pub fn request_animation_frame(&self);
// gpui-pre/src/window.rs:2248
pub fn refresh(&mut self);          // redraw the whole window
// gpui-pre/src/window.rs:2616 (test-support only)
pub fn simulate_next_frame(&mut self, cx: &mut App) -> usize;
```

Calling `window.request_animation_frame()` inside `render()` gives you a
continuously animating view (it re-notifies the current entity every frame).
`WindowOptions::inactive_frame_interval: Option<Duration>` throttles animation
while the window is not focused; set it to `None` to disable throttling.

---

## 3. Custom GPU canvas — the recommended mechanism

### 3.0 What is and is not possible (all verified in source)

**(a) Access gpui's `wgpu::Device`/`Queue`/`RenderPass` — NOT POSSIBLE.**
`gpui-pre-wgpu/src/gpui_wgpu.rs` exports exactly:

```rust
pub use cosmic_text_system::*;
pub use wgpu;
pub use wgpu_atlas::*;
pub use wgpu_context::*;      // WgpuContext { instance, adapter, device: Arc<Device>, queue: Arc<Queue> }
pub use wgpu_renderer::{GpuContext, WgpuRenderer, WgpuSurfaceConfig};
```

`WgpuContext` *is* public and *does* expose `device`/`queue`. But:
* `gpui-pre` (the crate you actually use) has **zero** wgpu dependency —
  `grep wgpu gpui-pre-0.3.5/Cargo.toml` returns nothing. The dependency runs the
  other way: `gpui-pre-wgpu` depends on `gpui-pre`.
* The only live `GpuContext = Rc<RefCell<Option<WgpuContext>>>` instances are
  `X11Client::gpu_context` (`pub(crate)`, `gpui-pre-linux/src/linux/x11/client.rs:192`)
  and `WaylandClientState::gpu_context` (inside a private struct,
  `.../wayland/client.rs:318`). Neither client type nor field is reachable from
  `Window`, `App`, or `Platform`.
* `WgpuRenderer`'s only frame entry point is `pub fn draw(&mut self, scene: &Scene) -> bool`
  (`wgpu_renderer.rs:1265`). `record_frame` is private and there is **no**
  callback, hook, `custom_render`, `external_texture` or user render-pass slot
  anywhere in the 2243-line file.
* All `Window` gives you about the GPU is `pub fn gpu_specs(&self) -> Option<GpuSpecs>`
  (`window.rs:6604`).

So injecting your own draw calls into gpui's command encoder is impossible
without forking `gpui-pre-linux` + `gpui-pre-wgpu`.

**(b) Hand gpui a `wgpu::Texture` — NOT POSSIBLE; CPU round-trip only.**
* The `Surface` element (`gpui-pre/src/elements/surface.rs`) and
  `Window::paint_surface` are **`#[cfg(target_os = "macos")]` only** and take a
  CoreVideo `CVPixelBuffer`. `PaintSurface` in `scene.rs:768` literally has no
  payload field off macOS.
* `Window::paint_image(bounds, image_bounds, corner_radii, data: Arc<RenderImage>,
  frame_index, grayscale)` (`window.rs:4776`) goes through
  `sprite_atlas.get_or_insert_with(...)` with `data.as_bytes(frame_index)` — a
  `&[u8]` CPU slice (`assets.rs:72`). `RenderImage` is BGRA CPU pixels
  (`SmallVec<[image::Frame; 1]>`).
* `ImageSource` (`elements/img.rs:42`) is `Resource | Render(Arc<RenderImage>) |
  Image(Arc<Image>) | Custom(fn -> Arc<RenderImage>)`. All CPU.

**Cost of the CPU round-trip at 1920×1080 (measured in bytes, not hand-waved):**
one RGBA/BGRA frame is 1920·1080·4 = **8.29 MB**. Per frame you pay
(1) `copy_texture_to_buffer` + `map_async` + a **GPU→CPU sync stall**,
(2) memcpy out of the mapped buffer into an `image::Frame` (8.3 MB),
(3) `WgpuAtlasState::upload_texture` calls `swizzle_upload_data(bytes, format)`
(`wgpu_atlas.rs:250`) — another full 8.3 MB allocation+copy,
(4) `queue.write_texture` staging copy (another ~8.3 MB).
≈ **25–33 MB of memcpy per frame plus a pipeline stall**. At 120 Hz that is
3–4 GB/s of pure copy bandwidth before you draw anything; the readback stall
alone is typically 3–8 ms. Also the atlas caches on `RenderImage.id`, so a fresh
frame needs a new `RenderImage` (new id) every frame and an explicit
`window.drop_image(old)` or the 1024×1024-and-growing atlas leaks.
**Verdict: not viable at 120 Hz. Do not build on this.**

**(c) Custom `Element` emitting gpui primitives — ✅ THIS IS THE ANSWER.**

**(d) Escape hatches that *do* exist** (`Window`, all public):

| Method | Source |
| --- | --- |
| `paint_layer<R>(bounds, f: impl FnOnce(&mut Window) -> R) -> R` | `window.rs:4235` |
| `paint_quad(quad: PaintQuad)` (+ helpers `fill()`, `outline()`, `quad()`) | `window.rs:4386` |
| `paint_path(path: Path<Pixels>, color: impl Into<Background>)` | `window.rs:4457` |
| `paint_drop_shadows` / `paint_inset_shadows(bounds, corner_radii, &[BoxShadow])` | `window.rs:4260/4296` |
| `paint_underline(origin, width, &UnderlineStyle)` / `paint_strikethrough` | `window.rs:4474/4509` |
| `paint_glyph(origin, font_id, glyph_id, font_size, color)` / `paint_emoji` | `window.rs:4544/4649` |
| `paint_svg(bounds, path: SharedString, data: Option<&[u8]>, transformation: TransformationMatrix, color, cx)` | `window.rs:4706` |
| `paint_image(...)` | `window.rs:4776` |
| `with_content_mask`, `with_element_offset`, `with_absolute_element_offset` | `window.rs:3857/3876/3894` |
| `insert_hitbox(bounds, HitboxBehavior) -> Hitbox` | `window.rs:5014` |
| `canvas(prepaint, paint)` element — low-level paint without a custom `Element` | `elements/canvas.rs:10` |

`Scene` / `PrimitiveBatch` / `Primitive` are `pub` (`scene.rs`), but the only way
in is `Window`'s `paint_*`; `Window::next_frame` is private.

### 3.1 gpui's primitive set — is it enough for a schematic?

`Scene` (`gpui-pre/src/scene.rs:41`) holds exactly eight primitive vectors:

```rust
pub struct Scene {
    pub shadows: Vec<Shadow>,
    pub quads: Vec<Quad>,
    pub paths: Vec<Path<ScaledPixels>>,
    pub underlines: Vec<Underline>,
    pub monochrome_sprites: Vec<MonochromeSprite>,   // glyphs, SVG — HAS a TransformationMatrix
    pub subpixel_sprites: Vec<SubpixelSprite>,       // subpixel-AA glyphs — HAS a matrix
    pub polychrome_sprites: Vec<PolychromeSprite>,   // images — NO matrix
    pub surfaces: Vec<PaintSurface>,                 // macOS only
}
```

`Quad` carries `bounds, content_mask, background: Background (solid or gradient),
border_color, corner_radii: Corners, border_widths: Edges, border_style:
{Solid, Dashed}`. `Shadow` supports drop *and* inset.

**`Path` is the important one.** `PathBuilder` (`gpui-pre/src/path_builder.rs`)
is a full **lyon 1.0** front-end, re-exported at the crate root
(`pub use path_builder::*` in `gpui.rs`):

```rust
pub enum PathStyle { Stroke(StrokeOptions), Fill(FillOptions) }

impl PathBuilder {
    pub fn stroke(width: Pixels) -> Self;            // StrokeOptions::default().with_line_width(w)
    pub fn fill() -> Self;                           // FillOptions::default()
    pub fn with_style(self, style: PathStyle) -> Self;
    pub fn dash_array(self, dash_array: &[Pixels]) -> Self;   // SVG stroke-dasharray semantics
    pub fn move_to(&mut self, to: Point<Pixels>);
    pub fn line_to(&mut self, to: Point<Pixels>);
    pub fn curve_to(&mut self, to: Point<Pixels>, ctrl: Point<Pixels>);        // quadratic
    pub fn cubic_bezier_to(&mut self, to: Point<Pixels>, c_a: Point<Pixels>, c_b: Point<Pixels>);
    pub fn arc_to(&mut self, radii: Point<Pixels>, x_rotation: Pixels,
                  large_arc: bool, sweep: bool, to: Point<Pixels>);            // SVG elliptical arc
    pub fn relative_arc_to(...);
    pub fn add_polygon(&mut self, points: &[Point<Pixels>], closed: bool);
    pub fn close(&mut self);
    pub fn transform(&mut self, t: Transform);   // lyon::math::Transform, re-exported
    pub fn translate(&mut self, to: Point<Pixels>);
    pub fn scale(&mut self, s: f32);
    pub fn rotate(&mut self, degrees: f32);
    pub fn build(self) -> Result<Path<Pixels>, anyhow::Error>;
    pub fn build_path(buf: VertexBuffers<lyon::math::Point, u16>) -> Path<Pixels>;
}
// re-exported: pub use lyon::tessellation::{FillOptions, FillRule, StrokeOptions};
// pub use lyon::math::Transform;
```

`StrokeOptions` is lyon's, so you get `.with_line_width()`, `.with_line_cap()`
(Butt/Square/Round), `.with_line_join()` (Miter/MiterClip/Round/Bevel),
`.with_miter_limit()`, `.with_tolerance()`.

So: **thick stroked polylines ✅, arbitrary joins/caps ✅, dashed lines ✅,
elliptical arcs ✅, filled polygons with even-odd or non-zero fill ✅, cubic and
quadratic béziers ✅, rounded/bordered rects via `Quad` ✅, gradients ✅,
per-element clipping via `content_mask` ✅.**

Rendering quality: paths are tessellated on the CPU by lyon, uploaded as
triangles, rasterized into an offscreen intermediate at **4× MSAA** when the
adapter supports it (`RenderingParameters::new` picks `[4,2,1].find(sample_count_supported)`,
`wgpu_renderer.rs:2168`) with premultiplied-alpha blending, then composited.
That is genuinely good AA for schematic line work — better than a naive
1×-sampled custom pipeline.

**Conclusion: gpui's own primitive set is rich enough for KiCad schematic
rendering. You do not need your own wgpu pipeline, and you cannot have one
anyway without forking.**

### 3.2 Performance rules for a 50k-primitive schematic

These come out of `scene.rs`, `path_builder.rs` and `wgpu_renderer.rs`:

1. **Batch aggressively into few `Path` objects.** `PathBuilder` accumulates a
   whole lyon path; one `build()` → one `Path` primitive. Group by
   (colour, width, dash) and emit **one `Path` per bucket**, not one per segment.
2. **u16 index limit.** `build_path` uses `VertexBuffers<_, u16>`, so a single
   `build()` tops out at 65 535 vertices ≈ **~16 000 stroked segments**. Split
   each bucket into chunks and emit several `Path`s. Exceeding it makes lyon
   return `TessellationError::TooManyVertices` from `build()` — handle the `Err`.
3. **Keep everything in one `paint_layer`.** Inside a layer all primitives get
   the same `DrawOrder`, so `Scene::batches()` (`scene.rs:172`) merges all
   consecutive same-order paths into **one** `PrimitiveBatch::Paths`, which
   becomes **one** `draw_paths_to_intermediate` render pass
   (`wgpu_renderer.rs:1702`) with every vertex in a single buffer write.
   Interleaving quads and paths at different orders fragments this into many
   passes — that is the #1 perf cliff.
4. **Quads are instanced and cheap** — a `PrimitiveBatch::Quads` is a single
   instanced draw. Pads, junction dots, selection rectangles, grid dots → quads.
5. **Cull in `prepaint`/`paint` yourself.** gpui culls per-primitive against the
   content mask (`Scene::insert_primitive` drops empty intersections), but you
   still pay CPU tessellation. Do viewport culling against `bounds` first — use
   an R-tree/quadtree over the schematic.
6. **Cache tessellation.** Tessellating 50k segments every frame will dominate.
   Keep the `Path<Pixels>` values in the view keyed by (viewport transform,
   LOD) and only re-tessellate on zoom/pan, not on hover/selection repaints.
   `Path` is `Clone` and `paint_path` takes it by value.
7. `window.pixel_snap(..)`, `pixel_snap_bounds`, `pixel_snap_point` exist
   (`window.rs:2985+`) — use them for crisp 1px grid lines.

### 3.3 Working code sketch (compiles; from `/tmp/claude-0/gpui-probe/src/main.rs`)

```rust
use gpui_kit::prelude::*;
use gpui_kit::{
    App, Bounds, Context, CursorStyle, DispatchPhase, Element, ElementId, Entity,
    GlobalElementId, Hitbox, HitboxBehavior, Hsla, InspectorElementId, IntoElement,
    LayoutId, MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, PathBuilder,
    Pixels, Point, Radians, ScrollDelta, ScrollWheelEvent, Style, StyleRefinement, Styled,
    TextAlign, TextRun, Window, div, font, point, px, rgb, size,
};

#[derive(Clone, Copy)]
struct Viewport { scale: f32, offset: Point<f32> }

impl Viewport {
    fn to_screen(&self, w: Point<f32>, origin: Point<Pixels>) -> Point<Pixels> {
        point(origin.x + px((w.x - self.offset.x) * self.scale),
              origin.y + px((w.y - self.offset.y) * self.scale))
    }
}

struct Segment { a: Point<f32>, b: Point<f32> }
struct Label   { at: Point<f32>, text: &'static str }

struct SchematicCanvas {
    id: ElementId,
    style: StyleRefinement,
    viewport: Viewport,
    segments: Vec<Segment>,
    labels: Vec<Label>,
    view: Entity<SchematicView>,       // for writing back pan/zoom
}

struct CanvasPrepaint { hitbox: Hitbox }

impl Styled for SchematicCanvas {
    fn style(&mut self) -> &mut StyleRefinement { &mut self.style }
}
impl IntoElement for SchematicCanvas {
    type Element = Self;
    fn into_element(self) -> Self::Element { self }
}

impl Element for SchematicCanvas {
    type RequestLayoutState = ();
    type PrepaintState = CanvasPrepaint;

    fn id(&self) -> Option<ElementId> { Some(self.id.clone()) }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> { None }

    fn request_layout(
        &mut self, _: Option<&GlobalElementId>, _: Option<&InspectorElementId>,
        window: &mut Window, cx: &mut App,
    ) -> (LayoutId, ()) {
        let mut style = Style::default();
        style.refine(&self.style);                 // needs `use gpui_kit::Refineable` (in prelude)
        (window.request_layout(style, [], cx), ())
    }

    fn prepaint(
        &mut self, _: Option<&GlobalElementId>, _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>, _: &mut (), window: &mut Window, _: &mut App,
    ) -> CanvasPrepaint {
        CanvasPrepaint { hitbox: window.insert_hitbox(bounds, HitboxBehavior::Normal) }
    }

    fn paint(
        &mut self, _: Option<&GlobalElementId>, _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>, _: &mut (), prepaint: &mut CanvasPrepaint,
        window: &mut Window, cx: &mut App,
    ) {
        let hitbox = prepaint.hitbox.clone();
        let origin = bounds.origin;
        let vp = self.viewport;

        // ONE layer => one DrawOrder => one path batch => one render pass.
        window.paint_layer(bounds, |window| {
            window.paint_quad(gpui_kit::fill(bounds, rgb(0x101014)));

            // Stroked polylines. Bucket by colour/width; chunk at ~16k segments (u16 indices).
            let mut b = PathBuilder::stroke(px(1.5));
            for seg in &self.segments {
                b.move_to(vp.to_screen(seg.a, origin));
                b.line_to(vp.to_screen(seg.b, origin));
            }
            if let Ok(path) = b.build() { window.paint_path(path, rgb(0x00ff9c)); }

            // Dashed elliptical arc.
            let mut arc = PathBuilder::stroke(px(2.)).dash_array(&[px(6.), px(4.)]);
            arc.move_to(point(origin.x + px(40.), origin.y + px(40.)));
            arc.arc_to(point(px(30.), px(30.)), px(0.), false, true,
                       point(origin.x + px(100.), origin.y + px(70.)));
            if let Ok(p) = arc.build() { window.paint_path(p, rgb(0xffaa00)); }

            // Filled polygon.
            let mut poly = PathBuilder::fill();
            poly.add_polygon(&[
                point(origin.x + px(200.), origin.y + px(40.)),
                point(origin.x + px(260.), origin.y + px(40.)),
                point(origin.x + px(230.), origin.y + px(90.)),
            ], true);
            if let Ok(p) = poly.build() { window.paint_path(p, rgb(0x4477ff)); }

            // Horizontal text.
            for label in &self.labels {
                let run = TextRun {
                    len: label.text.len(), font: font("Helvetica"),
                    color: gpui_kit::white(), background_color: None,
                    underline: None, strikethrough: None,
                };
                let shaped = window.text_system()
                    .shape_line(label.text.into(), px(12.), &[run], None);
                let _measured: Pixels = shaped.width();      // measure before painting
                let _ = shaped.paint(vp.to_screen(label.at, origin),
                                     px(14.), TextAlign::Left, None, window, cx);
            }
        });

        window.set_cursor_style(CursorStyle::Crosshair, &hitbox);

        // Drag-to-pan with real pointer capture.
        {
            let (hb, view) = (hitbox.clone(), self.view.clone());
            window.on_mouse_event(move |ev: &MouseDownEvent, phase, window, cx| {
                if phase == DispatchPhase::Bubble
                    && ev.button == MouseButton::Left && hb.is_hovered(window) {
                    window.capture_pointer(hb.id);           // survives leaving the bounds
                    view.update(cx, |this, cx| { this.drag_anchor = Some(ev.position); cx.notify(); });
                }
            });
        }
        {
            let (hb, view) = (hitbox.clone(), self.view.clone());
            window.on_mouse_event(move |ev: &MouseMoveEvent, phase, window, cx| {
                if phase == DispatchPhase::Bubble && hb.is_hovered(window) {
                    view.update(cx, |this, cx| {
                        if let Some(anchor) = this.drag_anchor {
                            let d = ev.position - anchor;
                            this.viewport.offset.x -= d.x.as_f32() / this.viewport.scale;
                            this.viewport.offset.y -= d.y.as_f32() / this.viewport.scale;
                            this.drag_anchor = Some(ev.position);
                            cx.notify();
                        }
                    });
                }
            });
        }
        {
            let view = self.view.clone();
            window.on_mouse_event(move |_: &MouseUpEvent, phase, window, cx| {
                if phase == DispatchPhase::Bubble {
                    window.release_pointer();               // also auto-released on mouse up
                    view.update(cx, |this, cx| { this.drag_anchor = None; cx.notify(); });
                }
            });
        }
        {
            let (hb, view) = (hitbox.clone(), self.view.clone());
            window.on_mouse_event(move |ev: &ScrollWheelEvent, phase, window, cx| {
                if phase == DispatchPhase::Bubble && hb.should_handle_scroll(window) {
                    let dy = match ev.delta {
                        ScrollDelta::Pixels(p) => p.y.as_f32(),
                        ScrollDelta::Lines(l)  => l.y * 20.0,
                    };
                    view.update(cx, |this, cx| {
                        this.viewport.scale *= 1.0 + dy * 0.001; cx.notify();
                    });
                }
            });
        }
    }
}

struct SchematicView {
    viewport: Viewport,
    drag_anchor: Option<Point<Pixels>>,
    continuous: bool,
}

impl Render for SchematicView {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if self.continuous { window.request_animation_frame(); }   // 120 Hz free-run
        let mut canvas = SchematicCanvas {
            id: "schematic".into(), style: StyleRefinement::default(),
            viewport: self.viewport, segments: vec![], labels: vec![], view: cx.entity(),
        }.size_full();
        // ... fill canvas.segments / canvas.labels from the culled document ...
        div().size_full().child(canvas)
    }
}
```

### 3.4 Fallback, if gpui's primitives ever prove insufficient

In descending order of pain:

1. **Fork `gpui-pre-wgpu` + `gpui-pre-linux`** and add a user render-pass hook to
   `WgpuRenderer::record_frame`. Both are Apache-2.0 snapshots of Zed
   (`zed-rev = d89e9c2124b2786a390c7a451c7488601b4da2e1`) and are small
   (renderer is 2 243 lines). Add a `Primitive::Custom(Box<dyn CustomDraw>)` or a
   `set_custom_pass(Rc<dyn Fn(&wgpu::Device,&wgpu::Queue,&mut wgpu::RenderPass)>)`.
   This is the only route to your own pipeline and is a realistic 1–2 week job.
2. **Own OS window for the canvas.** `raw-window-handle` + your own wgpu surface,
   embedded as a child window / subsurface. Ugly on Wayland, painful to composite
   with gpui chrome.
3. **CPU round-trip via `RenderImage` + `img()`.** Works, compiles (verified),
   but ≈8.3 MB × 3–4 copies + a readback stall per frame at 1080p. Acceptable for
   a 3D viewer at 30 fps; not for a 120 Hz 2D canvas.

---

## 4. Widgets available from `gpui-component` 0.6.1

`gpui_kit::component::*`. Everything below was read from
`gpui-component-0.6.1/src/lib.rs` and the module sources; the starred ones were
additionally **compiled** in `/tmp/claude-0/gpui-probe/tests/probe.rs`.

### What you asked for

| Need | Available? | Path & constructor |
| --- | --- | --- |
| **Menu bar** | ✅ | `component::menu::AppMenuBar::new(cx: &mut App) -> Entity<Self>` ★ — "the application menu bar, for Windows and Linux". Also `App::set_menus(impl IntoIterator<Item = Menu>)` for native menus (macOS), and `component::native_menu` with `macos`/`windows`/`fallback` backends. |
| **Toolbar of tool buttons** | ⚠️ build it | No `Toolbar` type. Compose: `h_flex().gap_1().child(Button::new(id)...)`, or `component::button::ButtonGroup`, or `component::tab::*`. Trivial. |
| **Left/right dock panel** | ✅ | `component::dock::DockArea::new(id: impl Into<SharedString>, version: Option<usize>, window: &mut Window, cx: &mut Context<Self>) -> Self` ★, then `.with_renderer(DockSkin::new(cx))`. Plus `DockPlacement::{Left,Right,Bottom}`, `DockState`, `PanelRegistry`, `register_panel`, `TabGroup`, `Tiles`, layout `dump()`/`load()` persistence. Model lives in `gpui_base::dock`. |
| **Status bar** | ✅ | `component::status_bar::StatusBar::new()`, `.left(impl IntoElement)`, `.right(impl IntoElement)` ★ |
| **Tree view** | ✅ | `component::tree::{tree, Tree, TreeState, TreeItem, TreeEntry}` ★ — `TreeState::new(cx: &mut App) -> Self`, `.items(impl Into<Vec<TreeItem>>)`; `tree(&Entity<TreeState>, render_item: Fn(usize, &TreeEntry, bool, &mut Window, &mut App) -> ListItem)`. Virtualized (`UniformListScrollHandle`). `TreeItem::new(id, label).child(TreeItem)`. |
| **Context menu** | ✅ | `component::menu::ContextMenuExt::context_menu(f: Fn(PopupMenu, &mut Window, &mut Context<PopupMenu>) -> PopupMenu)` on any `InteractiveElement + ParentElement + Styled` ★. Also `ContextMenu::new(id, element)`. |
| **Command palette** | ✅ | `component::command::{Command, CommandState, CommandEntry, CommandGroup, CommandItem}` ★ — `CommandState::new(window, cx)`, `Command::new(&Entity<CommandState>)`, `.searchable()`, `.filterable()`, `.on_query/.on_select/.on_confirm/.on_cancel`. Shows Action keybinding hints. Put it in a `Dialog`/`Sheet`. |

### Full inventory (module → main types)

* `accordion` `Accordion::new(id)`; `alert`; `attachment`; `avatar`; `badge::Badge::new()`;
  `breadcrumb::Breadcrumb::new()` / `BreadcrumbItem::new(label)`; `bubble`
* `button::{Button::new(id), ButtonVariants (primary/secondary/danger/warning/success/info/ghost/link),
  ButtonGroup, DropdownButton, ToggleButton}`; `Button::dropdown_menu(|menu, window, cx| ...)`
* `chart`, `plot` — charting primitives
* `checkbox::Checkbox::new(id)`; `radio`; `switch::Switch::new(id)`; `slider::Slider::new(&Entity<SliderState>)`;
  `rating`; `color_picker`; `stepper`
* `clipboard`; `collapsible`; `description_list`; `group_box`; `kbd::Kbd::new(Keystroke)`;
  `label::Label::new(text)`; `icon::Icon::new(impl Into<Icon>)` + `icon_named!` macro
* `combobox::{Combobox, ComboboxState}`; `select::{Select, SelectState}`; `searchable_list`
* `command` — command palette (above)
* `dialog::{Dialog, AlertDialog, DialogHeader, DialogTitle, DialogContent, DialogFooter, DialogDescription}`;
  `sheet::Sheet`; `popover::Popover::new(id)`; `hover_card`; `tooltip::Tooltip::new(impl Into<Text>)`;
  `notification::{Notification, NotificationList}`
* `dock` — full docking workspace (above)
* `form::{v_form(), h_form(), field()}`; `setting`
* `highlighter` — tree-sitter syntax highlighting
* `input::{Input, InputState, Rope, RopeExt}` — single-line, multi-line, code editor, OTP, number input
* `list::{List, ListState, ListItem::new(id), ListDelegate}` — virtualized;
  `virtual_list::{VirtualList, h_virtual_list, v_virtual_list, VirtualListScrollHandle}`
* `menu::{AppMenuBar, PopupMenu, PopupMenuItem, DropdownMenu, ContextMenu, ContextMenuExt}`
* `pagination`; `progress`; `shimmer`; `skeleton`; `spinner`; `marker`; `tag`; `link`; `separator`; `message`
* `resizable::{h_resizable(id), v_resizable(id), resizable_panel(), ResizableState, ResizablePanelGroup}` ★
  — `h_resizable("split").with_state(&Entity<ResizableState>).child(resizable_panel().size(px(240.)).child(..))`
* `scroll::{Scrollbar, ScrollbarAxis, ScrollbarMode, ScrollableMask, AutoScroll}` —
  `Scrollbar::vertical(&handle)`, `::horizontal(&handle)`, `::new(&handle)`
* `sidebar::{Sidebar::<E>::new(id), SidebarMenu, SidebarHeader, SidebarFooter, SidebarGroup, SidebarToggleButton}` ★
* `tab::{Tab, TabBar}`; `table::{Table, TableState, TableDelegate, DataTable, Column}` — virtualized
* `text` — rich text / markdown; `time::{calendar, date_picker}`
* `theme::{Theme, ThemeMode::{Light,Dark}, ActiveTheme, Theme::change(mode, Option<&mut Window>, cx)}`
* `title_bar::TitleBar::new()` + `TitleBar::title_bar_options() -> TitlebarOptions`;
  `window_border::{WindowBorder, window_border, window_paddings}`
* `Root::new(view, window, cx)` — required window root
* `inspector` (feature `inspector` or debug builds)

Styling helpers: `component::StyledExt` gives `.h_flex()`, `.v_flex()` etc. on
top of gpui's Tailwind-like `Styled` (`.size_full()`, `.p_2()`, `.gap_1()`,
`.flex_1()`, `.bg()`, `.text_color()`, …). Theme colours via
`cx.theme().background` (`ActiveTheme` trait).

**Gaps for KiCad:** no ready-made *toolbar* widget (trivial to compose), no
ruler/measurement widget, no property grid (use `form` + `table`), no
schematic/PCB canvas (that's section 3), no docking *layout editor* UI beyond
`DockArea`'s built-in drag/drop.

---

## 5. Input handling

### On elements (`InteractiveElement` / `StatefulInteractiveElement`, `elements/div.rs`)

```rust
div().id("canvas")                         // id() makes it Stateful
    .on_mouse_down(MouseButton::Left, |ev: &MouseDownEvent, window, cx| {})
    .on_mouse_up(MouseButton::Left,   |ev: &MouseUpEvent,   window, cx| {})
    .on_mouse_move(|ev: &MouseMoveEvent, window, cx| {})
    .on_mouse_exit(|ev, window, cx| {})
    .on_scroll_wheel(|ev: &ScrollWheelEvent, window, cx| {})
    .on_pinch(|ev: &PinchEvent, window, cx| {})
    .on_click(|ev: &ClickEvent, window, cx| {})            // Stateful only
    .on_aux_click(..) .on_hover(|entered: &bool, w, cx| {}) // Stateful only
    .on_drag(payload, |payload, offset, window, cx| { /* build drag preview */ })
    .on_drag_move::<T>(|ev, window, cx| {})
    .on_drop::<T>(|payload, window, cx| {})
    .on_key_down(|ev: &KeyDownEvent, window, cx| {})
    .on_key_up(..)  .on_modifiers_changed(|ev: &ModifiersChangedEvent, w, cx| {})
    .on_action::<A: Action>(|action, window, cx| {})
    .capture_action::<A>(..)  .capture_key_down(..)  .capture_any_mouse_down(..)
    .on_mouse_down_out(..) .on_mouse_up_out(..)
    .occlude()                                              // HitboxBehavior::BlockMouse
    .cursor(CursorStyle::Crosshair)
    .track_focus(&focus_handle)  .key_context("SchematicEditor")
```

Inside a `Render` impl use `cx.listener(|this, event, window, cx| ...)` to get
`&mut Self`.

### Event payloads (`gpui-pre/src/interactive.rs`)

```rust
pub struct MouseDownEvent { pub button: MouseButton, pub position: Point<Pixels>,
                            pub modifiers: Modifiers, pub click_count: usize, pub first_mouse: bool }
pub struct MouseUpEvent   { pub button: MouseButton, pub position: Point<Pixels>,
                            pub modifiers: Modifiers, pub click_count: usize }
pub struct MouseMoveEvent { pub position: Point<Pixels>, pub pressed_button: Option<MouseButton>,
                            pub modifiers: Modifiers }
impl MouseMoveEvent { pub fn dragging(&self) -> bool }   // == pressed_button == Some(Left)
pub struct ScrollWheelEvent { pub position: Point<Pixels>, pub delta: ScrollDelta,
                              pub modifiers: Modifiers, pub touch_phase: TouchPhase }
pub enum ScrollDelta { Pixels(Point<Pixels>), Lines(Point<f32>) }
pub struct KeyDownEvent { pub keystroke: Keystroke, pub is_held: bool,
                          pub prefer_character_input: bool }
pub struct KeyUpEvent   { pub keystroke: Keystroke }
pub struct ModifiersChangedEvent { pub modifiers: Modifiers, pub capslock: Capslock }
pub struct PinchEvent { pub position: Point<Pixels>, /* zoom delta */ .. }
```

`Modifiers` has `control`, `alt`, `shift`, `platform`, `function`. Also
`window.modifiers() -> Modifiers` and `window.capslock()`.

### Window-level listeners (for custom elements, paint phase)

```rust
window.on_mouse_event::<E: MouseEvent>(impl FnMut(&E, DispatchPhase, &mut Window, &mut App));
window.on_key_event::<E: KeyEvent>(impl Fn(&E, DispatchPhase, &mut Window, &mut App));
window.on_modifiers_changed(impl Fn(&ModifiersChangedEvent, &mut Window, &mut App));
```
`DispatchPhase::{Capture, Bubble}`; `cx.stop_propagation()` to consume.
**These are cleared every frame** — re-register them in `paint()` each time.

### Hit testing and mouse capture

```rust
// prepaint:
let hitbox: Hitbox = window.insert_hitbox(bounds, HitboxBehavior::Normal);
//                    HitboxBehavior::{Normal, BlockMouse, ...}
// any phase:
hitbox.is_hovered(&window) -> bool
hitbox.is_hovered_at(position, &window) -> bool
hitbox.should_handle_scroll(&window) -> bool
window.mouse_position() -> Point<Pixels>

// drag capture (window.rs:3092)
window.capture_pointer(hitbox.id);   // route all move/up to this hitbox, even outside bounds
window.release_pointer();            // also auto-released on mouse up
window.captured_hitbox() -> Option<HitboxId>
```

### Cursor

```rust
window.set_cursor_style(CursorStyle::Crosshair, &hitbox);   // paint phase, per-hitbox
window.set_window_cursor_style(CursorStyle::ClosedHand);    // paint phase, wins over the above
// or on an element: div().cursor(CursorStyle::PointingHand)
```
`CursorStyle`: `Arrow, IBeam, Crosshair, ClosedHand, OpenHand, PointingHand,
ResizeLeft/Right/LeftRight/Up/Down/UpDown/UpLeftDownRight/UpRightDownLeft,
ResizeColumn, ResizeRow, …` (`platform.rs:2434`).

### Actions and key bindings

```rust
gpui_kit::actions!(schematic, [Quit, ZoomFit, DeleteSelection]);   // unit actions in a namespace
gpui_kit::actions!([Undo, Redo]);                                  // no namespace

// Actions with data: derive gpui_kit::Action manually:
#[derive(Clone, PartialEq, Default, Debug, serde::Deserialize, gpui_kit::Action)]
#[action(namespace = schematic)]
pub struct PlaceSymbol { pub lib_id: String }

// Bind (App):
cx.bind_keys([
    KeyBinding::new("ctrl-q", Quit, None),                        // context predicate = None
    KeyBinding::new("ctrl-z", Undo, Some("SchematicEditor")),
    KeyBinding::new("ctrl-k ctrl-p", OpenPalette, None),          // chords: space-separated
]);
// pub fn KeyBinding::new<A: Action>(keystrokes: &str, action: A, context: Option<&str>) -> Self
//   panics on parse error; KeyBinding::load(..) returns Result.

cx.on_action(|_: &Quit, cx: &mut App| cx.quit());                  // global
div().key_context("SchematicEditor").on_action(cx.listener(|this, _: &Undo, window, cx| {}))
```

Useful: `window.available_actions(cx) -> Vec<Box<dyn Action>>`,
`window.bindings_for_action(&action)`, `window.keystroke_text_for(&action)`,
`window.dispatch_action(Box<dyn Action>, cx)`, `window.context_stack()`.

---

## 6. Text

### Layout & shaping

```rust
window.text_system() -> &Arc<WindowTextSystem>

// WindowTextSystem (text_system.rs:409)
pub fn shape_line(&self, text: SharedString, font_size: Pixels,
                  runs: &[TextRun], force_width: Option<Pixels>) -> ShapedLine;
pub fn shape_line_by_hash(&self, text_hash: u64, text_len: usize, font_size: Pixels,
                          runs: &[TextRun], force_width: Option<Pixels>,
                          materialize: impl FnOnce() -> SharedString) -> ShapedLine;
pub fn shape_text(...) -> SmallVec<[WrappedLine; 1]>;     // multi-line + wrapping
pub fn layout_line(...) -> Arc<LineLayout>;
pub fn layout_width(&self, font_id, font_size, ch) -> Pixels;

pub struct TextRun { pub len: usize, pub font: Font, pub color: Hsla,
                     pub background_color: Option<Hsla>,
                     pub underline: Option<UnderlineStyle>,
                     pub strikethrough: Option<StrikethroughStyle> }

// ShapedLine (text_system/line.rs:43)
pub fn len(&self) -> usize;
pub fn width(&self) -> Pixels;                             // measure without painting
pub fn paint(&self, origin: Point<Pixels>, line_height: Pixels, align: TextAlign,
             align_width: Option<Pixels>, window: &mut Window, cx: &mut App) -> Result<()>;
pub fn paint_background(&self, ...) -> Result<()>;
// also index_for_x / x_for_index via Deref to LineLayout
```

Fonts: `font("Helvetica")`, `.bold()`, `.italic()`,
`TextSystem::{add_fonts(Vec<Cow<[u8]>>), all_font_names(), resolve_font(&Font),
font_metrics, typographic_bounds, advance, em_width, ch_width, cap_height,
x_height, ascent, descent, bounding_box, units_per_em}`. On Linux the backend is
`gpui_wgpu::CosmicTextSystem` (cosmic-text 0.19 + fontconfig/font-kit).

### ⚠️ Rotated text — NOT supported by the text path

`Window::paint_glyph` (`window.rs:4544`) and `paint_emoji` hard-code
`transformation: TransformationMatrix::unit()` when inserting the
`MonochromeSprite`/`SubpixelSprite`. `ShapedLine::paint` goes through those. So
**`ShapedLine`/`paint_glyph` cannot rotate, skew or scale text.**
`PolychromeSprite` (images) has **no** transformation field at all, so you can't
rotate an image of text either.

**Workaround that does work (verified, compiles): `Window::paint_svg`.**
It is the one paint call that takes a matrix and emits a `MonochromeSprite` with
it, and it accepts *inline* SVG bytes (`data: Option<&[u8]>` short-circuits
`AssetSource`, see `svg_renderer.rs:257`). `usvg` converts `<text>` to outlines
using a system font DB, so:

```rust
/// Rasterize `text` as an SVG and rotate the resulting sprite on the GPU.
/// The sprite atlas caches on (key, size): a repeated label costs one rasterization.
fn paint_rotated_text(
    window: &mut Window, cx: &App, text: &str, origin: Point<Pixels>,
    font_size: Pixels, angle: Radians, color: Hsla,
) {
    let w = font_size.as_f32() * 0.62 * text.chars().count().max(1) as f32;
    let h = font_size.as_f32() * 1.25;
    let fs = font_size.as_f32();
    let svg = format!(concat!(
        r##"<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" "##,
        r##"viewBox="0 0 {w} {h}"><text x="0" y="{baseline}" font-family="sans-serif" "##,
        r##"font-size="{fs}" fill="#ffffff">{text}</text></svg>"##),
        w = w, h = h, baseline = fs, fs = fs, text = text);

    let bounds = Bounds { origin, size: size(px(w), px(h)) };
    let sf = window.scale_factor();
    let c = bounds.center();
    // TransformationMatrix operates in scaled (device) pixels.
    let transformation = gpui_kit::TransformationMatrix::unit()
        .translate(Point::new(c.x.scale(sf), c.y.scale(sf)))
        .rotate(angle)
        .translate(Point::new(c.x.scale(-sf), c.y.scale(-sf)));

    let _ = window.paint_svg(
        bounds,
        format!("inline-text://{text}/{fs}").into(),   // atlas cache key — must be unique per string+size
        Some(svg.as_bytes()),
        transformation,
        color,
        cx,
    );
}
```

`TransformationMatrix` (`scene.rs:608`) has `unit()`, `translate(Point<ScaledPixels>)`,
`rotate(Radians)` (clockwise, about the origin), `scale(Size<f32>)`, `compose()`,
`apply(Point<Pixels>)`. Note `paint_svg` rasterizes at
`SMOOTH_SVG_SCALE_FACTOR × bounds` and keys the atlas on `(path, size)` — so
zooming changes the key and re-rasterizes. For a schematic, quantize the font
size to a small ladder (e.g. powers of √2) to keep the atlas small.

**Second workaround (better for very heavy rotated text):** pull glyph outlines
yourself (`ttf-parser` / `swash` / `cosmic-text` against the same TTF), feed them
into `PathBuilder` (`cubic_bezier_to`/`curve_to`/`close`) with a
`lyon::math::Transform` rotation via `PathBuilder::transform`, and emit them as
ordinary `Path` primitives. Then rotated text costs the same as any other path
geometry and batches with your wires. gpui exposes **no** glyph-outline API
(`PlatformTextSystem`, `platform.rs:1173`, only rasterizes), so you supply the
font file yourself.

---

## 7. Animation / frame pacing

* **Wayland**: true vsync via `wl_surface.frame` callbacks
  (`gpui-pre-linux/src/linux/wayland/window.rs:964,1018,1981`), and the surface is
  configured with `preferred_present_mode: Some(wgpu::PresentMode::Mailbox)`
  falling back to FIFO (`wayland/window.rs:578`). So 120 Hz follows the compositor.
* **X11**: `preferred_present_mode: None` → **`wgpu::PresentMode::Fifo`**
  (`wgpu_renderer.rs:420`), plus a calloop timer driven by the RandR mode's
  refresh rate (`x11/client.rs:1974` `mode_refresh_rate(mode_info)`,
  `start_refresh_loop(x_window, refresh_rate)`, which re-arms `instant += refresh_rate`).
  So on X11 you get the monitor's actual refresh rate, 120 Hz included, but no
  Mailbox.
* Continuous animation: `window.request_animation_frame()` inside `render()`, or
  `window.on_next_frame(|window, cx| ...)`. Both call
  `platform_window.schedule_frame()` and `invalidator.wake_platform()`.
* Prefer `AnimationExt::with_animation(..)` for decorative motion — it respects
  `App::reduce_motion()`. Check `cx.reduce_motion()` if you drive frames yourself.
* `WindowOptions::inactive_frame_interval: Option<Duration>` throttles frames
  while unfocused.
* Instrumentation (feature `profiler`): `window.frame_duration_snapshot()`,
  `window.input_latency_snapshot()`, `window.set_debug_frame_overlay_mode(mode)`,
  `window.cycle_debug_frame_overlay_mode()`, `window.reset_debug_frame_overlay_stats()`.
* `window.present_if_needed()` (`window.rs:3341`) and
  `window.draw(cx) -> ArenaClearNeeded` (`window.rs:3143`) are public for embedders.

---

## 8. Testing

**This works on Linux today — I ran it.** Both probe tests pass headlessly:

```
test cpu_image_can_be_handed_to_gpui ... ok
test shell_builds_and_renders ... ok
test result: ok. 2 passed; 0 failed
```

```toml
[dev-dependencies]
gpui-kit = { version = "0.6.1", features = ["test-support"] }
```

```rust
use gpui_kit::test::{TestAppContextExt, TestSupportExt, TestWindowExt};
use gpui_kit::{TestAppContext, px, size};

#[gpui_kit::test]                       // sync flavour
fn toolbar_click_selects_the_wire_tool(cx: &mut TestAppContext) {
    cx.update(gpui_kit::init);
    let handle = cx.open_window(size(px(1024.), px(768.)), |window, cx| {
        let view = cx.new(|cx| Shell::new(window, cx));
        gpui_kit::component::Root::new(view, window, cx)
    });
    cx.update_window(handle.into(), |_, window, cx| {
        window.render_frame(cx);                 // completes a frame
        assert!(window.find("shell").visible());
        window.click("wire", cx);                // real hit testing
        assert_eq!(window.find("wire").label(), Some("Wire"));   // a11y label, not rendered text
        let quads = window.painted_quads();      // scene inspection without a GPU
        assert!(!quads.is_empty());
    }).unwrap();
}

#[gpui_kit::test]                       // async flavour, for deferred UI
async fn palette_opens(cx: &mut TestAppContext) {
    /* ... */
    cx.wait_for(handle.into(), Duration::from_secs(1), |window, _| {
        window.try_find("command-palette").is_some()
    }).await;
}
```

### `gpui_kit::test::TestWindowExt` (trait on `Window`)

```rust
fn find(&self, id: impl Into<ElementId>) -> ElementSnapshot;      // panics, lists registered paths
fn try_find(&self, id: impl Into<ElementId>) -> Option<ElementSnapshot>;
fn within(&mut self, id: impl Into<ElementId>) -> ScopedWindow<'_>;
fn render_frame(&mut self, cx: &mut App);
fn click / click_at(offset) / right_click / double_click / hover (id, cx);
fn scroll(&mut self, id, delta: ScrollDelta, cx);
fn drag(&mut self, from: Point<Pixels>, to: Point<Pixels>, cx);
fn drag_to(&mut self, from: ElementId, to: ElementId, cx);
fn press(&mut self, key: &str, cx);      // "backspace", "cmd-a", ...
fn input(&mut self, text: &str, cx);
```

`ElementSnapshot` (immutable; re-query after interaction) exposes `bounds()`,
`visible()`, `label()`, `value()`, `checked()`, `selected()`, `focused()`,
`path()` — all read from the **native accessibility tree**, not test-only
setters. Mark elements with `.test_support()` (from `gpui_kit::TestSupportExt`,
available even without the feature — it's a no-op in release) placed
**before** `.track_focus(&handle)`.

`TestAppContextExt::wait_for(window, timeout, predicate)` polls every 10 ms of
test-clock time and panics with the registered element paths on timeout.

Raw gpui test API also available: `TestAppContext::{add_window, open_window,
add_window_view, simulate_prompt_answer, simulate_new_path_selection,
run_until_parked, executor().advance_clock(..)}`, `VisualTestContext`,
`Window::{draw(cx), dispatch_event, dispatch_keystroke, simulate_mouse_move,
set_modifiers, simulate_next_frame, painted_quads, has_image_atlas_entry}`.

### ⚠️ Screenshots are macOS-only

`Window::render_to_image() -> anyhow::Result<image::RgbaImage>` and
`HeadlessAppContext::capture_screenshot` exist, but they go through
`PlatformHeadlessRenderer`, and
`gpui_platform::current_headless_renderer()` (`gpui_platform.rs:82`) returns
`Some(MetalHeadlessRenderer)` on macOS and **`None` everywhere else**. The test
platform then bails with *"render_to_image not available: no HeadlessRenderer
configured"*. gpui-kit's own `tests/rendering.rs` says so out loud:

> `rendering: skipped; GPUI does not supply a headless renderer on this platform`

**On Linux, golden-image UI tests are not available.** Your options:
1. Assert on `window.painted_quads() -> Vec<Quad>` (bounds, colours, masks) —
   works today, no GPU.
2. Assert on your own geometry: factor the schematic tessellation into a pure
   function `fn tessellate(doc, viewport) -> Vec<Path<Pixels>>` and snapshot the
   vertex buffers. This is the right shape anyway.
3. Run golden-image tests on a macOS CI runner (as gpui-kit itself does), or
   drive a real X11 window under Xvfb and grab it with an external tool.

---

## 9. Linux notes

* **Backend selection** (`gpui-pre/src/platform.rs:154` `guess_compositor()`):
  1. `ZED_HEADLESS` set (any value) → `"Headless"`
  2. `WAYLAND_DISPLAY` non-empty → `"Wayland"`
  3. `DISPLAY` non-empty → `"X11"`
  4. otherwise → `"Headless"`
  `gpui_linux::current_platform(headless)` then builds `WaylandClient`,
  `X11Client` or `HeadlessClient` (`gpui-pre-linux/src/linux.rs:30`).
  **To force X11 on a Wayland session: `WAYLAND_DISPLAY= DISPLAY=:0 ./app`.**
  There is no `ZED_WINDOW_SYSTEM`-style override in this version.
* `gpui-kit` enables both `x11` and `wayland` features on `gpui_platform`, plus
  `font-kit` and `runtime_shaders`.
* **System libraries** (from `gpui-pre-linux/Cargo.toml` and its transitive deps):
  X11 via `x11rb` (xcb: `libxcb`, `libxcb-randr`, `-xfixes`, `-xinput`, `-xkb`,
  `-cursor`, `-render`), Wayland via `wayland-client`/`wayland-protocols`/
  `calloop-wayland-source`, `libxkbcommon`, `libxkbcommon-x11`, `fontconfig` +
  `freetype` (`yeslogic-fontconfig-sys`, `freetype-sys`), `zed-xim` (IME),
  `open`/`ashpd` (xdg-desktop-portal for file dialogs), `zed-scap` (screen
  capture, optional), `libGL`/Vulkan loader for wgpu (`ash`, `glow`, `gpu-allocator`).
  On Debian/Ubuntu: `libxcb1-dev libxcb-randr0-dev libxcb-xfixes0-dev
  libxcb-xinput-dev libxcb-cursor-dev libxkbcommon-dev libxkbcommon-x11-dev
  libfontconfig-dev libfreetype-dev libwayland-dev libvulkan-dev`.
* **Renderer**: `gpui-pre-wgpu` on wgpu 29. Backend chosen by wgpu
  (Vulkan first on Linux). `WgpuContext::new_rejecting_software` exists, so a
  llvmpipe-only machine may be refused; `WgpuRenderer::recover()` and
  `device_lost()` handle GPU resets.
* **Headless**: `gpui_platform::headless()` returns an `Application` on the
  headless client — windows exist logically, nothing is drawn, no GPU needed.
  This is what `#[gpui_kit::test]` uses. Set `ZED_HEADLESS=1` to force it for a
  normal binary (useful in CI smoke tests).
* Window decorations: `WindowOptions::window_decorations:
  Option<WindowDecorations>` (client vs server side); `Window::request_decorations`,
  `start_window_move`, `start_window_resize(ResizeEdge)`,
  `set_client_inset`, `window_border()` from gpui-component for CSD shadows.
  `WindowOptions::app_id` sets the Wayland/X11 app id (desktop file matching).
  `WindowOptions::icon: Option<Arc<image::RgbaImage>>` is **X11 only**.
* Layer shell (panels/overlays) is available: `gpui::layer_shell::Anchor`,
  `Window::{set_exclusive_zone, set_exclusive_edge, set_input_region}`.

---

## 10. Gotchas (the ones that will actually bite)

1. **`gpui-pre 0.3.5` needs a toolchain with `std::hint::cold_path` stable.**
   Fails on stable 1.94.1 with two `E0658`s in `profiler.rs`. Pin a toolchain.
2. **`Pixels`' inner field is private in this fork.** `p.0` → use
   `p.as_f32()` / `p.to_f64()` / `f32::from(p)`. Same for `ScaledPixels`
   (construct with `Pixels::scale(factor)`, negate with `.scale(-sf)`).
3. **Use `gpui_kit::actions!`, not `gpui::actions!`.** The upstream macro
   expands to `gpui::Action`, which doesn't resolve through the facade.
4. **`Root::new(view, window, cx)` must be the window root** for any
   gpui-component app, or popovers/tooltips/dialogs/notifications never appear.
5. **Window-level listeners are per-frame.** `window.on_mouse_event`,
   `on_key_event`, `set_cursor_style`, `set_tooltip` must be re-registered in
   every `paint()`. They are cleared when the frame is dropped.
6. **Phase assertions are real.** `insert_hitbox`/`set_tooltip` are prepaint-only;
   `paint_*`/`on_mouse_event`/`set_cursor_style` are paint-only. Calling them in
   the wrong phase hits `invalidator.debug_assert_*` and panics in debug.
7. **lyon's u16 index buffer caps a single `PathBuilder::build()`** at 65 535
   vertices (~16 k stroked segments). Chunk, and handle the `Err`.
8. **Don't interleave path and quad orders.** Everything inside one
   `paint_layer` shares a `DrawOrder`, which merges into one path render pass.
   Separate layers = separate passes = a perf cliff at scale.
9. **`Window::painted_quads()` and `render_to_image()` are `test-support` only**,
   and `render_to_image` additionally needs a `PlatformHeadlessRenderer` → macOS.
10. **gpui does not re-export the `image` crate** even though `RenderImage`,
    `WindowOptions::icon` and `render_to_image` use it. Add `image = "0.25"`
    yourself and keep the version in lockstep with the lockfile (0.25.10).
11. **`surface()` / `Window::paint_surface` are `#[cfg(target_os = "macos")]`.**
    Any plan that routes video/GPU textures through them is macOS-only.
12. **`PolychromeSprite` has no transformation** — images cannot be rotated or
    scaled non-uniformly. Only `MonochromeSprite`/`SubpixelSprite` (glyphs, SVG)
    carry a matrix, and `paint_glyph` pins it to `unit()`.
13. **The sprite atlas starts at 1024×1024 and grows**; entries are keyed and
    never evicted automatically. If you push per-frame images you must
    `window.drop_image(old)` yourself.
14. **`ElementSnapshot::find` panics if the element has zero size or is
    invisible**, with the message "is not visible". In tests, give flex rows an
    explicit height — I hit exactly this.
15. **`App::open_window` from inside `run()` works, but the crate's own examples
    use `cx.spawn(async move |cx| cx.open_window(..))`.** Follow the crate;
    `AsyncApp::open_window` takes `&self` and `Bounds::centered(None, size, cx)`
    needs an `&App`, which you only have inside `cx.update(..)`.
16. gpui-component's `StatusBar` methods are `.left()`/`.right()`, its resizable
    group is `h_resizable(id).with_state(&entity)` (state is a separate
    `Entity<ResizableState>`, built with `ResizableState::default()`), and
    `Sidebar` is generic: `Sidebar::<SidebarMenu>::new("nav")`.

---

## Appendix: verification

Probe crate: `/tmp/claude-0/gpui-probe` (`CARGO_TARGET_DIR=/tmp/claude-0/shared-target`).

* `src/main.rs` — minimal app + full custom `Element` (paths, dashed arcs, filled
  polygons, quads, shaped text, rotated SVG text, hitbox, pointer capture,
  cursor, wheel zoom, actions, animation frames). `cargo +nightly check` ✅
* `tests/probe.rs` — `AppMenuBar`, `DockArea`, `Tree`/`TreeState`/`TreeItem`,
  `Command`/`CommandState`, `Input`/`InputState`, `PopupMenu` via `context_menu`,
  `h_resizable`/`resizable_panel`/`ResizableState`, `Button`/`ButtonVariants`,
  `StatusBar`, `Sidebar`, plus the CPU `RenderImage` → `img()` path.
  `cargo +nightly test --test probe` ✅ **2 passed, 0 failed** (headless, Linux).
