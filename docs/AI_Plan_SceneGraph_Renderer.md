# PathSpace — Scene Graph and Renderer Plan

Status: Active plan (MVP in progress)
Scope: UI surfaces, renderers, presenters, multi-scene targets; atomic params and snapshot-based rendering
Audience: Engineers building UI/rendering layers and contributors adding platform backends

## Goals

- Application-scoped resources: windows, scenes, renderers, and surfaces all live under a single application root so that deleting the root tears everything down.
- Multi-scene renderers: a single renderer can render multiple scenes concurrently; consumers (surfaces/presenters) select which scene via per-target configuration.
- Window-agnostic surfaces: surfaces are offscreen render targets (software or GPU) that can be presented by multiple windows within the same application.
- Typed wiring: avoid brittle string concatenation via small C++ builder/helpers; prefer app-relative references and validate containment within the app root.
- Atomicity and concurrency: adopt “prepare off-thread, publish atomically, render from immutable snapshots” for both target parameters and scene data.
- Cross-platform path: start with software on macOS, then add Metal; keep Vulkan as a future option.

## Application roots and ownership

Applications are mounted under:

- System-owned: `/system/applications/<app>`
- User-owned: `/users/<user>/system/applications/<app>`

Everything an application needs is a subtree below the app root. No cross-app sharing of surfaces or renderers. References between components are app-relative (no leading slash) and must resolve within the app root.

## App-internal layout (standardized)

- `scenes/<scene-id>/` — authoring tree (`src/...`), immutable builds (`builds/<revision>/...`), and `current_revision`.
- `renderers/<renderer-id>/` — renderer with per-target subtrees (multi-scene capable).
- `surfaces/<surface-id>/` — offscreen render targets; coordinate with a renderer target.
- `windows/<window-id>/` — platform window shell and views (presenters).

Example (abridged):

```
/system/applications/notepad/
  scenes/
    main/
      src/...
      builds/<revision>/...
      current_revision
    settings/
      src/...
      builds/<revision>/...
      current_revision
  renderers/
    2d/
      caps
      targets/
        surfaces/editor/
          scene = "scenes/main"                # app-relative
          desc                                 # target descriptor (SurfaceDesc/TextureDesc/HtmlTargetDesc)
          desc/active                          # mirror written by renderer after reconfigure (optional)
          settings/
            inbox                              # queue of whole RenderSettings objects (write-only by producers)
            active                             # single-value mirror written by renderer after adoption (optional)
          render                               # Execution: render one frame for this target
          output/
            v1/
              software/framebuffer             # pixels + stride
              common/                          # timings, indices, etc.
                frameIndex
                revision
                renderMs
                lastError
  surfaces/
    editor/
      renderer = "renderers/2d"                # app-relative reference
      scene    = "scenes/main"
      render   # Execution: coordinates with renderer target
  windows/
    MainWindow/
      title = "Notepad — Main"
      window  # (optional provider; platform shell)
      views/
        editor/
          surface = "surfaces/editor"          # app-relative
          present  # Execution: blit/draw surface into the window
```

## Entities and responsibilities

- Window (shell)
  - Platform-native window; emits state/events (resize, focus, close).
  - Lives under `windows/<id>/window` as a provider. Unaware of rendering.
- Presenter/View (window view)
  - Lives under `windows/<id>/views/<view-id>/...`.
  - Reads its `surface` (app-relative), optionally triggers one frame on the surface, and presents the result:
    - Software: blit bytes to the native window.
    - Metal/Vulkan: draw textured quad sampling the offscreen texture/image into the window’s drawable/swapchain.
- Surface (offscreen render target)
  - Lives under `surfaces/<id>/...`.
  - Holds `renderer` and `scene` (both app-relative strings). Can be shown in any number of windows within the same app.
  - `render` execution coordinates a target-scoped render with its renderer, then exposes output (framebuffer or GPU handles).
- Renderer (multi-scene)
  - Lives under `renderers/<id>/...`.
  - Stateless w.r.t. windows; serves work per target:
    - `targets/<target-id>/scene` — app-relative scene path to render.
    - `targets/<target-id>/settings/inbox` — queue of whole RenderSettings objects (atomic via insert/take).
    - `targets/<target-id>/settings/active` — single-value mirror written by renderer after adoption (optional, for introspection).
    - `targets/<target-id>/render` — execution that renders one frame for this target.
    - `targets/<target-id>/output/v1/...` — per-target outputs and stats (software/GPU/HTML).
  - Target-id convention: use consumer’s app-local path, e.g., `surfaces/<surface-name>` or `textures/<texture-name>`.

## Atomicity and concurrency

### Render settings atomicity (per renderer target)

- Settings update queue (atomic via PathSpace queues):
  - Writers insert whole `RenderSettings` values into `settings/inbox`.
  - Renderer drains `settings/inbox` with take() and adopts only the last (last-write-wins).
  - Renderer, at a safe boundary:
    - May mirror the adopted struct to `settings/active` as a single-value replace for introspection.
  - Renderers read their adopted in-memory settings; `settings/active` is optional mirror only.

Benefits: single-path commit; readers never see half-updated params.

### Scene graph concurrency (authoring vs rendering)

- Authoring tree is mutable: `scenes/<sid>/src/...`.
- Builds (snapshots) are immutable, versioned by revision:
  - `scenes/<sid>/builds/<revision>/...` — pre-baked display list (world transforms, z-order, draw commands).
  - `scenes/<sid>/current_revision` — pointer to latest published build (single-value register).
- Build pipeline:
  1) Edits to `src` mark `dirty` and trigger a debounced layout/build.
  2) A builder execution computes a display-list snapshot off-thread.
  3) Build written under `builds/<new_revision>/...`.
  4) Publish via atomic replace of `scenes/<sid>/current_revision` to `<new_revision>`.
  5) Optionally GC old snapshots once not in use.

Benefits: renderers read a consistent build (`current_revision` latched per frame) with no global locks.

### Locking strategy

- No global locks for scene edits; builders work from `src` to a new immutable snapshot.
- Renderer param adoption uses a short local mutex; render loop reads from adopted, immutable state.
- Presenters marshal final present to the correct thread/runloop (e.g., macOS/Metal).

## Frame orchestration

Renderer render (per target):

1) Adopt settings from `settings/inbox` (drain queue; last-write-wins; optionally mirror to `settings/active`).
2) Resolve `targets/<tid>/scene` against app root; validate it stays within the same app subtree.
3) Read `scenes/<sid>/current_revision`; latch for this render.
4) Traverse `scenes/<sid>/builds/<revision>/...`, render:
   - Software: produce a framebuffer (pixels + stride).
   - GPU: render into an offscreen texture/image.
5) Write `targets/<tid>/output/v1/...` and stamp `frameIndex` + `revision`.

Presenter (per window view):

1) Read `views/<vid>/surface`; resolve to `surfaces/<sid>`.
2) Optionally call `surfaces/<sid>/render` (which:
   - Writes render settings to `renderers/<rid>/targets/surfaces/<sid>/settings/inbox`,
   - (No commit needed; updates are whole objects in a queue),
   - Triggers `renderers/<rid>/targets/surfaces/<sid>/render`).
