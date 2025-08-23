# PathSpace — Scene Graph and Renderer Plan

Status: Draft (planning)
Scope: UI surfaces, renderers, presenters, multiple-scenes support, atomic updates
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
- Approach: add an HTML output adapter that walks the same sorted drawables or scene snapshot and emits:
  - DOM/CSS (quick path, not pixel-perfect), or
  - Canvas 2D JSON command stream + tiny JS runtime (closer to software renderer),
  - Optional WebGL later for performance and 3D effects.
- Paths (under a renderer target base):
  - `<targetBase>/output/v1/html/dom` — full HTML document as a string (may inline CSS/JS)
  - `<targetBase>/output/v1/html/css` — CSS string, if split
  - `<targetBase>/output/v1/html/commands` — JSON string of canvas commands
  - `<targetBase>/output/v1/html/assets/<name>` — optional assets (base64 or URLs)
- Mapping hints:
  - Rects/rounded-rects → div with border-radius or canvas roundRect
  - Images → img or canvas drawImage
  - Text → DOM text or canvas fillText (or pre-shaped glyph quads later)
  - Transforms → CSS matrix()/matrix3d() or canvas transform()
  - Shadows → CSS box-shadow or canvas shadow* properties
  - Z-order → z-index stacking contexts
  - Clipping → overflow:hidden (DOM) or canvas clip()
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
- Example (Canvas JSON commands):
```/dev/null/target_outputs_html_commands.json#L1-60
{
  "surface": { "size_px": {"w": 640, "h": 360}, "dpi_scale": 2.0 },
  "commands": [
    { "op": "clear", "color": [0.125, 0.125, 0.125, 1.0] },
    { "op": "save" },
    { "op": "transform", "m": [1,0,0,1,0,0] },
    { "op": "shadow", "color": [0,0,0,0.25], "blur": 20, "offsetX": 0, "offsetY": 8 },
    { "op": "fillRoundRect", "x": 40, "y": 30, "w": 200, "h": 120, "r": 12, "color": [0.29, 0.56, 0.89, 1] },
    { "op": "shadow", "color": [0,0,0,0], "blur": 0, "offsetX": 0, "offsetY": 0 },
    { "op": "fillText", "x": 56, "y": 70, "text": "Hello, PathSpace!", "font": "16px system-ui", "color": [1,1,1,1] },
    { "op": "restore" }
  ]
}
```
- PathKeys additions:
  - `target_outputs_html_dom(targetBase)`
  - `target_outputs_html_css(targetBase)`
  - `target_outputs_html_commands(targetBase)`
- Notes:
  - DOM/CSS is fastest to implement but not pixel-perfect; Canvas JSON offers better parity with the software renderer.
  - Text fidelity improves if we pre-shape glyphs in the snapshot and emit positioned quads.
  - This adapter is optional and does not affect software/GPU outputs.
  
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

## Gaps and Decisions (unresolved areas)

Next to decide:
1) Scene authoring model
   - Node properties: transform representation (TRS vs matrix), style/visibility, interaction flags.
   - Hierarchy semantics: property inheritance, z-order, clipping behavior.
   - Initial layout systems: absolute/stack; plan flex/grid later. Measurement contracts for text and images.
   - Authoring API: thread model and batching for updates into scenes/<sid>/src.

2) Snapshot builder spec
   - Triggering/debounce policy and max rebuild frequency.
   - Work partitioning across passes (measure, layout, batching) and threading.
   - Transform propagation from hierarchy; text shaping pipeline and caching.
   - Snapshot/resource GC policy and sharing across revisions.

3) Rendering pipeline specifics
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
 
## RenderSettings v1 (final)
 
- time: `{ time_ms: double, delta_ms: double, frame_index: uint64 }`
- pacing: `{ user_cap_fps: optional<double> }`  # effective rate = min(display refresh, user cap)
- surface: `{ size_px:{w:int,h:int}, dpi_scale: float, visibility: bool }`
- clear_color: `[float,4]`
- camera: `{ projection: Orthographic | Perspective, zNear:float, zFar:float }` (optional)
- debug: `{ flags: uint32 }` (optional)
 
Invariants:
- Writers always insert whole `RenderSettings` to `settings/inbox` (no partial field writes).
- Renderer drains `settings/inbox` via take(), adopts only the last (last-write-wins), and may mirror to `settings/active`.
- `scene` paths are app-relative and must resolve to within the same application root.
- `output/v1` contains only the latest render result for the target.
 
## Glossary

- App root: `/system/applications/<app>` or `/users/<user>/system/applications/<app>`.
- App-relative path: a path string without leading slash, resolved against the app root.
- Renderer target: a per-consumer subtree under `renderers/<id>/targets/<kind>/<name>`.
- Snapshot: immutable render-ready representation of a scene at a point in time (`revision`).
- Revision: monotonically increasing version used for atomic publish/adoption.

## Cross-references

- See `docs/AI_ARCHITECTURE.md` for core PathSpace concepts (paths, NodeData, TaskPool, serialization). If any core behavior changes as part of implementing this plan (e.g., new blocking semantics, notification routing), update that document accordingly.