3) Present:
   - Software: read framebuffer and blit to the window.
   - GPU: draw textured quad sampling the offscreen texture/image to the window drawable/swapchain.

Staleness policy: presenters can present last-complete outputs if a fresh frame is in-flight (configurable freshness threshold).
Pacing:
- Default: follow the display device’s refresh/vsync for the window/surface (variable refresh compatible).
- HTML: use requestAnimationFrame.
- Headless/offscreen: on-demand; if continuous, timer-driven execution.
- Optional user cap: effective rate = min(display refresh, user cap).

## Hierarchical coordinates and layout

- Authoring nodes in `scenes/<sid>/src/...` store local transforms, layout hints, and style.
- Snapshot builder computes:
  - World transforms and bounds.
  - Z-order and batching.
  - Text glyph runs and image resolves.
  - Optional clip/stencil info.
- Snapshots store pre-baked draw commands for fast traversal, and the snapshot builder materializes the DrawableBucket’s staging arrays (flat, sorted/bucketed) that correspond to the published revision.

### DrawableBucket (no widget-tree traversal at render time)

- Maintain a flat registry per scene for render-time iteration. In snapshot-driven mode, the builder populates these arrays from `scenes/<sid>/src` into `builds/<revision>`; authoring-time updates populate staging, and publishing writes a new `builds/<revision>` and updates `current_revision`.
- The renderer iterates a contiguous array (or a few arrays by layer) for visibility/culling/sorting and issuing draw commands.
- API outline (conceptual; builder/widget-facing, not used by the renderer during a frame):
  - register(widgetId) -> handle
  - update(handle, {worldTransform, boundsLocal, material, layer, z, visibility, contentEpoch, transformEpoch, drawRef})
  - deregister(handle)
  - markDirty(handle, flags)
- Entry data (per drawable):
  - Identifiers: `widgetId`, stable `handle`
  - Transforms: local and world matrices; `transformEpoch`
  - Bounds: local and world `BoundingSphere` and `BoundingBox` (AABB or optional OBB)
  - Draw metadata: layer, z, pipeline flags (opaque/alpha), material/shader id
  - Draw commands: cached command list pointer/handle + `contentEpoch` (or a prepare callback)
  - Visibility flag
- Double-buffering:
  - Keep `staging` and `active` arrays per scene. The snapshot builder/authoring side updates `staging`; when publishing a new snapshot (by updating `current_revision`), it atomically swaps `staging` to `active` for that scene.
  - The renderer latches `current_revision` at the start of a frame and reads only from the matching `active` arrays for the duration of that frame (no renderer-owned swaps).

### Transforms and hierarchy without per-frame traversal

- Keep hierarchy for authoring/layout only; propagate transforms on change, not per frame:
  - `world = parentWorld * local`
  - Update world bounds; bump `transformEpoch`
  - Push updated entry to DrawableBucket `staging` buffer; enqueue children updates if needed
- Result: render-time is O(n_visible) with no parent walks.

### Bounding volumes and culling

- Store both local/world `BoundingSphere` and `BoundingBox`:
  - Sphere for cheap broad-phase (3D; circle in 2D); `r_world = r_local * maxScale(world)`
  - AABB for tighter viewport clipping (2D) or optional OBB if rotation accuracy is needed
- Per view/camera:
  - Frustum test against sphere first; optional AABB vs viewport test for 2D
  - Maintain buckets by layer/material to improve cache locality and reduce state changes

### Draw command generation and caching

- Widgets expose either:
  - A stable `DrawCommands` object + `contentEpoch`, or
  - A “prepare” function to (re)build commands into a command allocator off-thread when `contentEpoch` changes
- Renderer requests commands only when `contentEpoch` differs from last seen (retained rendering). Software UI may still redraw every frame; evolve to dirty-rects later. Preparation of command buffers should occur off-thread where possible to avoid blocking the render loop.

### Sorting and batching

- Partition visible drawables:
  - Opaque pass: sort by material/pipeline then by z (or depth); write-friendly ordering for depth early-out
  - Alpha pass: back-to-front by z within layer
- Batch small UI ops (rects, rounded rects, images, glyph quads) into SoA buffers for software rasterization

### Minimal types (sketch)

```
struct Transform { float m[16]; }; // 3D; 2D via orthographic with z=0

struct BoundingSphere { float cx, cy, cz, r; };
struct BoundingBox { float min[3], max[3]; };

enum class BoundsKind { Sphere, Box };
struct Bounds {
  BoundsKind kind;
  BoundingSphere sphereLocal, sphereWorld;
  BoundingBox    boxLocal,    boxWorld;
};

struct DrawCommand {
  uint32_t type;           // Rect, RoundedRect, Image, TextGlyphs, Mesh, Path, ...
  uint32_t materialId;
  uint32_t pipelineFlags;  // opaque/alpha, blend, etc.
  uint32_t vertexOffset, vertexCount; // or payload handle
  // software-specific payload as needed
};

struct DrawableEntry {
  uint64_t  id;
  Transform world;
  Bounds    bounds;
  uint32_t  layer;
  float     z;
  uint32_t  pipelineFlags;
  uint32_t  materialId;
  uint64_t  transformEpoch;
  uint64_t  contentEpoch;
  const DrawCommand* cmds;
  uint32_t  cmdCount;
  bool      visible;
};
```

<<<<<<< HEAD
## Scene authoring model (C++ API)

- Authoring is done via typed C++ helpers; no JSON authoring.
- Scene content lives under `<app>/scenes/<sid>/src/...`; a commit barrier signals atomic batches.
- Minimal node kinds: `Container`, `Rect`, `Text`, `Image`.
- Transforms: 2D TRS per-node (position, rotationDeg, scale), relative to parent.
- Layout: `Absolute` and `Stack` (vertical/horizontal) in v1.
- Style: opacity (inherits multiplicatively), fill/stroke, strokeWidth, cornerRadius, clip flag (on containers).
- Z-order: by `(zIndex asc, then children[] order)` within a parent.

Authoring pattern (preferred: nested PathSpace mount)
- Build the scene in a temporary PathSpace:
  - Create a local PathSpace that is not yet mounted in the app tree.
  - Insert nodes under its `/src/...` subtree (typed inserts).
  - Set `/src/nodes/root` and `/src/root` when ready.
- Atomically publish by mounting:
  - Insert the local PathSpace into the main PathSpace at `<app>/scenes/<sid>` using `std::unique_ptr<PathSpace>`.
  - The nested space adopts the parent context/prefix; a single notify wakes waiters (e.g., the snapshot builder).
- Incremental alternative (optional):
  - For streaming updates, you may write directly under `<app>/scenes/<sid>/src/...` and optionally use a `src/commit` counter as a publish hint.

Example: atomic publish via nested PathSpace (preferred)
```
SP::PathSpace ps;

// Build scene in a local (unmounted) PathSpace
auto local = std::make_unique<SP::PathSpace>();

// Author nodes under the local space's /src subtree (typed inserts)
local->insert("/src/nodes/card", RectNode{
  .style = Style{ .fill = Color{0.29f,0.56f,0.89f,1}, .cornerRadius = 12 },
  .layout = Absolute{ .x = 40, .y = 30, .w = 200, .h = 120 }
});

local->insert("/src/nodes/title", TextNode{
  .text = TextProps{ .content = "Hello, PathSpace!", .font = "16px system-ui" },
  .layout = Absolute{ .x = 56, .y = 70 } // auto size for text
});

local->insert("/src/nodes/root", ContainerNode{
  .layout = Absolute{ .x = 0, .y = 0, .w = 800, .h = 600 },
  .children = std::vector<NodeId>{ id("card"), id("title") }
});

// Set the root reference within the local space
local->insert("/src/root", std::string{"root"});

// Atomically publish the completed scene by mounting it
ps.insert("apps/demo/scenes/home", std::move(local));
// The nested space adopts ps's context/prefix and emits a single notify to wake the builder.
```

### Stack layout (v1)

- Purpose: arrange children along one axis with spacing, alignment, and optional weight-based distribution.
- Fields:
  - `axis`: Vertical (top→bottom) or Horizontal (left→right)
  - `spacing`: gap between adjacent children (main axis)
  - `alignMain`: Start | Center | End — pack the whole sequence if there’s slack
  - `alignCross`: Start | Center | End | Stretch — cross-axis placement/sizing
  - Per-child `weight` (>= 0): shares leftover main-axis space; 0 = fixed
  - Per-child min/max hints on both axes (optional)

Main-axis sizing
1) Measure fixed items (weight==0): main size = explicit size if given, else intrinsic; clamp to min/max.
2) Compute `available = containerMain - (sumFixed + spacing*(N-1))`.
3) Distribute to weighted items: each gets `available * (weight_i / totalWeight)`.
4) Clamp weighted items to min/max iteratively; redistribute remaining `available` among unclamped weighted items until stable.
5) Overflow: if content exceeds container, no auto-shrink in v1; items overflow sequentially. If the container’s `clip` is true, overflow is clipped.

Cross-axis sizing
- If explicit cross size: use it.
- Else if `alignCross=Stretch`: child cross size = container cross size (then clamp).
- Else: use intrinsic cross size; position by `alignCross`.

Positioning
- Start with `offset = 0`, adjust by `alignMain` when totalChildrenSize < containerMain.
- Place child at `(offset, crossOffset)`, then `offset += childMain + spacing`.
- Child `Transform2D` applies after layout (visual only; does not affect layout in v1).

Z-order and hit testing
- Order within a stack remains `(zIndex asc, then children[] order)`.
- If the container has `clip=true`, descendants are clipped for draw and hit testing.

Text and images
- Text:
  - If width is implicit via Stretch on cross-axis (vertical stack) or explicit size, wrap to that width; height grows.
  - Otherwise, single-line intrinsic width/height; may overflow unless clipped.
- Image:
  - Natural size if no explicit size; otherwise apply fit mode (contain/cover/fill/none).

### Authoring publish strategies

- Atomic mount (preferred):
  - Build the scene in a temporary PathSpace and insert it at `<app>/scenes/<sid>` (unique_ptr<PathSpace>).
  - The nested space adopts the parent context/prefix and a single notify wakes waiters (e.g., the snapshot builder).
  - Avoids partial reads without extra synchronization primitives.
- Optional commit barrier (incremental authoring):
  - For streaming/incremental edits to `<app>/scenes/<sid>/src`, writers may update nodes and then bump `<app>/scenes/<sid>/src/commit` as a publish hint.
  - Builders can listen to `src/commit` to rebuild immediately; non-commit edits may be debounced (e.g., ~16ms).

#### Builder: waiting for mount or revision

- Atomic mount detection:
  - Since the parent notifies on the mount path (`<app>/scenes/<sid>`), the builder should poll for readiness of `<app>/scenes/<sid>/src/root` using short blocking reads in a loop until it appears, then proceed to build.
  - Example polling loop:
```
using namespace std::chrono_literals;

Expected<void> wait_for_scene_ready(SP::PathSpace& ps, std::string const& app, std::string const& sid) {
  const std::string rootPath = app + "/scenes/" + sid + "/src/root";
  for (;;) {
    auto root = ps.read<std::string>(rootPath, Block{50ms});
    if (root) return {};
    // Timed out or not found yet; loop again (optionally add a sleep or backoff if needed)
  }
}
```

- Incremental edits with commit barrier:
  - When authors bump `<app>/scenes/<sid>/src/commit`, builders can block on that path and rebuild when it changes:
```
using namespace std::chrono_literals;

void wait_for_commit_and_rebuild(SP::PathSpace& ps, std::string const& app, std::string const& sid) {
  const std::string commitPath = app + "/scenes/" + sid + "/src/commit";
  uint64_t last = ps.read<uint64_t>(commitPath).value_or(0);
  for (;;) {
    auto cur = ps.read<uint64_t>(commitPath, Block{500ms}); // wakes on commit writes
    if (cur && *cur != last) {
      last = *cur;
      // trigger rebuild for sid
      break;
    }
    // else continue waiting
  }
}
```

- Waiting for a new published revision:
  - After building/publishing, the builder (or renderer) can wait for `current_revision` to change:
```
using namespace std::chrono_literals;

uint64_t wait_for_new_revision(SP::PathSpace& ps, std::string const& app, std::string const& sid, uint64_t known) {
  const std::string revPath = app + "/scenes/" + sid + "/current_revision";
  for (;;) {
    auto rev = ps.read<uint64_t>(revPath, Block{250ms});
    if (rev && *rev != known) return *rev;
    // continue waiting
  }
}
```

=======
>>>>>>> ee97327 (docs: update AI_ARCHITECTURE and add scene graph renderer plan)
### Renderer loop outline

```
void renderTarget(const Camera& cam,
                  const DrawableEntry* entries, size_t count,
                  Surface& surface)
{
  // 1) Frustum cull (sphere), optional AABB vs viewport for 2D
  // 2) Partition opaque/alpha
  // 3) Sort opaque by pipeline/material then z; sort alpha back-to-front
  // 4) Opaque pass: depth-friendly or painter’s order for 2D
  // 5) Alpha pass: blend back-to-front
  // 6) Execute DrawCommands into software raster (or GPU encoder)
}
```


## Notifications and scheduling

- Edits set a scene `dirty` flag/counter and notify a layout worker (debounced).
- Renderers may watch target scene subtrees to mark targets dirty.
- Modes:
  - Explicit: surfaces/presenters trigger frames.
  - On-notify: renderer schedules frames for dirty targets.

## Safety and validation

- App-relative resolution: if a path lacks a leading slash, resolve against the app root.
- Same-app validation: after resolution, verify the target path still lies within the app root; reject otherwise.
- Platform handle hygiene:
  - Use opaque typed wrappers for CAMetalLayer*, VkImage, etc.
  - Clearly document ownership and thread affinity (present must occur on the correct thread).

## Performance notes

- Software path introduces an extra copy (renderer → presenter blit).
  - Mitigate with double-buffering and mapped memory; reuse buffers on resize when possible.
- GPU path uses an offscreen pass plus a present pass.
  - Optionally allow a “presentable surface” mode (direct-to-window) when multi-window reuse isn’t needed.
- Debounce layout and frame triggers to avoid overdraw on bursty updates.
- Memory: constrain number of retained snapshots and share resources across snapshots where safe.

## Builder/helpers (typed wiring)

Introduce small C++ helpers to avoid brittle path-string glue:

- Helpers return canonical absolute paths (strings) instead of handles (no separate “handles” layer).
- Names are simple identifiers under the app root (e.g., "main", "2d", "editor", "MainWindow").
- All functions accept names or app-relative/absolute paths where noted; helpers resolve to absolute and validate containment within the same app root.

Helper responsibilities:

- Resolve app-relative paths and validate containment.
- Manage target-id convention (`surfaces/<name>`).
- Use PathSpace atomic primitives:
  - Single-value replace for small configs (e.g., surface desc).
  - Params update queue for per-target renderer params (renderer drains via take() and adopts last).
  - Snapshot revision flip (single write) for scene publish (builder concern).
- Provide readable errors with context (target-id, frame index, snapshot revision).

### Helper API (schema-as-code; returns paths)

```/dev/null/include/pathspace/ui/Builders.hpp#L1-220
#pragma once
#include <pathspace/PathSpace.hpp>
#include <string>
#include <string_view>
#include <optional>

namespace SP::ui {

// App root is a canonical absolute path under which the app lives.
using AppRoot = std::string;

// Creation parameter structs (passed to create_* helpers)

struct SceneParams {
  std::string name;           // scene folder name under "<app>/scenes/"
  std::string description;    // optional human-readable description
};

struct RendererParams {
  std::string name;           // renderer folder name under "<app>/renderers/"
  RendererKind kind;          // Software2D, Metal2D, Vulkan2D
  std::string description;    // optional
};

struct SurfaceParams {
  std::string name;           // surface folder name under "<app>/surfaces/"
  SurfaceDesc desc;           // backend, size, format, colorSpace, etc.
  std::string renderer;       // name or path to renderer ("2d", "renderers/2d", or absolute)
};

struct WindowParams {
  std::string name;           // window folder name under "<app>/windows/"
  std::string title;          // window title
  int         width  = 0;     // initial logical width
  int         height = 0;     // initial logical height
  float       scale  = 1.0f;  // initial UI scale/DPI
  std::string background;     // optional background color name or hex
};

// Resolve a name or app-relative path against appRoot; return an absolute path.
// Behavior:
// - If 'maybeRel' has no leading '/', interpret it relative to appRoot.
//   Example: appRoot="/system/applications/notepad", maybeRel="scenes/main"
//            => "/system/applications/notepad/scenes/main"
// - If 'maybeRel' starts with '/', it must begin with appRoot (same-app containment).
//   Example: maybeRel="/system/applications/notepad/scenes/main" => OK (returned as-is)
//            maybeRel="/system/applications/other/scenes/main"   => Error (cross-app)
// - Names are allowed (no slashes) only when a helper has already chosen a base subpath.
//   Example: a helper may build "surfaces/" + name, then call this with "surfaces/editor".
// Errors:
// - Return an error if the resolved absolute path does not lie within appRoot.
Expected<std::string> resolve_app_relative(AppRoot const& appRoot, std::string_view maybeRel);

// Derive the renderer target base path for a target (unique per surface/texture within the app).
// Returns "<app>/renderers/<rendererName>/targets/surfaces/<surfaceName>" (or "targets/textures/<textureName>")
std::string derive_target_base(AppRoot const& appRoot,
                               std::string const& rendererPathAbs,
                               std::string const& targetPathAbs /* surface or texture path */);

// ----- Creation (create-or-bind; idempotent) -----
// Creates (or binds to existing) scene subtree at "<app>/scenes/<SceneParams.name>".
// Returns the absolute path to the scene root. Does not build snapshots.
Expected<std::string> create_scene(PathSpace&, AppRoot const& appRoot, SceneParams const& scene);

// Creates renderer at "<app>/renderers/<RendererParams.name>", seeds minimal caps if needed.
// Returns the absolute renderer path. Kind selects initial capability flags.
Expected<std::string> create_renderer(PathSpace&, AppRoot const& appRoot, RendererParams const& renderer);

// Creates surface at "<app>/surfaces/<SurfaceParams.name>", writes desc as a single-value config,
// links to the renderer (stored app-relative), and records targetPath for convenience.
// SurfaceParams.renderer may be a name ("2d"), app-relative ("renderers/2d"), or absolute.
// Returns the absolute surface path.
Expected<std::string> create_surface(PathSpace&, AppRoot const& appRoot, SurfaceParams const& surface);

// Creates window at "<app>/windows/<WindowParams.name>" and sets title/initial size/scale.
// The native provider can be mounted later. Returns the absolute window path.
Expected<std::string> create_window(PathSpace&, AppRoot const& appRoot, WindowParams const& window);

// ----- Wiring -----
// Attaches a surface to a window view by writing an app-relative reference at
// "<app>/windows/<windowName>/views/<viewName>/surface". The window and surface
// can be referenced by simple names or paths; the helper resolves/validates containment.
Expected<void> attach_surface_to_view(PathSpace&, AppRoot const& appRoot,
                                      std::string windowPathOrName, std::string_view viewName,
                                      std::string surfacePathOrName);

// Sets which scene the surface asks the renderer to render by writing an app-relative path
// at "<app>/surfaces/<surfaceName>/scene". Accepts name or path; resolves within appRoot.
Expected<void> set_surface_scene(PathSpace&, AppRoot const& appRoot,
                                 std::string surfacePathOrName, std::string scenePathOrName);

// ----- Params and frame trigger -----
// Updates per-target render settings.
// Mode Queue (default): inserts a full RenderSettings object into "<targetBase>/settings/inbox" as a queue element.
                        The renderer take()s and adopts the last one atomically.
// Mode ReplaceActive (debug/introspection only): writes the full struct to "<targetBase>/settings/active" as a single-value replace; renderer may mirror adopted settings here but does not adopt from this value.
enum class ParamUpdateMode { Queue, ReplaceActive };
Expected<void> update_target_settings(PathSpace&, AppRoot const& appRoot,
                                      std::string targetPathOrSpec /* e.g., targets/surfaces/<name> */,
                                      RenderSettings const& settings,
                                      ParamUpdateMode mode = ParamUpdateMode::Queue);

// Triggers a single frame for the renderer target by executing "<targetBase>/render".
// If 'overrides' is provided, applies RenderSettings first (using the chosen mode).
// Returns a FutureAny for completion tracking.
Expected<FutureAny> render_target_once(PathSpace&, AppRoot const& appRoot,
                                       std::string targetPathOrSpec,
                                       std::optional<RenderSettings> overrides = std::nullopt);

// ----- Present -----
// Presents the view "<app>/windows/<windowName>/views/<viewName>" into the window.
// Ensures the referenced surface exists and is same-app; may trigger a render if freshness policy demands.
// For software, blits the latest framebuffer; for GPU, encodes a quad to the window's drawable/swapchain.
Expected<void> present_view(PathSpace&, AppRoot const& appRoot,
                            std::string windowPathOrName, std::string_view viewName);

// ----- Introspection (optional) -----
// Reads capability summary from "<app>/renderers/<rendererName>/caps" (shape TBD; v1 caps suggested).
Expected<RendererCaps> get_renderer_caps(PathSpace const&, AppRoot const&, std::string rendererPathOrName);
// Reads surface description from "<app>/surfaces/<surfaceName>/desc".
Expected<SurfaceDesc>  get_surface_desc(PathSpace const&, AppRoot const&, std::string surfacePathOrName);

} // namespace SP::ui
```
  - `include/pathspace/ui/Builders.hpp`
  - `src/pathspace/ui/`
    - scene/
      - SceneSnapshotBuilder.{hpp,cpp}
    - renderer/
      - PathRenderer2D.{hpp,cpp}
      - DrawableBucket.{hpp,cpp}
    - surface/
      - PathSurface.hpp
      - PathSurfaceSoftware.{hpp,cpp}
      - PathSurfaceMetal.{hpp,mm}   # ObjC++ (Apple)
      - SurfaceTypes.hpp
    - window/
      - PathWindow.{hpp,mm}         # platform window shell
      - PathWindowView.{hpp,cpp|mm} # presenter
    - platform/
      - macos/…
      - win32/…
      - x11/wayland/…

- CMake options (single library, feature-gated):
  - `PATHSPACE_ENABLE_UI` (ON)
  - `PATHSPACE_UI_SOFTWARE` (ON)
  - `PATHSPACE_UI_METAL` (ON on Apple)
  - `PATHSPACE_UI_VULKAN` (OFF for now)

Example CMake integration:

```/dev/null/CMakeLists.txt#L1-80
option(PATHSPACE_ENABLE_UI "Build UI (surfaces/renderers/windows)" ON)
option(PATHSPACE_UI_SOFTWARE "Enable software surface" ON)
option(PATHSPACE_UI_METAL "Enable Metal surface/presenter (Apple)" ON)

if(PATHSPACE_ENABLE_UI)
  target_sources(PathSpace PRIVATE
    include/pathspace/ui/Builders.hpp
    src/pathspace/ui/renderer/PathRenderer2D.cpp
    src/pathspace/ui/renderer/PathRenderer2D.hpp
    src/pathspace/ui/renderer/DrawableBucket.cpp
    src/pathspace/ui/renderer/DrawableBucket.hpp
    src/pathspace/ui/scene/SceneSnapshotBuilder.cpp
    src/pathspace/ui/scene/SceneSnapshotBuilder.hpp
    src/pathspace/ui/surface/SurfaceTypes.hpp
  )
  if(PATHSPACE_UI_SOFTWARE)
    target_sources(PathSpace PRIVATE
      src/pathspace/ui/surface/PathSurfaceSoftware.cpp
      src/pathspace/ui/surface/PathSurfaceSoftware.hpp
    )
  endif()
  if(APPLE AND PATHSPACE_UI_METAL)
    enable_language(OBJCXX)
    target_sources(PathSpace PRIVATE
      src/pathspace/ui/surface/PathSurfaceMetal.mm
      src/pathspace/ui/surface/PathSurfaceMetal.hpp
      src/pathspace/ui/window/PathWindowView.mm
      src/pathspace/ui/window/PathWindowView.hpp
    )
    target_link_libraries(PathSpace PRIVATE "-framework Cocoa" "-framework Metal" "-framework QuartzCore")
    if(CMAKE_OBJCXX_COMPILER_ID MATCHES "Clang")
      set_source_files_properties(
        src/pathspace/ui/surface/PathSurfaceMetal.mm
        src/pathspace/ui/window/PathWindowView.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
      )
    endif()
  endif()
endif()
```

## Examples (creation with parameter structs)

```/dev/null/examples/ui_builders_params.cpp#L1-200
#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
using namespace SP;
using namespace SP::ui;

int main() {
  PathSpace space;
  AppRoot app = "/system/applications/notepad";

  // 1) Create a scene
  SceneParams scene{ .name = "main", .description = "Main UI scene" };
  auto scenePath = create_scene(space, app, scene).value();

  // 2) Create a renderer
  RendererParams rparams{ .name = "2d", .kind = RendererKind::Software2D, .description = "Software 2D renderer" };
  auto rendererPath = create_renderer(space, app, rparams).value();

  // 3) Create a surface linked to the renderer (renderer can be name, app-rel, or abs)
  SurfaceDesc sdesc;
  sdesc.size = {1280, 720, 2.0f};
  // sdesc.pixelFormat, sdesc.colorSpace, sdesc.premultipliedAlpha can be set as needed

  SurfaceParams sparams{ .name = "editor", .desc = sdesc, .renderer = "2d" };
  auto surfacePath = create_surface(space, app, sparams).value();

  // 4) Create a window
  WindowParams wparams{ .name = "MainWindow", .title = "Notepad — Main", .width = 1280, .height = 720, .scale = 2.0f };
  auto windowPath = create_window(space, app, wparams).value();

  // 5) Wire: attach surface to window view and pick a scene for the surface
  attach_surface_to_view(space, app, "MainWindow", "editor", "editor").value(); // names allowed
  set_surface_scene(space, app, surfacePath, "scenes/main").value();

  // 6) Update settings (queued) and render once
  RenderSettings rs; rs.surface.size_px = {1280, 720}; rs.surface.dpi_scale = 2.0f;
  update_target_settings(space, app, "renderers/2d/targets/surfaces/editor", rs).value();
  auto fut = render_target_once(space, app, "renderers/2d/targets/surfaces/editor").value();

  // 7) Present the view
  present_view(space, app, windowPath, "editor").value();
}
```

## Source layout and build integration

- Source tree:
  - `include/pathspace/ui/Builders.hpp`
  - `src/pathspace/ui/`
    - scene/
      - SceneSnapshotBuilder.{hpp,cpp}
    - renderer/
      - PathRenderer2D.{hpp,cpp}
      - DrawableBucket.{hpp,cpp}
    - surface/
      - PathSurface.hpp
      - PathSurfaceSoftware.{hpp,cpp}
      - PathSurfaceMetal.{hpp,mm}
      - SurfaceTypes.hpp
    - window/
      - PathWindow.{hpp,mm}
      - PathWindowView.{hpp,cpp|mm}
    - platform/
      - macos/…
      - win32/…
      - x11/wayland/…
- CMake options and example are listed above.
## HTML/Web output (optional adapter)
- Motivation: enable preview/export of UI scenes to browsers without changing the core pipeline.
- Approach: semantic HTML/DOM adapter that maps widgets to native elements with CSS-based layout/animation. No ray tracing, no canvas command stream, no WebGL.
- Paths (under a renderer target base):
  - `<targetBase>/output/v1/html/dom` — full HTML document as a string (may inline CSS/JS)
  - `<targetBase>/output/v1/html/css` — CSS string, if split
  - `<targetBase>/output/v1/html/assets/<name>` — optional assets (base64 or URLs)
- Mapping hints:
  - Rects/rounded-rects → div with border-radius
  - Images → img or CSS background-image on div
  - Text → DOM text (or pre-shaped glyph spans later)
  - Transforms → CSS matrix()/matrix3d()
  - Shadows → CSS box-shadow
  - Z-order → z-index stacking contexts
  - Clipping → overflow:hidden or CSS clip-path
- Example (DOM/CSS):
```/dev/null/target_outputs_html_dom_example.html#L1-80
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>PathSpace UI — target preview</title>
<style>
  .root { position: relative; width: 640px; height: 360px; background: #202020; }
  .rect { position: absolute; left: 40px; top: 30px; width: 200px; height: 120px;
          background: #4a90e2; border-radius: 12px; box-shadow: 0 8px 20px rgba(0,0,0,0.25); }
  .text { position: absolute; left: 56px; top: 50px; color: white; font: 16px/20px system-ui, sans-serif; }
</style>
</head>
<body>
<div class="root">
  <div class="rect"></div>
  <div class="text">Hello, PathSpace!</div>
</div>
</body>
</html>
```
- PathKeys additions:
  - `target_outputs_html_dom(targetBase)`
  - `target_outputs_html_css(targetBase)`
- Notes:
  - Semantic DOM/CSS only; no ray tracing or canvas/WebGL in this adapter.
  - Text fidelity improves if we pre-shape glyphs in the snapshot and emit positioned spans.
  - This adapter is optional and does not affect software/GPU outputs.
<<<<<<< HEAD

<<<<<<< HEAD
## Software renderer — path tracing with tetrahedral acceleration

Status: Resolved plan for software renderer pipeline

Summary
- Scenes are often static; we prioritize incremental refinement via path tracing over tetrahedral meshes.
- Acceleration is snapshot-integrated:
  - TLAS per revision over instances; BLAS per unique geometry (deduped).
  - Tetrahedral face adjacency enables “tet-walk” traversal between neighboring tets for multi-bounce paths.
- Caching for fast edits:
  - Per-pixel PixelHitCache stores primary hit data and accumulation state; supports targeted invalidation.
  - Per-face reservoirs (ReSTIR-style) store reusable direct- and indirect-light samples to guide secondary rays.

Geometry and traversal
- Mesh: tetrahedra with precomputed face table and adjacency (two incident tets per face, boundary sentinel).
- Surface classification: a face is a “surface” when adjacent media differ (or one side is boundary); shading happens on surfaces.
- Primary rays: TLAS → BLAS (face BVH) intersection.
- Secondary rays: start from a hit face and tet-walk across faces inside the same medium; intersect surface faces; fall back to BVH if needed.

Per-pixel cache (primary visibility and accumulation)
- Store per pixel: instanceIndex, primIndex, t, barycentrics, uv, packed normal, materialId, and flags: hitValid, shadeValid.
- Invalidate selectively:
  - Material shading-only change → shadeValid = 0 for pixels using that material (no retrace).
  - Material visibility/alpha/displacement → hitValid = 0 for pixels using that material (retrace).
  - Instance transform/visibility → hitValid = 0 for that instance’s pixels.
  - Camera/viewport change → full reset (or reprojection heuristics).
- Use per-tile (e.g., 16x16) inverted indices mapping materialId/instanceIndex → bitsets of affected pixels for O(tiles) invalidation.

Per-face reservoirs and guiding (reuse across pixels/frames)
- Direct lighting: small ReSTIR DI reservoir per face; sample from it and update with new candidates each iteration.
- Indirect lighting (optional): GI reservoirs per face capturing incoming direction/radiance; guide BSDF sampling.
- Spatial reuse: for faces with sparse history, reseed from adjacent faces via tet adjacency.
- Guiding: optionally fit a lightweight directional PDF (e.g., 1–2 vMF lobes) from reservoir history; mix with BSDF and apply MIS.

Iteration pipeline (per frame/refinement step)
1) Acquire snapshot revision and settings; map TLAS/BLAS, face adjacency; allocate per-frame state.
2) For each visible pixel:
   - If hitValid, reuse primary hit; else regenerate and update PixelHitCache.
   - Direct light via DI reservoir + BSDF importance sampling (MIS).
   - Spawn secondary ray(s), guided by face GI/DI where available; walk via tet-walk until a surface is hit; accumulate contribution.
   - Update per-face reservoirs at landing faces with MIS-consistent weights.
3) Write accumulated color; update per-tile indices for invalidation.

Settings (software renderer)
- rt.enable, rt.raysPerPixel, rt.maxDepth, rt.branching (4|8), rt.leafMaxPrims, rt.quantization (none|aabb16), rt.tileSize (8|16|32)
- reuse: di.enable, gi.enable, reuseStrength
- caches: pixelCache.enable, faceReservoirs.enable, faceReservoirs.capacity (small K)
- debug: showTetWalk, showSurfaceFaces, showInvalidation, showReservoirs

Stats (output/v1/common and output/v1/rt_stats)
- cpu_build_ms, traversal_ms, shading_ms
- tlas_nodes, blas_nodes_total, rays_traced, hits, miss_rate
- retraced_pixels, reshaded_pixels, reservoir_updates

Failure modes and fallbacks
- If TLAS/BLAS not ready: render with cached primary where valid; schedule (re)build within budget; optionally hybrid raster primary as fallback.
- If adjacency is non-manifold/degenerate in a region: fall back to BVH traversal there; log once.

Cross-reference
- When core snapshot semantics or acceleration formats evolve, reflect changes in docs/AI_ARCHITECTURE.md (Snapshot Builder and Rebuild Policy).

## Tetrahedral face adjacency

Purpose
- Provide a watertight, deterministic connectivity index over tetrahedral meshes to enable fast multi-bounce path tracing (“tet-walk”) and robust physics sweeps without re-entering a BVH at every step.

Data model
- Face table (one record per unique triangle):
  - Canonical vertex triplet (deterministic winding)
  - Incident tetrahedra: up to two per face; boundary has one
  - Per-incidence metadata: local face index in the tet, winding parity vs canonical
  - Precomputed plane (n.xyz, d), optional tangent frame and area
  - Optional per-side medium/material id to classify “surface” faces (medium change)
- Tet table (one record per tetrahedron):
  - Vertex indices [a,b,c,d]
  - Global faceId[4] mapping local faces to face table
  - neighborTet[4] and neighborLocalFace[4] across each local face (or -1 for boundary)

Canonical local faces (for tet [a,b,c,d])
- f0 (opposite a): (b,c,d)
- f1 (opposite b): (a,d,c)
- f2 (opposite c): (a,b,d)
- f3 (opposite d): (a,c,b)

Build algorithm (deterministic)
- For each tet local face, compute a sorted triplet key (ascending vertex ids) to deduplicate faces.
- First incidence sets the face’s canonical winding (use the tet’s local winding).
- Second incidence fills neighbor links and winding parity.
- Compute planes once per face from canonical winding; assign medium ids per side if available.
- Validate: flag non-manifold faces (>2 incidences) and zero-area faces for fallback.

Traversal (renderer/physics)
- Path tracing secondary rays: start at a surface face; choose entering tet by sign of d·n; repeatedly intersect current tet’s face planes and cross to the next tet until reaching a surface/boundary face; only intersect triangles at surface faces. Fall back to BVH in degenerate regions.
- Physics shape-casts/CCD: same face-to-face stepping with conservative inflation; use the static TLAS for broad-phase, adjacency for narrow-phase marching.

Medium transitions (disjoint air/object)
- Keep separate adjacency for object and air; no need to stitch meshes.
- Surface portals (precomputed):
  - For each surface face, store 1..K candidate entry air tet ids on the outward side; build by offsetting a point on the face (e.g., centroid) along the outward normal by ε and locating the containing air tet via an air AABB BVH + point-in-tet test. Sample a few intra-face points for robustness.
  - At runtime, after shading with an outgoing direction into air, jump to the portal’s air tet and begin the air tet-walk from there.
- Clip-and-test fallback:
  - While walking in air (or object), compute exit distance to the next tet face; clip the segment [t_cur, t_exit] and test that short segment against the global surface-face BVH. If a hit occurs before exit, transition media at that face; otherwise cross to the neighbor tet and continue.
  - Guarantees correctness when portals are missing or ambiguous; segment tests are cheap because they’re short and coherent.
- Build-time guarantees:
  - Provide an air boundary layer so every surface face has an air tet within ε outward; refine locally if needed.
  - Maintain a global surface-face BVH (faces where medium changes) as the transition oracle.
- Edge handling:
  - Always nudge a small ε into the target medium to avoid re-hitting the same face; deterministically break ties for near-coplanar/edge cases.
  - If a portal’s tet fails the point-in-tet check at runtime, fall back to BVH-based point location in air and continue.

Caching and invalidation
- Per-face lighting reservoirs (DI/GI) keyed by faceId; invalidate by material/region; optionally diffuse seeds to neighbors.
- PixelHitCache remains per-pixel for primary visibility; instance/material edits flip hit/shade validity via per-tile bitmaps.

Robustness
- Watertightness via shared canonical planes for both incident tets.
- Tie-breaking on near-coplanar cases: deterministic next-face selection; small forward epsilon step when crossing faces.

=======
=======
  
>>>>>>> ee97327 (docs: update AI_ARCHITECTURE and add scene graph renderer plan)
>>>>>>> 4290844 (docs: update AI_ARCHITECTURE and add scene graph renderer plan)
## MVP plan
1) Scaffolding and helpers
   - Add `src/pathspace/ui/` with stubs for `PathRenderer2D`, `PathSurfaceSoftware`, and `PathWindowView` (presenter).
   - Add `ui/Builders.hpp` helpers (header-only) with comments explaining atomicity and path resolution behavior.

2) Software-only pipeline (macOS-friendly)
   - Implement `PathSurfaceSoftware` with pixel buffer + double-buffer.
   - Implement `PathRenderer2D` with target params, commit protocol, and simple rect/text rendering into the buffer.
   - Implement `PathWindowView` that blits buffers into a simple window (pair with existing Cocoa event pump).

3) Scene snapshots (minimal)
   - Define `scenes/<sid>/src/...`, `builds/<revision>/...`, `current_revision`.
   - Implement a minimal snapshot builder execution (stacking/absolute layout).
   - Integrate renderer to read snapshots by `current_revision`.

4) Notifications and scheduling
   - Debounced layout when `src` changes.
   - Optional renderer auto-schedule on notify; otherwise explicit trigger via surface/frame.

5) Tests and docs
   - Golden tests for snapshots and target param atomicity.
   - Concurrency tests (hammer edits while rendering).
   - Document this plan and update `docs/AI_ARCHITECTURE.md` if any core semantics change.

6) Metal backend (next)
   - `PathSurfaceMetal` producing an offscreen `MTLTexture`.
   - Presenter draws textured quad into `CAMetalLayer` drawable on the UI thread.

## Open questions

- Present policy: per-app defaults for staleness vs. always-fresh frames?
- Snapshot GC triggers and retention policy (K snapshots vs. revision-based).
- Text shaping and bidi strategy; font fallback.
- Color management (sRGB, HDR) across software/GPU paths.
- Direct-to-window bypass for single-view performance-critical cases (optional surface mode).
- HTML adapter fidelity targets (DOM/CSS vs Canvas JSON vs WebGL) and text shaping strategy for web output.

## Decision: Snapshot Builder (resolved)

Summary:
- Maintain the previous snapshot in memory and apply targeted patches for small changes; perform full rebuilds only on global invalidations or when fragmentation/cost crosses thresholds.

Approach:
- Patch-first: dirty propagation with per-node dirty bits (STRUCTURE, LAYOUT, TRANSFORM, VISUAL, TEXT, BATCH) and epoch counters to skip up-to-date nodes.
- Copy-on-write: assemble new revisions by reusing unchanged SoA arrays/chunks; only modified subtrees allocate new chunks.
- Text shaping cache: key by font+features+script+dir+text; re-shape only dirty runs.
- Chunked draw lists per subtree with k-way merge; re-bucket only affected chunks.

Full rebuild triggers:
- Global params changed (DPI/root constraints/camera/theme/color space/font tables).
- Structure churn: inserts+removes > 15% or reparent touch > 5% of nodes.
- Batching churn: moved draw ops > 30% or stacking contexts change broadly.
- Fragmentation: tombstones > 20% in nodes/draw chunks.
- Performance: 3 consecutive frames over budget, or patch_ms ≥ 0.7 × last_full_ms.
- Consistency violation detected by validations.

Publish/GC:
- Atomic write to `builds/<rev>.staging` then rename; update `current_revision` atomically.
- Retain last 3 revisions or 2 minutes; defer GC while a renderer references an older revision.

Notes:
- Renderer behavior is unchanged; it consumes the latest revision agnostic to patch vs rebuild.
- If any core semantics change, reflect them in `docs/AI_ARCHITECTURE.md` and update tests accordingly.

## Gaps and Decisions (unresolved areas)

Next to decide:
<<<<<<< HEAD
<<<<<<< HEAD

<<<<<<< HEAD
1) Lighting and shadows
=======

1) Snapshot builder spec
=======
1) Path/schema specs and typing
   - Finalize schemas for renderer target params (settings/inbox and settings/active), surface descriptions (pixel format, stride, premultiplied alpha, color space), and outputs (software framebuffer; GPU handle wrappers).
   - Introduce versioned keys (e.g., params/v1, outputs/v1).
   - Define app-relative resolution helper API and enforce same-app containment checks.

2) Scene authoring model
=======
1) Scene authoring model
>>>>>>> a841b6b (docs: clarify Doxygen HTML viewing and add Pages instructions; prune resolved items from SceneGraph plan 'Gaps and Decisions')
   - Node properties: transform representation (TRS vs matrix), style/visibility, interaction flags.
   - Hierarchy semantics: property inheritance, z-order, clipping behavior.
   - Initial layout systems: absolute/stack; plan flex/grid later. Measurement contracts for text and images.
   - Authoring API: thread model and batching for updates into scenes/<sid>/src.

<<<<<<< HEAD
3) Snapshot builder spec
>>>>>>> ee97327 (docs: update AI_ARCHITECTURE and add scene graph renderer plan)
=======
2) Snapshot builder spec
>>>>>>> a841b6b (docs: clarify Doxygen HTML viewing and add Pages instructions; prune resolved items from SceneGraph plan 'Gaps and Decisions')
   - Triggering/debounce policy and max rebuild frequency.
   - Work partitioning across passes (measure, layout, batching) and threading.
   - Transform propagation from hierarchy; text shaping pipeline and caching.
   - Snapshot/resource GC policy and sharing across revisions.

<<<<<<< HEAD
<<<<<<< HEAD
2) Rendering pipeline specifics
   - Software rasterization details (AA, clipping, blending, color pipeline) and text composition order.
   - GPU plans (command encoding patterns, pipeline caching) for Metal/Vulkan.

3) Lighting and shadows
>>>>>>> 4290844 (docs: update AI_ARCHITECTURE and add scene graph renderer plan)
   - Software UI lighting model (directional light, Lambert/Blinn-Phong) and elevation-based shadow heuristics.
   - Opt-in normals/3D attributes for “2.5D” widgets.

2) Coordinate systems and cameras
   - Units/DPI/scale conventions; orthographic defaults for UI; z-ordering semantics across 2D/3D.

3) Input, hit testing, and focus
   - Mapping OS input to scene coords, hit-testing via DrawableBucket bounds, event routing (capture/bubble), IME/text.

4) GPU backend architecture
   - Device/queue ownership, thread affinity, synchronization, offscreen texture/image formats and color spaces.

7) Resource system (images, fonts, shaders)
   - Async loading/decoding, caches, eviction, asset path conventions, font fallback/shaping library.

8) Error handling, observability, and profiling
   - Error propagation style, structured logging/tracing per target/scene/frame, metrics and debug overlays.

10) Documentation and diagrams
   - Add “UI/Rendering” to AI_ARCHITECTURE.md when APIs solidify; include Mermaid diagrams for data flow and schemas.

## Target keys (final)

=======
4) DrawableBucket details
   - Handle API (stable handles, generation counters), free lists.
   - Memory layout (SoA vs AoS), per-layer arrays, indices for fast material/layer iteration.
   - Thread safety: who updates staging, who publishes, child dirty propagation, and publish protocol.

Additional areas to flesh out:
5) Culling and spatial acceleration
   - Sphere vs AABB/OBB choices; optional quadtree/BVH later; rebuild vs incremental thresholds.

6) Rendering pipeline specifics
=======
3) Rendering pipeline specifics
>>>>>>> a841b6b (docs: clarify Doxygen HTML viewing and add Pages instructions; prune resolved items from SceneGraph plan 'Gaps and Decisions')
   - Software rasterization details (AA, clipping, blending, color pipeline) and text composition order.
   - GPU plans (command encoding patterns, pipeline caching) for Metal/Vulkan.

4) Lighting and shadows
   - Software UI lighting model (directional light, Lambert/Blinn-Phong) and elevation-based shadow heuristics.
   - Opt-in normals/3D attributes for “2.5D” widgets.

5) Coordinate systems and cameras
   - Units/DPI/scale conventions; orthographic defaults for UI; z-ordering semantics across 2D/3D.

6) Input, hit testing, and focus
   - Mapping OS input to scene coords, hit-testing via DrawableBucket bounds, event routing (capture/bubble), IME/text.

7) GPU backend architecture
   - Device/queue ownership, thread affinity, synchronization, offscreen texture/image formats and color spaces.

8) Resource system (images, fonts, shaders)
   - Async loading/decoding, caches, eviction, asset path conventions, font fallback/shaping library.

9) Error handling, observability, and profiling
   - Error propagation style, structured logging/tracing per target/scene/frame, metrics and debug overlays.

10) Documentation and diagrams
   - Add “UI/Rendering” to AI_ARCHITECTURE.md when APIs solidify; include Mermaid diagrams for data flow and schemas.

## Target keys (final)
 
>>>>>>> ee97327 (docs: update AI_ARCHITECTURE and add scene graph renderer plan)
- Target base:
  - `<app>/renderers/<rendererName>/targets/<kind>/<name>`
  - kind ∈ { `surfaces`, `textures`, `html` }
- Keys under a target:
  - `scene` — app-relative path to the scene root to render (must resolve within the same app root)
  - `desc` — descriptor for the target (SurfaceDesc/TextureDesc/HtmlTargetDesc)
  - `desc/active` — mirror written by renderer after reconfigure
  - `status/*` — e.g., `reconfiguring`, `device_lost`, `message`
  - `settings/inbox` — queue of whole `RenderSettings` objects (writers insert; renderer takes and adopts last)
  - `settings/active` — single-value mirror written by renderer after adoption (optional, for introspection)
  - `render` — execution to render one frame for this target
  - `output/v1/...` — latest outputs for this target:
    - `common/` — timings and metadata (`frameIndex`, `revision`, `renderMs`, `lastError`)
    - `software/framebuffer` — pixel buffer + metadata (width, height, stride, format, colorSpace, premultiplied)
    - `metal/texture` or `vulkan/image` — opaque GPU handles and metadata
    - `html/dom`, `html/commands`, `html/assets/*` — optional web outputs
<<<<<<< HEAD

## RenderSettings v1 (final)

=======
 
## RenderSettings v1 (final)
 
>>>>>>> ee97327 (docs: update AI_ARCHITECTURE and add scene graph renderer plan)
- time: `{ time_ms: double, delta_ms: double, frame_index: uint64 }`
- pacing: `{ user_cap_fps: optional<double> }`  # effective rate = min(display refresh, user cap)
- surface: `{ size_px:{w:int,h:int}, dpi_scale: float, visibility: bool }`
- clear_color: `[float,4]`
- camera: `{ projection: Orthographic | Perspective, zNear:float, zFar:float }` (optional)
- debug: `{ flags: uint32 }` (optional)
<<<<<<< HEAD

=======
 
>>>>>>> ee97327 (docs: update AI_ARCHITECTURE and add scene graph renderer plan)
Invariants:
- Writers always insert whole `RenderSettings` to `settings/inbox` (no partial field writes).
- Renderer drains `settings/inbox` via take(), adopts only the last (last-write-wins), and may mirror to `settings/active`.
- `scene` paths are app-relative and must resolve to within the same application root.
- `output/v1` contains only the latest render result for the target.
<<<<<<< HEAD

=======
 
>>>>>>> ee97327 (docs: update AI_ARCHITECTURE and add scene graph renderer plan)
## Glossary

- App root: `/system/applications/<app>` or `/users/<user>/system/applications/<app>`.
- App-relative path: a path string without leading slash, resolved against the app root.
- Renderer target: a per-consumer subtree under `renderers/<id>/targets/<kind>/<name>`.
- Snapshot: immutable render-ready representation of a scene at a point in time (`revision`).
- Revision: monotonically increasing version used for atomic publish/adoption.

## Cross-references

- See `docs/AI_ARCHITECTURE.md` for core PathSpace concepts (paths, NodeData, TaskPool, serialization). If any core behavior changes as part of implementing this plan (e.g., new blocking semantics, notification routing), update that document accordingly.
