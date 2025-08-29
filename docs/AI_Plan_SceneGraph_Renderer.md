# PathSpace — Scene Graph and Renderer Plan

Status: Active plan (MVP in progress)
Scope: UI surfaces, renderers, presenters, multi-scene targets; atomic params and snapshot-based rendering
Audience: Engineers building UI/rendering layers and contributors adding platform backends

## Goals

- Application-scoped resources: windows, scenes, renderers, and surfaces live under one app root so removing the root tears everything down
- Multi-scene renderers: a single renderer can render multiple scenes concurrently; consumers select a scene via per-target configuration
- Window-agnostic surfaces: offscreen render targets (software or GPU) that can be presented by multiple windows within the same application
- Typed wiring: small C++ helpers return canonical paths; avoid string concatenation; validate same-app containment
- Atomicity and concurrency: prepare off-thread, publish atomically, and render from immutable snapshots for both target parameters and scene data
- Cross-platform path: start with software on macOS; add Metal; keep Vulkan as a future option

## Application roots and ownership

Applications are mounted under:
- System-owned: `/system/applications/<app>`
- User-owned: `/users/<user>/system/applications/<app>`

Everything an application needs is a subtree below the app root. No cross-app sharing of surfaces or renderers. All references are app-relative (no leading slash) and must resolve within the app root. See docs/AI_PATHS.md for the canonical path namespaces and layout conventions.

## App-internal layout (standardized)

- `scenes/<scene-id>/` — authoring tree (`src/...`), immutable builds (`builds/<revision>/...`), and `current_revision`
- `renderers/<renderer-id>/` — renderer with per-target subtrees (multi-scene capable)
- `surfaces/<surface-id>/` — offscreen render targets; coordinate with a renderer target
- `windows/<window-id>/` — platform window shell and views (presenters)

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
          scene                         # "scenes/main" (app-relative)
          desc                          # SurfaceDesc/TextureDesc/HtmlTargetDesc
          desc/active                   # mirror written by renderer (optional)
          settings/
            inbox                       # queue of whole RenderSettings (write-only by producers)
            active                      # mirror of adopted settings (optional)
          render                        # Execution: render one frame
          output/
            v1/
              software/framebuffer      # pixels + stride
              common/                   # timings, indices, etc.
                frameIndex
                revision
                renderMs
                lastError
  surfaces/
    editor/
      renderer = "renderers/2d"
      scene    = "scenes/main"
      render   # Execution: coordinates with renderer target
  windows/
    MainWindow/
      title = "Notepad — Main"
      window
      views/
        editor/
          surface = "surfaces/editor"
          present  # Execution: blit/draw surface into the window
```

## Entities and responsibilities

- Window (shell)
  - Platform-native window; emits state/events (resize, focus, close)
  - Lives under `windows/<id>/window` and is unaware of rendering
- Presenter/View (window view)
  - Lives under `windows/<id>/views/<view-id>/...`
  - Reads its `surface` (app-relative), optionally triggers a frame on the surface, and presents:
    - Software: blit bytes to the native window
    - Metal/Vulkan: draw a textured quad sampling the offscreen texture/image into the window’s drawable/swapchain
- Surface (offscreen render target)
  - Lives under `surfaces/<id>/...`
  - Holds `renderer` and `scene` (both app-relative strings); usable in any number of windows within the same app
  - `render` coordinates a target-scoped render with its renderer, then exposes output (framebuffer or GPU handles)
- Renderer (multi-scene)
  - Lives under `renderers/<id>/...`
  - Stateless w.r.t. windows; serves work per target:
    - `targets/<target-id>/scene` — app-relative scene path to render
    - `targets/<target-id>/settings/inbox` — queue of whole `RenderSettings` (atomic via insert/take)
    - `targets/<target-id>/settings/active` — optional mirror of adopted settings
    - `targets/<target-id>/render` — execution that renders one frame for this target
    - `targets/<target-id>/output/v1/...` — per-target outputs and stats (software/GPU/HTML)
  - Target-id convention: use the consumer’s app-local path, e.g., `surfaces/<surface-name>` or `textures/<texture-name>`

## Atomicity and concurrency

### Render settings atomicity (per renderer target)

- Settings update queue:
  - Writers insert whole `RenderSettings` values into `settings/inbox`
  - Renderer drains `settings/inbox` with `take()` and adopts only the last (last-write-wins)
  - Renderer may mirror the adopted struct to `settings/active` (single-value) for introspection
- Readers never see half-updated params; a single-path commit is used for producers

### Scene graph concurrency (authoring vs rendering)

- Authoring tree is mutable: `scenes/<sid>/src/...`
- Builds (snapshots) are immutable, versioned by revision:
  - `scenes/<sid>/builds/<revision>/...` — pre-baked display list (world transforms, z-order, draw commands)
  - `scenes/<sid>/current_revision` — pointer to the latest published build (single-value register)
- Build pipeline:
  1) Edits to `src` mark dirty and trigger a debounced layout/build
  2) A builder computes a display-list snapshot off-thread
  3) Build written under `builds/<new_revision>/...`
  4) Publish via atomic replace of `scenes/<sid>/current_revision` to `<new_revision>`
  5) Optionally GC old snapshots once not in use
- Renderers read a consistent build (`current_revision` latched per frame) with no global locks

### Locking strategy

- No global locks for scene edits; builders work from `src` to a new immutable snapshot
- Renderer param adoption uses a short local mutex; the render loop reads from adopted, immutable state
- Presenters marshal final present to the correct thread/runloop (e.g., macOS/Metal)

## Frame orchestration

Renderer (per target):
1) Adopt settings from `settings/inbox` (drain; last-write-wins; optionally mirror to `settings/active`)
2) Resolve `targets/<tid>/scene` against the app root; validate it stays within the same app subtree
3) Read `scenes/<sid>/current_revision`; latch for this render
4) Traverse `scenes/<sid>/builds/<revision>/...` and render:
   - Software: produce a framebuffer (pixels + stride)
   - GPU: render into an offscreen texture/image
5) Write `targets/<tid>/output/v1/...` and stamp `frameIndex` + `revision`

Presenter (per window view):
1) Read `views/<vid>/surface`; resolve to `surfaces/<sid>`
2) Optionally call `surfaces/<sid>/render`, which:
   - Writes `RenderSettings` to `renderers/<rid>/targets/surfaces/<sid>/settings/inbox`
   - Triggers `renderers/<rid>/targets/surfaces/<sid>/render`
3) Present:
   - Software: read framebuffer and blit to the window
   - GPU: draw a textured quad sampling the offscreen texture/image to the window drawable/swapchain

Staleness policy: presenters can present last-complete outputs if a fresh frame is in-flight (configurable freshness threshold)

Pacing:
- Default: match the display device’s refresh/vsync for the window/surface (variable-refresh compatible)
- HTML: `requestAnimationFrame`
- Headless/offscreen: on-demand; if continuous, timer-driven execution
- Optional user cap: effective rate = min(display refresh, user cap)

## Hierarchical coordinates and layout

- Authoring nodes in `scenes/<sid>/src/...` store local transforms, layout hints, and style
- Snapshot builder computes:
  - World transforms and bounds, z-order, batching, text glyph runs, image resolves, optional clip/stencil
- Snapshots store pre-baked draw commands for fast traversal, and the builder materializes the `DrawableBucket` staging arrays (flat, sorted/bucketed) corresponding to the published revision

### DrawableBucket (no widget-tree traversal at render time)

- Maintain a flat registry per scene for render-time iteration. In snapshot-driven mode, the builder populates arrays from `src` into `builds/<revision>`; publishing updates `current_revision`
- The renderer iterates contiguous arrays (or a few arrays by layer) for visibility/culling/sorting and issuing draw commands
- TLAS/BLAS integration (software path tracer): the active `DrawableBucket` also materializes acceleration views per snapshot revision
  - TLAS (instances): one record per drawable instance with world transform, `MaterialKey`, `pipelineFlags`, `layer`, and a reference to a deduplicated BLAS id derived from `drawRef`
  - BLAS (unique geometry): table of unique geometry payloads (rects, rounded-rects, images, glyph meshes, paths, meshes) with optional per-face BVH; deduped across instances within a revision
  - Global surface-face BVH (when applicable) plus optional tetrahedral face adjacency for objects that carry tet connectivity; used by the tet-walk traversal in the software renderer
  - Publish emits TLAS/BLAS alongside the draw lists; the renderer latches both with the same `revision` and can choose raster (draw lists) or path tracing (TLAS→BLAS) without re-traversing the widget tree

Conceptual API (builder/widget-facing; not used by the renderer during a frame):
- `register(widgetId) -> handle`
- `update(handle, {worldTransform, boundsLocal, material, layer, z, visibility, contentEpoch, transformEpoch, drawRef})`
- `deregister(handle)`
- `markDirty(handle, flags)`

Entry data (per drawable):
- Identifiers: `widgetId`, stable `handle`
- Transforms: local and world matrices; `transformEpoch`
- Bounds: local/world `BoundingSphere` and `BoundingBox` (AABB; optional OBB)
- Draw metadata: `layer`, `z`, pipeline flags (opaque/alpha), `materialId`
- Draw commands: cached list pointer/handle + `contentEpoch` (or a prepare callback)
- Visibility flag

Double-buffering:
- Keep `staging` and `active` arrays per scene. The builder/authoring side updates `staging`; publishing a new snapshot atomically swaps `staging` to `active`
- The renderer latches `current_revision` at the start of a frame and reads only from the matching `active` arrays (no renderer-owned swaps)

#### DrawableBucket — v1 contract

Handles and IDs
- DrawableId: 64-bit stable identifier scoped to a scene; reused only after a generation bump
- Generation: 32-bit counter incremented on reuse from a free-list; handle = (id, generation)
- Free-list and tombstones: removals push ids into a free-list with tombstone count; reuse occurs only when generation++ and all references to the old revision are gone
- Stability: within minor edits (no removal), drawables keep the same (id, generation); indices into SoA arrays may change between revisions, but ids do not
- Mapping: authoring node id → 1..N drawables (e.g., text can expand to multiple drawables); record this mapping in snapshot metadata for diagnostics

SoA layout (per snapshot revision)
- Arrays sized N drawables unless stated otherwise, tightly packed, immutable per revision:
  - world: Transform[N]
  - boundsWorld: Bounds[N] (contains sphereWorld and boxWorld; boxWorld optional/empty if unused)
  - layer: uint32[N]
  - z: float[N] (quantized for sort keys; keep float for accuracy)
  - materialId: uint32[N]
  - pipelineFlags: uint32[N] (blend/clip/scissor/etc.)
  - visible: uint8[N] (0/1)
  - cmdOffset: uint32[N], cmdCount: uint32[N] into the command buffer
- Command buffer:
  - cmdKinds: uint32[M] (enum per command)
  - cmdPayload: contiguous blob; per-kind fixed headers with offsets to variable-size payloads
- Optional index arrays (to avoid re-partitioning at render time):
  - opaqueIndices[]: uint32[K_opaque] (pre-filtered by visibility and pipelineFlags)
  - alphaIndices[]: uint32[K_alpha]
  - per-layer indices: indices/layer/<layer>.bin if layer-local traversal is desired
- Alignment/versioning:
  - All binary buffers are 64-bit aligned; headers include magic, version, endianness, counts, checksum

Sorting keys (render-time)
- Opaque: stable 64-bit key encouraging state locality then depth order
  - Key(materialId ↑, pipelineFlags ↑, layer ↑, zQuant ↑) where zQuant is an implementation-defined monotone quantization of z
- Alpha: stable 64-bit key for painter’s algorithm within layers
  - Key(layer ↑, zQuant ↓, materialId ↑, pipelineFlags ↑)
- Renderers may derive keys on the fly from SoA or use precomputed indices; key layout is not persisted across revisions (avoid baking it into the snapshot unless profiling says so)

Draw command encoding (per drawable range)
- Multi-command drawables use [cmdOffset, cmdCount] into the command buffer; no pointers in persisted files
- Command kinds (minimum):
  - Rect, RoundedRect, Image, TextGlyphs, Path, Mesh
- Payloads:
  - Rect/RoundedRect: geometry params, stroke/fill data
  - Image: atlas/texture id, UV rect, sampling flags
  - TextGlyphs: glyph-run id, per-glyph quad/atlas refs (pre-shaped); see text shaping cache notes in “Decision: Snapshot Builder (resolved)”
  - Path/Mesh: handle to geometry blob within the same command buffer
- Pipeline constraints:
  - pipelineFlags define ordering barriers (e.g., clip groups) that prevent batching across incompatible states
  - Backends map commands 1:1 to software raster ops or to GPU vertex/index streams without walking the widget tree

Bounds and culling
- Target-specific requirements:
  - 2D software/GPU: BoundingSphere in world space (required); r_world = r_local × maxScale(world). AABB is optional for tighter viewport clipping.
  - 3D ray tracing: BoundingSphere and per-drawable AABB are both required (TLAS/BLAS also carry AABBs for instances/geometry).
  - HTML: Neither sphere nor AABB is required by the renderer; authoring may omit bounds entirely.
- Per pass:
  - Opaque: frustum/viewport cull then sort by opaque key; depth early-out favored
  - Alpha: frustum/viewport cull then back-to-front within layer
- Invariants:
  - boundsWorld must enclose the drawable’s rendered pixels; violations are validation errors

Validations (loader/renderer)
- Array sizes equal N; indices within range; cmdOffset+cmdCount within M
- (id, generation) uniqueness within snapshot; materialId/pipelineFlags known to pipeline
- Optional: checksum of buffers; reject snapshot on mismatch and report to `output/v1/common/lastError`

#### Snapshot layout (builds/<revision>)

On-disk schema (per scene, per revision)
- Path: `scenes/<sid>/builds/<rev>/bucket/`
  - drawables.bin         — header (magic, version, counts, checksum) + SoA layout descriptor
  - transforms.bin        — world transforms (N)
  - bounds.bin            — Bounds[N] with sphereWorld always present; boxWorld optional
  - state.bin             — layer[N], z[N], materialId[N], pipelineFlags[N], visible[N]
  - cmd-buffer.bin        — cmdKinds[M] + payload blob
  - indices/opaque.bin    — optional opaqueIndices[]
  - indices/alpha.bin     — optional alphaIndices[]
  - indices/layer/*.bin   — optional per-layer indices
  - meta.json             — small JSON with revision, created_at, tool versions, and authoring→drawable id map summary
  - trace/tlas.bin        — optional (software path tracer; instances carry AABBs)
  - trace/blas.bin        — optional (software path tracer; geometry bounds/AABBs per BLAS)
- Binary headers:
  - All *.bin start with: magic(4), version(u32), endianness(u8), reserved, counts/offsets(u64), checksum(u64)

Publish/adopt/GC protocol
- Build writes to `scenes/<sid>/builds/<rev>.staging/*`, fsyncs, then atomically renames to `scenes/<sid>/builds/<rev>`
- Publish: atomically replace `scenes/<sid>/current_revision` with `<rev>`
- Renderer adopts by reading `current_revision` once per frame, mapping only `/<rev>/bucket/*` for that frame; the revision is pinned until frame end
- GC: retain last K revisions (default 3) or T minutes (default 2m), whichever greater; never delete a pinned revision
- Observability: renderer writes `frameIndex`, `revision`, `renderMs`, `lastError` under `targets/<tid>/output/v1/common/*` (see “Target keys (final)”)

Cross-references
- Changes to publish/adopt, snapshot schema, or DrawableBucket invariants must be reflected in `docs/AI_ARCHITECTURE.md` and associated tests
- For output paths and timings, see “Target keys (final)” and “RenderSettings v1 (final)”; keep examples and path references stable

### Transforms without per-frame traversal

- Hierarchy is for authoring/layout; propagate transforms on change, not per frame:
  - `world = parentWorld * local`
  - Update world bounds; bump `transformEpoch`
  - Push updated entry to `DrawableBucket` `staging`; enqueue children updates if needed
- Result: render-time is O(n_visible) with no parent walks

### Bounding volumes and culling

- Target-specific bounds policy:
  - 2D software/GPU: store world `BoundingSphere` (required); `BoundingBox` (AABB) optional for tighter viewport clipping.
  - 3D ray tracing: store both world `BoundingSphere` and per-drawable `BoundingBox` (AABB) (both required).
  - HTML: bounds not required by the renderer; may be omitted.
  - Sphere for broad-phase; `r_world = r_local * maxScale(world)`
- Per view/camera:
  - Frustum against spheres first; optional AABB vs viewport for 2D
  - Maintain buckets by layer/material to improve cache locality and reduce state changes

### Draw command generation and caching

- Widgets expose either:
  - A stable `DrawCommands` object + `contentEpoch`, or
  - A prepare function to (re)build commands into a command allocator off-thread when `contentEpoch` changes
- Renderer requests commands only when `contentEpoch` differs from last seen (retained rendering). Software UI may still redraw every frame; dirty-rects can follow later. Command preparation should be off-thread to avoid blocking the render loop

### Sorting and batching

- Partition visible drawables:
  - Opaque pass: sort by material/pipeline then by `z` (or depth); write-friendly for depth early-out
  - Alpha pass: back-to-front by `z` within layer
- Batch small UI ops (rects, rounded rects, images, glyph quads) into SoA buffers for software rasterization

## Rendering pipeline specifics (v1)

Overview
- The renderer consumes pre-baked commands and issues draw work with no widget-tree traversal. State changes are governed by `pipelineFlags` and `materialId`.
- Two backends are targeted in v1: a software rasterizer (2D UI focus) and a GPU path (Metal first; Vulkan later). Both follow the same ordering and blending rules.

Common state model and ordering
- Passes:
  - Opaque pass: drawables with no blending (alpha == 1 or fully opaque material). Sorted by material → pipeline → z.
  - Alpha pass: drawables requiring blending. Sorted back-to-front by z within layer.
- Clipping:
  - Clip rectangles: can be mapped to scissor on GPU and fast rect-mask in software.
  - Clip paths: require stencil/subpass on GPU; in software, draw into a coverage/stencil buffer.
  - Clip stack barriers are expressed in the command stream via `pipelineFlags` transitions; batching must not cross incompatible clip state.
- Color pipeline:
  - Render-time is linear. Textures flagged as sRGB are linearized on sample. Framebuffer conversion to target color space (e.g., sRGB) happens on write-out.
  - Premultiplied alpha is the default format for images and intermediate surfaces.

Software renderer (2D UI)
- Primitives:
  - Rect, RoundedRect: analytic edge coverage AA (1x sampling with coverage), optional multi-sample tiles for stress cases.
  - Path: even-odd or non-zero fill; analytic coverage preferred; stroke join/cap styles supported.
  - Image: bilinear sampling; optional mip selection; sRGB decoding on sample when flagged.
  - TextGlyphs: pre-shaped glyph quads; support grayscale and optional subpixel LCD rendering.
- Anti-aliasing:
  - Analytic coverage AA at edges; gamma-aware compositing. LCD subpixel AA available when `TextLCD` is set and background constraints allow.
- Blending:
  - Default Porter-Duff SrcOver with premultiplied alpha. Optional blend modes (Multiply, Screen) can be added via `BlendMode` bits.
- Clipping:
  - ClipRect: SIMD rect mask per tile.
  - ClipPath: software stencil/coverage buffer; commands between clip begin/end respect the active mask.
- Color management:
  - Linear working space. Inputs decode per asset flags; outputs encode to target color space (e.g., sRGB) with optional dithering for 8-bit.

GPU renderer (Metal/Vulkan)
- Targets:
  - Offscreen texture/image matching the target descriptor (size/format/color space). For sRGB targets, enable framebuffer sRGB encode or explicit conversion in shader.
- Pipelines:
  - Pipeline state keyed by {program, blend, clip mode, sample count, color space/sRGB encode, premultiplied, debug flags}.
  - For 2D UI, depth test off; depth write off; cull mode none. Optional depth for 3D overlays later.
- Vertex formats:
  - Rects/Images/TextGlyphs: instanced quads with per-instance attributes (transform/z/layer/material/UV).
  - Path: tessellated fills/strokes or signed-distance fields; choice is backend-specific and off the command encoding.
- Clipping:
  - ClipRect → scissor. ClipPath → stencil (separate pass to populate, then draw with ref test).
  - Clip stack changes introduce ordering barriers; do not batch across incompatible clip state.
- Batching:
  - Sort opaque by (materialId, pipelineFlags, zQuant), alpha back-to-front; submit in batches grouped by the active pipeline state.
- Synchronization:
  - Target-local synchronization only in the render loop. Presentation is marshaled by the presenter on the UI thread.

pipelineFlags mapping (v1)
- Flags classify drawables for pass selection and encode per-pass state. Multiple bits may be combined; batching must respect incompatible combinations.
- Classification:
  - Opaque vs Alpha is derived from flags: `AlphaBlend` implies alpha pass; absence implies opaque pass.
  - Clip presence selects stencil/scissor paths; `ClipPath` requires stencil-capable pipelines.
- Invariants:
  - When `AlphaBlend` is set without `UnpremultipliedSrc`, inputs are treated as premultiplied alpha.
  - `TextLCD` implies background constraints (no rotation/scaling that breaks subpixel layout); otherwise render as grayscale text.

Example (non-exhaustive) flags
```cpp
enum PipelineFlags : uint32_t {
  // Pass and blend
  Opaque               = 0x00000001, // hint; absence of AlphaBlend implies opaque
  AlphaBlend           = 0x00000002, // src-over premultiplied unless UnpremultipliedSrc is set
  UnpremultipliedSrc   = 0x00000004, // request conversion to premultiplied before blend

  // Clipping
  ClipRect             = 0x00000010,
  ClipPath             = 0x00000020,
  ScissorEnabled       = 0x00000040, // GPU-only optimization hint (maps from ClipRect when possible)

  // Text rendering
  TextLCD              = 0x00000100, // subpixel AA for text
  TextNoSubpixel       = 0x00000200, // force grayscale AA

  // Color pipeline
  SrgbFramebuffer      = 0x00001000, // target expects sRGB encode on write-out
  LinearFramebuffer    = 0x00002000, // target expects linear; convert assets accordingly

  // Debug/diagnostics
  DebugOverdraw        = 0x00010000,
  DebugWireframe       = 0x00020000,
};
```

Notes
- Materials own shader program and textures; `materialId` selects programs and resource bindings. `pipelineFlags` selects pipeline variants and state bits.
- If this section changes (blend behavior, flags, or color pipeline), update examples and tests, and reflect any core behavior impacts in `docs/AI_ARCHITECTURE.md`.

## Culling and spatial acceleration (v1)

Target policies
- 2D software/GPU:
  - Broad-phase: world-space sphere vs frustum/viewport (required).
  - Narrow-phase: optional world-space AABB vs viewport for tighter rejection.
- 3D ray tracing:
  - Required per-drawable BoundingSphere and AABB for culling and correctness.
  - TLAS/BLAS also carry AABBs (instances and geometry) and are used for traversal; per-drawable tests happen before instance-level traversal when beneficial.
- HTML:
  - Bounds not required by the renderer; culling disabled; rely on DOM layout rules in adapters.

Per-pass algorithm
- Opaque pass:
  - Sphere cull → optional AABB viewport clip (2D) → sort by opaque key (materialId, pipelineFlags, layer, zQuant) → draw.
- Alpha pass:
  - Sphere cull → optional AABB viewport clip (2D) → sort back-to-front within layer → draw.

Optional acceleration indices (deferred by default)
- Precomputed index buffers may be emitted by the builder per revision:
  - opaqueIndices[], alphaIndices[], indices/layer/<layer>.bin.
- Enable heuristics:
  - N ≥ 50k drawables and cull rejection ≥ 60%, or stable camera with repeated views where indices amortize sort cost.
- For 3D/path tracing, rely on TLAS/BLAS for traversal; no per-drawable spatial trees in MVP.

Metrics and observability
- Record per-frame:
  - totalDrawables, culledBySphere, culledByAABB, visibleAfterCull.
  - opaqueCount, alphaCount, batchCount, sortMs, cullMs.
- Surface target writes high-level timings to `output/v1/common/*`; renderer-specific counters may be exposed under a debug path.

Future work
- Add opt-in grids/BVH for 2D when N is large and camera stable.
- Consider OBBs or oriented bounds where rotation precision matters.

## Coordinate systems and cameras (v1)

Spaces
- Local space: per-node transform space used during authoring/layout.
- World space: accumulated parent→child transforms; snapshot builder writes world transforms for render.
- View space: world transformed by the active camera.
- Clip space: normalized device coordinates after projection.
- Screen space: pixel coordinates in the render target; y-down for UI.

Units and DPI
- RenderSettings.surface.size_px gives framebuffer size in physical pixels; dpi_scale indicates device pixel ratio.
- UI logical units are device-independent pixels (dp). World-space units for 2D UI equal logical px; physical px = logical px × dpi_scale.

Orthographic UI defaults (2D)
- Projection: Orthographic with y-down. Screen origin at top-left of the target; +X right, +Y down.
- Default world-to-screen mapping: x_world,y_world interpreted as logical pixels; z defaults to 0 unless specified.
- Z-ordering for 2D:
  - Primary ordering by `layer` (ascending).
  - Within a layer, z is used for sorting: opaque pass sorts z ascending; alpha pass sorts z descending (painter’s algorithm).
  - Ties are broken by stable `DrawableId` to ensure determinism.
- Depth buffer is disabled in the 2D UI pipeline; z participates only in sorting, not depth testing.

3D conventions (ray tracing and future 3D overlays)
- World: right-handed. +X right, +Y up, camera looks toward −Z by default.
- Camera:
  - Projection: `Orthographic` (2D UI) or `Perspective` (3D).
  - Parameters from RenderSettings.camera: zNear, zFar in view space; must satisfy zNear < zFar.
- Z semantics:
  - Opaque: sort by increasing camera-space depth (consistent with z ascending in our sort key).
  - Alpha: sort back-to-front by camera-space depth within layer.
  - When depth testing is enabled (future 3D overlays), sorting may be reduced to state-locality plus depth test.

Cameras in RenderSettings
- RenderSettings v1 carries an optional `camera`:
  - `Projection { Orthographic, Perspective }`
  - `zNear: float`, `zFar: float`
- UI default if camera is absent:
  - Orthographic y-down mapping sized to the surface in logical pixels.
  - zNear/zFar default to [-1, +1] in view space for sorting; depth test remains off.

Cross-references
- See “RenderSettings v1 (final)” for camera fields and invariants; “Rendering pipeline specifics (v1)” for pass sorting and blending rules.
- Changes that affect coordinate systems or camera semantics should be reflected in `docs/AI_ARCHITECTURE.md`.

## Input, hit testing, and focus (v1)

Coordinate mapping
- Event spaces:
  - OS/window coords → screen space (pixels in the render target; y-down for UI)
  - Screen → view space via inverse projection (orthographic for UI)
  - View → world via inverse camera/view transform
  - World → local (optional) via inverse node transform for precise tests
- DPI:
  - Convert device coordinates using `RenderSettings.surface.dpi_scale` so hit tests operate in logical pixels for 2D UI
- Camera:
  - If `RenderSettings.camera` is absent, use orthographic y-down mapping sized to the surface (see “Coordinate systems and cameras (v1)”)

Hit testing (DrawableBucket-driven)
- Snapshot consistency:
  - Latch `scenes/<sid>/current_revision` at event time; use that revision’s `DrawableBucket` for the entire hit test
- Ordering:
  - Evaluate candidates by descending priority: higher `layer` first; within a layer, sort by z descending (painter’s order). Use stable `DrawableId` to break ties deterministically
- Broad/narrow phases:
  - Broad: test point or pick ray against `BoundingSphere` (required) to prune
  - Narrow (2D UI):
    - Optional AABB vs viewport or point-in-rect test for tighter rejection
    - Exact shape test when needed:
      - Rect/RoundedRect: analytic edge test
      - Image: alpha-threshold test (premultiplied) with UV lookup; respect clip
      - TextGlyphs: quad bounds; optional glyph coverage test per glyph
      - Path: winding/even-odd test against path geometry
    - Respect active clip stack (rect or path) when present; clipped drawables do not receive hits outside the clip
  - 3D ray tracing:
    - Build a pick ray in view space; test per-drawable AABB (required for 3D) before invoking TLAS/BLAS traversal for precise intersections when applicable
- Results:
  - Return the topmost hit target plus an ordered list of ancestors for routing; include local/world coords, uv (if image/text), and modifiers

Event routing
- Phases:
  - Capture: root → target ancestor chain
  - Target: target receives the event
  - Bubble: target ancestor chain → root
- Controls:
  - stopPropagation: halts further capture/bubble
  - preventDefault: signals default behavior should not run (app-defined)
- Common events:
  - Pointer: down/up/move/enter/leave/cancel, wheel, button, modifiers
  - Keyboard: keydown/keyup/text input, repeat flags, modifiers
  - Touch: start/move/end/cancel with contact ids

Focus and IME
- Focus model:
  - One focused element per scene; store focused `DrawableId` (or authoring node id) and revision
  - Focus traversal: next/prev order is app-defined; default traversal by DOM-like preorder if unspecified
- Text input/IME:
  - Composition events are delivered to the focused text-editable element
  - Caret and selection rectangles map from local to screen for IME candidate windows; update on transform/content changes
- Accessibility hooks (forward-looking):
  - Expose semantic roles and focus order; coordinate with platform accessibility APIs (not in MVP)

Threading and staleness
- Event delivery targets the authoring tree; the snapshot builder publishes revisions asynchronously
- Hit testing uses the latest available revision; if a new revision lands mid-routing, continue routing using the latched revision to stay consistent for that event
- Presenters may show last-complete outputs while handling input; see staleness policy in Frame orchestration

HTML adapter differences
- HTML targets delegate hit testing and focus to the browser’s DOM
- The adapter maps DOM events to scene events where applicable; geometry-level tests are not run in the renderer for HTML targets

## GPU backend architecture (v1)

Scope
- Applies to GPU targets (Metal first; Vulkan later). Focuses on device/queue ownership, thread affinity, synchronization, resources/reconfigure, command encoding, pipeline caches, color formats, device loss, and observability.

Device and queue ownership
- Each renderer instance owns its GPU device/context and one or more queues:
  - Metal: MTLDevice + MTLCommandQueue (one graphics queue per renderer; optional transfer-only queue in future)
  - Vulkan: VkDevice + graphics queue (and present queue when applicable); command pools are per-thread
- Targets render offscreen; presenters handle display integration on the UI thread (e.g., CAMetalLayer)

Thread affinity and submission
- Command encoding happens on a renderer worker thread per target (no UI thread dependency)
- Metal objects with thread affinity (e.g., CAMetalDrawable) are handled by the presenter; renderers consume offscreen textures/images
- Vulkan command pools are thread-bound; allocate and reset per-thread pools

Synchronization model
- Per-target synchronization only; no global renderer lock
- CPU side:
  - Short mutex to adopt `settings/active`; snapshot read is lock-free after latching `current_revision`
- GPU side:
  - One command buffer per frame per target (or a small ring); fences/semaphores wait for completion before resource reuse
  - Avoid cross-target synchronization; each target’s timeline is independent
- Present:
  - Software path: blit framebuffer to window on UI thread
  - Metal: presenter draws a textured quad sampling the offscreen texture into CAMetalDrawable
  - Vulkan: similar present path via swapchain in the presenter (future)

Resources and reconfigure
- Target descriptor changes (`desc`) trigger reconfigure:
  - Recreate offscreen textures/images with new size/format/color space
  - Rebuild or rebind framebuffers/render passes as needed
- Resources:
  - Immutable assets (images, glyph atlases) are cached per renderer; lifetime spans multiple frames
  - Per-frame: transient buffers for instance data and uniform blocks; ring-allocated to avoid stalls

Command encoding (per backend)
- Metal:
  - One MTLCommandBuffer per frame; one MTLRenderCommandEncoder for opaque, one for alpha (or separate passes when stencil is needed)
  - ClipRect → scissor; ClipPath → stencil prepass and test; bind materials (pipeline state + textures + constants)
- Vulkan:
  - One primary command buffer per frame; subpasses when stencil is required
  - Dynamic states for scissor/viewport; descriptor sets for materials; pipeline barriers confined within the frame’s scope

Pipeline caches
- Pipeline state keyed by {program/material, blend mode, clip mode, sample count, color space encode, premultiplied, debug flags}
- Cache lookup per batch; LRU eviction for rarely used variants
- Warmup common pipelines at startup to reduce first-frame stutter

Color formats and spaces
- Framebuffer formats:
  - sRGB: prefer SRGB8A8 for UI when available; enable automatic sRGB encode on write-out
  - Linear: RGBA8 for linear targets; explicit encode when presenting to sRGB displays
- Textures:
  - Respect asset color space flags; sRGB textures sampled with automatic decode; linear textures sampled as-is
- Premultiplied alpha as default for images/intermediates; `UnpremultipliedSrc` flag triggers conversion on sample/blend

Device loss and recovery
- Detect device loss or drawable acquisition failures; set `status/device_lost` and write `lastError`
- Attempt reinitialize-on-demand for subsequent frames; if recovery fails, keep reporting error and skip rendering work for the target

Observability
- Per-frame metrics written to `output/v1/common/*`: `frameIndex`, `revision`, `renderMs`, `lastError`
- Optional GPU counters (backend-specific) may be exposed under a debug subtree for diagnostics; avoid mandatory dependencies on profiling APIs

### Minimal types (sketch)

```cpp
struct Transform { float m[16]; }; // 3D; 2D via orthographic with z=0

struct BoundingSphere { float cx, cy, cz, r; };
struct BoundingBox { float min[3], max[3]; };


struct Bounds {
  // Runtime: sphereWorld is required; boxWorld is optional (hasBoxWorld=false when omitted).
  // Authoring-only locals (sphereLocal/boxLocal) are not persisted in runtime snapshots.
  BoundingSphere sphereWorld;
  bool           hasBoxWorld;
  BoundingBox    boxWorld; // only valid if hasBoxWorld
};

struct DrawCommand {
  uint32_t type;                  // Rect, RoundedRect, Image, TextGlyphs, Mesh, Path, ...
  uint32_t materialId;
  uint32_t pipelineFlags;         // opaque/alpha, blend, etc.
  uint32_t vertexOffset, vertexCount; // or payload handle
  // software-specific payload as needed
};

struct DrawableEntry {
  uint64_t          id;
  Transform         world;
  Bounds            bounds;
  uint32_t          layer;
  float             z;
  uint32_t          pipelineFlags;
  uint32_t          materialId;
  uint64_t          transformEpoch;
  uint64_t          contentEpoch;
  const DrawCommand* cmds;
  uint32_t          cmdCount;
  bool              visible;
};
```

## Scene authoring model (C++ API)

- Authoring is via typed C++ helpers; no JSON authoring
- Scene content lives under `<app>/scenes/<sid>/src/...`; a commit barrier can signal atomic batches
- Minimal node kinds: `Container`, `Rect`, `Text`, `Image`
- Transforms: 2D TRS per node (position, rotationDeg, scale), relative to parent
- Layout: `Absolute` and `Stack` (vertical/horizontal) in v1
- Style: opacity (inherits multiplicatively), fill/stroke, strokeWidth, cornerRadius, `clip` flag (on containers)
- Z-order: by `(zIndex asc, then children[] order)` within a parent

Preferred authoring pattern: nested PathSpace mount
- Build the scene in a temporary, unmounted `PathSpace`:
  - Insert nodes under its `/src/...` subtree (typed inserts)
  - Set `/src/nodes/root` and `/src/root` when ready
- Atomically publish by mounting:
  - Insert the local `PathSpace` into the main `PathSpace` at `<app>/scenes/<sid>` using `std::unique_ptr<PathSpace>`
  - A single notify wakes waiters (e.g., the snapshot builder)

Example: atomic publish via nested PathSpace
```cpp
SP::PathSpace ps;

// Build scene in a local (unmounted) PathSpace
auto local = std::make_unique<SP::PathSpace>();

// Author nodes under the local space's /src subtree (typed inserts)
local->insert("/src/nodes/card", RectNode{
  .style  = Style{ .fill = Color{0.29f,0.56f,0.89f,1}, .cornerRadius = 12 },
  .layout = Absolute{ .x = 40, .y = 30, .w = 200, .h = 120 }
});

local->insert("/src/nodes/title", TextNode{
  .text   = TextProps{ .content = "Hello, PathSpace!", .font = "16px system-ui" },
  .layout = Absolute{ .x = 56, .y = 70 } // auto size for text
});

local->insert("/src/nodes/root", ContainerNode{
  .layout   = Absolute{ .x = 0, .y = 0, .w = 800, .h = 600 },
  .children = std::vector<NodeId>{ id("card"), id("title") }
});

// Set the root reference within the local space
local->insert("/src/root", std::string{"root"});

// Atomically publish the completed scene by mounting it
ps.insert("apps/demo/scenes/home", std::move(local));
```

### Stack layout (v1)

Fields:
- `axis`: Vertical (top→bottom) or Horizontal (left→right)
- `spacing`: gap between adjacent children (main axis)
- `alignMain`: `Start | Center | End` — pack sequence if there’s slack
- `alignCross`: `Start | Center | End | Stretch` — cross-axis placement/sizing
- Per-child `weight` (>= 0): shares leftover main-axis space; `0` = fixed
- Per-child min/max hints on both axes (optional)

Main-axis sizing:
1) Measure fixed items (`weight==0`): explicit size else intrinsic; clamp to min/max
2) `available = containerMain - (sumFixed + spacing*(N-1))`
3) Distribute to weighted items: `available * (weight_i / totalWeight)`
4) Clamp weighted items to min/max iteratively; redistribute remaining among unclamped until stable
5) Overflow: if content exceeds container, no auto-shrink in v1; items overflow sequentially. If `container.clip`, overflow is clipped

Cross-axis sizing:
- If explicit cross size: use it
- Else if `alignCross=Stretch`: child cross size = container cross size (then clamp)
- Else: intrinsic cross size; position by `alignCross`

Positioning:
- Start `offset = 0`; adjust by `alignMain` when `totalChildrenSize < containerMain`
- Place child at `(offset, crossOffset)`, then `offset += childMain + spacing`
- Child transform applies post-layout (visual only; does not affect layout in v1)

Z-order and hit testing:
- Order within a stack remains `(zIndex asc, then children[] order)`
- If `container.clip==true`, descendants are clipped for draw and hit testing

Text and images:
- Text wraps when width is implicit via `Stretch` on cross-axis (vertical stack) or explicit width; height grows
- Otherwise, single-line intrinsic size; may overflow unless clipped
- Image natural size if no explicit size; otherwise apply fit (`contain | cover | fill | none`)

### Authoring publish strategies

- Atomic mount (preferred):
  - Build in a temporary `PathSpace` and insert it at `<app>/scenes/<sid>` (`unique_ptr<PathSpace>`)
  - Single notify wakes waiters (e.g., the snapshot builder)
  - Avoids partial reads without extra synchronization
- Commit barrier (incremental authoring):
  - For streaming/incremental edits to `<app>/scenes/<sid>/src`, writers may update nodes and then bump `<app>/scenes/<sid>/src/commit` as a publish hint
  - Builders can listen to `src/commit` to rebuild immediately; non-commit edits may be debounced (e.g., ~16 ms)

#### Builder: waiting for mount or revision

Atomic mount detection (poll until `/src/root` exists):
```cpp
using namespace std::chrono_literals;

Expected<void> wait_for_scene_ready(SP::PathSpace& ps, std::string const& app, std::string const& sid) {
  const std::string rootPath = app + "/scenes/" + sid + "/src/root";
  for (;;) {
    auto root = ps.read<std::string>(rootPath, Block{50ms});
    if (root) return {};
  }
}
```

Incremental edits with commit barrier:
```cpp
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
  }
}
```

Waiting for a new published revision:
```cpp
using namespace std::chrono_literals;

uint64_t wait_for_new_revision(SP::PathSpace& ps, std::string const& app, std::string const& sid, uint64_t known) {
  const std::string revPath = app + "/scenes/" + sid + "/current_revision";
  for (;;) {
    auto rev = ps.read<uint64_t>(revPath, Block{250ms});
    if (rev && *rev != known) return *rev;
  }
}
```

### Renderer loop outline

```cpp
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

- Edits set a scene dirty flag/counter and notify a layout worker (debounced)
- Renderers may watch target scene subtrees to mark targets dirty
- Modes:
  - Explicit: surfaces/presenters trigger frames
  - On-notify: renderer schedules frames for dirty targets

## Safety and validation

- App-relative resolution: a path without a leading slash is resolved against the app root
- Same-app validation: after resolution, verify the target path still lies within the app root; reject otherwise
- Platform handle hygiene:
  - Use opaque typed wrappers for `CAMetalLayer*`, `VkImage`, etc.
  - Document ownership and thread affinity (present must occur on the correct thread)

## Performance notes

- Software path introduces an extra copy (renderer → presenter blit)
  - Mitigate with double-buffering and mapped memory; reuse buffers on resize where possible
- GPU path uses an offscreen pass plus a present pass
  - Optionally allow a presentable-surface mode (direct-to-window) when multi-window reuse is not needed
- Debounce layout and frame triggers to avoid overdraw on bursty updates
- Constrain retained snapshots; share resources across snapshots where safe

## Builder/helpers (typed wiring)

Introduce small C++ helpers to avoid brittle path-string glue.

Responsibilities:
- Resolve app-relative paths and validate containment
- Manage target-id convention (`targets/surfaces/<name>`)
- Use PathSpace atomic primitives:
  - Single-value replace for small configs (e.g., `desc`)
  - Params update queue for per-target renderer params (drained via `take()`; last-write-wins)
  - Snapshot revision flip (single write) for scene publish (builder concern)
- Provide readable errors with context (target-id, frame index, snapshot revision)

Helper API (schema-as-code; returns canonical absolute paths; excerpt):
```cpp
namespace SP::ui {

using AppRoot = std::string;

struct SceneParams    { std::string name; std::string description; };
enum class RendererKind { Software2D, Metal2D, Vulkan2D };
struct RendererParams { std::string name; RendererKind kind; std::string description; };
struct SurfaceDesc    { /* backend, size, format, colorSpace, etc. */ };
struct SurfaceParams  { std::string name; SurfaceDesc desc; std::string renderer; };
struct WindowParams   { std::string name, title; int width=0, height=0; float scale=1.0f; std::string background; };

Expected<std::string> resolve_app_relative(AppRoot const& appRoot, std::string_view maybeRel);
std::string derive_target_base(AppRoot const& appRoot, std::string const& rendererPathAbs, std::string const& targetPathAbs);

Expected<std::string> create_scene   (PathSpace&, AppRoot const&, SceneParams const&);
Expected<std::string> create_renderer(PathSpace&, AppRoot const&, RendererParams const&);
Expected<std::string> create_surface (PathSpace&, AppRoot const&, SurfaceParams const&);
Expected<std::string> create_window  (PathSpace&, AppRoot const&, WindowParams const&);

Expected<void> attach_surface_to_view(PathSpace&, AppRoot const&,
                                      std::string windowPathOrName, std::string_view viewName,
                                      std::string surfacePathOrName);

Expected<void> set_surface_scene(PathSpace&, AppRoot const&,
                                 std::string surfacePathOrName, std::string scenePathOrName);

struct RenderSettings { /* see RenderSettings v1 (final) */ };
enum class ParamUpdateMode { Queue, ReplaceActive };

Expected<void> update_target_settings(PathSpace&, AppRoot const&,
                                      std::string targetPathOrSpec,
                                      RenderSettings const&,
                                      ParamUpdateMode mode = ParamUpdateMode::Queue);

struct FutureAny {}; // opaque future type for completion tracking

Expected<FutureAny> render_target_once(PathSpace&, AppRoot const&,
                                       std::string targetPathOrSpec,
                                       std::optional<RenderSettings> overrides = std::nullopt);

Expected<void> present_view(PathSpace&, AppRoot const&,
                            std::string windowPathOrName, std::string_view viewName);

struct RendererCaps {};
Expected<RendererCaps> get_renderer_caps(PathSpace const&, AppRoot const&, std::string rendererPathOrName);
Expected<SurfaceDesc>  get_surface_desc (PathSpace const&, AppRoot const&, std::string surfacePathOrName);

} // namespace SP::ui
```

Source layout (proposed):
- `include/pathspace/ui/Builders.hpp`
- `src/pathspace/ui/`
  - `scene/SceneSnapshotBuilder.{hpp,cpp}`
  - `renderer/PathRenderer2D.{hpp,cpp}`
  - `renderer/DrawableBucket.{hpp,cpp}`
  - `surface/PathSurface.hpp`
  - `surface/PathSurfaceSoftware.{hpp,cpp}`
  - `surface/PathSurfaceMetal.{hpp,mm}` (ObjC++ on Apple)
  - `surface/SurfaceTypes.hpp`
  - `window/PathWindow.{hpp,mm}`
  - `window/PathWindowView.{hpp,cpp|mm}`
  - `platform/macos/...`, `platform/win32/...`, `platform/x11|wayland/...`

CMake options (single library, feature-gated):
```cmake
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

```cpp
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

  // 3) Create a surface linked to the renderer
  SurfaceDesc sdesc; sdesc.size = {1280, 720, 2.0f};
  SurfaceParams sparams{ .name = "editor", .desc = sdesc, .renderer = "2d" };
  auto surfacePath = create_surface(space, app, sparams).value();

  // 4) Create a window
  WindowParams wparams{ .name = "MainWindow", .title = "Notepad — Main", .width = 1280, .height = 720, .scale = 2.0f };
  auto windowPath = create_window(space, app, wparams).value();

  // 5) Wire: attach surface to window view and pick a scene for the surface
  attach_surface_to_view(space, app, "MainWindow", "editor", "editor").value();
  set_surface_scene(space, app, surfacePath, "scenes/main").value();

  // 6) Update settings (queued) and render once
  RenderSettings rs; rs.surface.size_px = {1280, 720}; rs.surface.dpi_scale = 2.0f;
  update_target_settings(space, app, "renderers/2d/targets/surfaces/editor", rs).value();
  auto fut = render_target_once(space, app, "renderers/2d/targets/surfaces/editor").value();

  // 7) Present the view
  present_view(space, app, windowPath, "editor").value();
}
```

## HTML/Web output (optional adapter)

Motivation: preview/export UI scenes to browsers without changing the core pipeline

Approach: semantic HTML/DOM adapter mapping widgets to native elements with CSS-based layout/animation. No ray tracing, no canvas command stream, no WebGL.

Paths (under a renderer target base):
- `output/v1/html/dom` — full HTML document as a string (may inline CSS/JS)
- `output/v1/html/css` — CSS string, if split
- `output/v1/html/assets/<name>` — optional assets (base64 or URLs)

Mapping hints:
- Rects/rounded-rects → `div` with `border-radius`
- Images → `img` or CSS `background-image` on `div`
- Text → DOM text (or pre-shaped glyph spans later)
- Transforms → CSS `matrix()/matrix3d()`
- Shadows → CSS `box-shadow`
- Z-order → `z-index` stacking contexts
- Clipping → `overflow:hidden` or CSS `clip-path`

Example (DOM/CSS):
```html
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

Notes:
- Semantic DOM/CSS only; no ray tracing or canvas/WebGL in this adapter
- Text fidelity improves if we pre-shape glyphs in the snapshot and emit positioned spans
- Optional adapter; does not affect software/GPU outputs

## Software renderer — path tracing with tetrahedral acceleration

Status: resolved plan for software renderer pipeline

Summary:
- Scenes are often static; we prioritize incremental refinement via path tracing with snapshot-integrated acceleration structures
- Acceleration is snapshot-integrated:
  - TLAS per revision over instances; BLAS per unique geometry (deduped)
  - Tetrahedral face adjacency enables “tet-walk” traversal between neighboring tets for multi-bounce paths

Caching for fast edits:
- Per-pixel `PixelHitCache` stores primary hit data and accumulation state; supports targeted invalidation
- Per-face reservoirs (ReSTIR-style) store reusable direct- and indirect-light samples to guide secondary rays

Geometry and traversal:
- Mesh: tetrahedra with precomputed face table and adjacency (two incident tets per face; boundary sentinel)
- Surface classification: a face is a “surface” when adjacent media differ (or one side is boundary); shading happens on surfaces
- Primary rays: TLAS → BLAS (face BVH) intersection
- Secondary rays: start from a hit face and tet-walk across faces inside the same medium; intersect surface faces; fall back to BVH if needed

Per-pixel cache (primary visibility and accumulation):
- Store per pixel: `instanceIndex`, `primIndex`, `t`, `barycentrics`, `uv`, packed normal, `materialId`, flags `hitValid/shadeValid`
- Invalidate selectively:
  - Material shading-only change → `shadeValid = 0` (no retrace)
  - Material visibility/alpha/displacement → `hitValid = 0` (retrace)
  - Instance transform/visibility → `hitValid = 0` for that instance’s pixels
  - Camera/viewport change → full reset (or reprojection heuristics)
- Use per-tile (e.g., 16×16) inverted indices mapping `materialId/instanceIndex` → bitsets of affected pixels for O(tiles) invalidation

Per-face reservoirs and guiding (reuse across pixels/frames):
- Direct lighting: small ReSTIR DI reservoir per face; sample from it and update with new candidates each iteration
- Indirect lighting (optional): GI reservoirs per face capturing incoming direction/radiance; guide BSDF sampling
- Spatial reuse: reseed from adjacent faces via tet adjacency for sparse-history faces
- Guiding: optionally fit a lightweight directional PDF (e.g., 1–2 vMF lobes) from reservoir history; mix with BSDF and apply MIS

Iteration pipeline (per frame/refinement step):
1) Acquire snapshot revision and settings; map TLAS/BLAS, face adjacency; allocate per-frame state
2) For each visible pixel:
   - If `hitValid`, reuse primary hit; else regenerate and update `PixelHitCache`
   - Direct light via DI reservoir + BSDF importance sampling (MIS)
   - Spawn secondary ray(s), guided by face GI/DI where available; walk via tet-walk until a surface is hit; accumulate contribution
   - Update per-face reservoirs at landing faces with MIS-consistent weights
3) Write accumulated color; update per-tile indices for invalidation

Settings (software renderer):
- `rt.enable`, `rt.raysPerPixel`, `rt.maxDepth`, `rt.branching (4|8)`, `rt.leafMaxPrims`, `rt.quantization (none|aabb16)`, `rt.tileSize (8|16|32)`
- `reuse`: `di.enable`, `gi.enable`, `reuseStrength`
- `caches`: `pixelCache.enable`, `faceReservoirs.enable`, `faceReservoirs.capacity`
- `debug`: `showTetWalk`, `showSurfaceFaces`, `showInvalidation`, `showReservoirs`

Stats (`output/v1/common` and `output/v1/rt_stats`):
- `cpu_build_ms`, `traversal_ms`, `shading_ms`
- `tlas_nodes`, `blas_nodes_total`, `rays_traced`, `hits`, `miss_rate`
- `retraced_pixels`, `reshaded_pixels`, `reservoir_updates`

Failure modes and fallbacks:
- If TLAS/BLAS not ready: render with cached primary where valid; schedule (re)build within budget; optionally hybrid raster primary as fallback
- If adjacency is non-manifold/degenerate in a region: fall back to BVH traversal there; log once

Cross-reference:
- When snapshot semantics or acceleration formats evolve, reflect changes in `docs/AI_ARCHITECTURE.md` (Snapshot Builder and Rebuild Policy)

## Tetrahedral face adjacency

Purpose:
- Provide a watertight, deterministic connectivity index over tetrahedral meshes to enable fast multi-bounce path tracing (tet-walk) and robust physics sweeps without re-entering a BVH at every step

Data model:
- Face table (one record per unique triangle):
  - Canonical vertex triplet (deterministic winding)
  - Incident tetrahedra: up to two per face; boundary has one
  - Per-incidence metadata: local face index in the tet, winding parity vs canonical
  - Precomputed plane (`n.xyz, d`), optional tangent frame and area
  - Optional per-side medium/material id to classify “surface” faces (medium change)
- Tet table (one record per tetrahedron):
  - Vertex indices `[a,b,c,d]`
  - Global `faceId[4]` mapping local faces to face table
  - `neighborTet[4]` and `neighborLocalFace[4]` across local faces (or `-1` for boundary)

Canonical local faces (for tet `[a,b,c,d]`):
- `f0` (opposite `a`): `(b,c,d)`
- `f1` (opposite `b`): `(a,d,c)`
- `f2` (opposite `c`): `(a,b,d)`
- `f3` (opposite `d`): `(a,c,b)`

Build algorithm (deterministic):
- For each tet local face, compute a sorted triplet key (ascending vertex ids) to deduplicate faces
- First incidence sets the face’s canonical winding (use the tet’s local winding)
- Second incidence fills neighbor links and winding parity
- Compute planes once per face from canonical winding; assign medium ids per side if available
- Validate: flag non-manifold faces (>2 incidences) and zero-area faces for fallback

Traversal (renderer/physics):
- Secondary rays: start at a surface face; choose entering tet by sign of `d·n`; repeatedly intersect current tet’s face planes and cross to the next tet until reaching a surface/boundary face; only intersect triangles at surface faces. Fall back to BVH in degenerate regions
- Physics shape-casts/CCD: same face-to-face stepping with conservative inflation; use the static TLAS for broad-phase, adjacency for narrow-phase marching

Medium transitions (disjoint air/object):
- Keep separate adjacency for object and air; no need to stitch meshes
- Surface portals (precomputed):
  - For each surface face, store `1..K` candidate entry air tet ids on the outward side; build by offsetting a point on the face along outward normal by `ε` and locating the containing air tet via an air AABB BVH + point-in-tet test. Sample a few intra-face points for robustness
  - At runtime, after shading with an outgoing direction into air, jump to the portal’s air tet and begin the air tet-walk from there
- Clip-and-test fallback:
  - While walking in air (or object), compute exit distance to the next tet face; clip `[t_cur, t_exit]` and test that short segment against the global surface-face BVH. If a hit occurs before exit, transition media at that face; otherwise cross to the neighbor tet and continue
  - Guarantees correctness when portals are missing or ambiguous; short coherent segments make tests cheap
- Build-time guarantees:
  - Provide an air boundary layer so every surface face has an air tet within `ε` outward; refine locally if needed
  - Maintain a global surface-face BVH (where medium changes) as the transition oracle
- Edge handling:
  - Nudge `ε` into the target medium to avoid re-hitting the same face; deterministically break ties for near-coplanar/edge cases
  - If a portal’s tet fails the point-in-tet check at runtime, fall back to BVH-based point location in air and continue

Caching and invalidation:
- Per-face lighting reservoirs (DI/GI) keyed by `faceId`; invalidate by material/region; optionally diffuse seeds to neighbors
- `PixelHitCache` remains per-pixel for primary visibility; instance/material edits flip hit/shade validity via per-tile bitmaps

Robustness:
- Watertightness via shared canonical planes for both incident tets
- Deterministic next-face selection; small forward epsilon step when crossing faces

## MVP plan

1) Scaffolding and helpers
   - Add `src/pathspace/ui/` with stubs for `PathRenderer2D`, `PathSurfaceSoftware`, and `PathWindowView` (presenter)
   - Add `ui/Builders.hpp` (header-only) with comments explaining atomicity and path resolution behavior
2) Software-only pipeline (macOS-friendly)
   - Implement `PathSurfaceSoftware` with pixel buffer + double-buffer
   - Implement `PathRenderer2D` with target params, commit protocol, and simple rect/text rendering
   - Implement `PathWindowView` that blits buffers into a simple window (pair with Cocoa event pump)
3) Scene snapshots (minimal)
   - Define `scenes/<sid>/src/...`, `builds/<revision>/...`, `current_revision`
   - Implement a minimal snapshot builder (stacking/absolute layout)
   - Integrate renderer to read snapshots by `current_revision`
4) Notifications and scheduling
   - Debounced layout when `src` changes
   - Optional renderer auto-schedule on notify; otherwise explicit trigger via surface/frame
5) Tests and docs
   - Golden tests for snapshots and target param atomicity
   - Concurrency tests (hammer edits while rendering)
   - Update `docs/AI_ARCHITECTURE.md` if any core semantics change
6) Metal backend (next)
   - `PathSurfaceMetal` producing an offscreen `MTLTexture`
   - Presenter draws textured quad into `CAMetalLayer` drawable on the UI thread

## Open questions

- Present policy: per-app defaults for staleness vs always-fresh frames
- Snapshot GC triggers and retention policy (K snapshots vs revision-based)
- Text shaping and bidi strategy; font fallback
- Color management (sRGB, HDR) across software/GPU paths
- Direct-to-window bypass for single-view performance-critical cases (optional surface mode)
- Documentation and diagrams: add UI/Rendering section to `docs/AI_ARCHITECTURE.md` when APIs solidify; include Mermaid diagrams for data flow and schemas

## Gaps and Decisions (unresolved areas)

The following items are intentionally unresolved and tracked as backlog. Promote items to “Decision” when resolved and keep in sync with `docs/AI_TODO.task` and code/tests.

1) Lighting and shadows
- Software UI lighting model (directional light, Lambert/Blinn-Phong)
- Elevation-based shadow heuristics
- Opt-in normals/3D attributes for “2.5D” widgets





2) Resource system (images, fonts, shaders)
- Async loading/decoding, caches, eviction
- Asset path conventions
- Font fallback and shaping library selection

3) Error handling, observability, and profiling
- Error propagation style
- Structured logging/tracing per target/scene/frame
- Metrics and debug overlays

4) Documentation and diagrams
- Add UI/Rendering to `docs/AI_ARCHITECTURE.md` when APIs solidify
- Include Mermaid diagrams for data flow and schemas
- Keep images in `docs/images/` and cross-link to code/tests

5) HTML adapter fidelity targets
- DOM/CSS vs Canvas JSON vs WebGL
- Text shaping strategy for web output

6) Present policy
- Per-app defaults for staleness vs always-fresh frames

7) Snapshot GC and retention
- Triggers and policy (K snapshots vs revision-based)

## Decision: Snapshot Builder (resolved)

Summary:
- Maintain the previous snapshot in memory and apply targeted patches for small changes; perform full rebuilds only on global invalidations or when fragmentation/cost crosses thresholds

Approach:
- Patch-first: per-node dirty bits (`STRUCTURE`, `LAYOUT`, `TRANSFORM`, `VISUAL`, `TEXT`, `BATCH`) and epoch counters to skip up-to-date nodes
- Copy-on-write: assemble new revisions by reusing unchanged SoA arrays/chunks; only modified subtrees allocate new chunks
- Text shaping cache: key by `font+features+script+dir+text`; re-shape only dirty runs
- Chunked draw lists per subtree with k-way merge; re-bucket only affected chunks

Full rebuild triggers:
- Global params changed (DPI/root constraints/camera/theme/color space/font tables)
- Structure churn: inserts+removes > 15% or reparent touch > 5% of nodes
- Batching churn: moved draw ops > 30% or stacking contexts change broadly
- Fragmentation: tombstones > 20% in nodes/draw chunks
- Performance: 3 consecutive frames over budget, or `patch_ms ≥ 0.7 × last_full_ms`
- Consistency violation detected by validations

Publish/GC:
- Atomic write to `builds/<rev>.staging` then rename; update `current_revision` atomically
- Retain last 3 revisions or 2 minutes; defer GC while a renderer references an older revision

Notes:
- Renderer behavior is unchanged; it consumes the latest revision agnostic to patch vs rebuild
- If any core semantics change, reflect them in `docs/AI_ARCHITECTURE.md` and update tests accordingly

## Schemas and typing (v1)

This section specifies the C++ types bound to target keys and the versioning policy for renderer I/O.

C++ types per key:
- `scene` — `std::string`
  - App-relative path to the scene root, e.g., `"scenes/<sid>"`. Must resolve within the same app root
- `desc` — `SurfaceDesc | TextureDesc` (per target kind)
  - `SurfaceDesc`:
    - `size_px { int w, int h }`
    - `pixel_format` (enum)
    - `color_space` (enum)
    - `premultiplied_alpha` (bool)
  - `TextureDesc`:
    - `size_px { int w, int h }`
    - `pixel_format` (enum)
    - `color_space` (enum)
    - `usage_flags` (bitmask)
- `desc/active` — mirror of the adopted descriptor (`SurfaceDesc | TextureDesc`) for introspection
- `settings/inbox` — queue of whole `RenderSettingsV1` objects (writers insert; renderer takes and adopts last)
  - `RenderSettingsV1`:
    - `time { double time_ms, double delta_ms, uint64_t frame_index }`
    - `pacing { std::optional<double> user_cap_fps }` (effective = min(display, cap))
    - `surface { {int w,int h} size_px, float dpi_scale, bool visibility }`
    - `std::array<float,4> clear_color`
    - `camera` (optional): `{ enum Projection { Orthographic, Perspective }, float zNear, float zFar }`
    - `debug { uint32_t flags }` (optional)
- `settings/active` — single-value mirror of the last adopted `RenderSettingsV1` (optional)
- `render` — execution that renders one frame for this target (no payload)
- `output/v1/common/*` — single-value registers with latest metadata:
  - `frameIndex: uint64_t`
  - `revision: uint64_t`
  - `renderMs: double`
  - `lastError: std::string` (empty on success)
- `output/v1/software/framebuffer` — `SoftwareFramebuffer`:
  - `std::vector<uint8_t> pixels`
  - `int width, int height, int stride`
  - `enum pixel_format`, `enum color_space`, `bool premultiplied_alpha`
- `output/v1/metal/texture` — `MetalTextureHandle` (opaque handle or registry id + size/format/color_space)
- `output/v1/vulkan/image` — `VulkanImageHandle` (opaque handle or registry id + size/format/color_space/layout)

App-relative resolution helpers:
- `is_app_relative(std::string_view)`
- `resolve_app_relative(AppRoot, std::string_view)`
- `ensure_within_app(AppRoot, std::string_view resolved)`

Versioning policy:
- Settings and descriptors: unversioned at the path level (pure C++ in-process; producers/consumers recompile together). Keep the `V1` suffix in C++ type names for source-level evolution
- Outputs: versioned at the path level under `output/v1`. If an incompatible change is needed, add `output/v2` and keep `output/v1` during a deprecation window (update docs/tests accordingly)

## Target keys (final)

Target base:
- `<app>/renderers/<rendererName>/targets/<kind>/<name>`
- `kind ∈ { surfaces, textures, html }`

Keys under a target:
- `scene` — app-relative path to the scene root to render (must resolve within the same app root)
- `desc` — descriptor for the target (`SurfaceDesc | TextureDesc | HtmlTargetDesc`)
- `desc/active` — mirror written by renderer after reconfigure
- `status/*` — e.g., `reconfiguring`, `device_lost`, `message`
- `settings/inbox` — queue of whole `RenderSettings` objects (writers insert; renderer takes and adopts last)
- `settings/active` — single-value mirror written by renderer after adoption (optional)
- `render` — execution to render one frame for this target
- `output/v1/...` — latest outputs for this target:
  - `common/` — timings and metadata (`frameIndex`, `revision`, `renderMs`, `lastError`)
  - `software/framebuffer` — pixel buffer + metadata (width, height, stride, format, colorSpace, premultiplied)
  - `metal/texture` or `vulkan/image` — opaque GPU handles and metadata
  - `html/dom`, `html/commands`, `html/assets/*` — optional web outputs

## RenderSettings v1 (final)

- `time: { time_ms: double, delta_ms: double, frame_index: uint64 }`
- `pacing: { user_cap_fps: optional<double> }`  // effective rate = min(display refresh, user cap)
- `surface: { size_px:{w:int,h:int}, dpi_scale: float, visibility: bool }`
- `clear_color: [float,4]`
- `camera: { projection: Orthographic | Perspective, zNear:float, zFar:float }` (optional)
- `debug: { flags: uint32 }` (optional)

Invariants:
- Writers always insert whole `RenderSettings` to `settings/inbox` (no partial field writes)
- Renderer drains `settings/inbox` via `take()`, adopts only the last (last-write-wins), and may mirror to `settings/active`
- `scene` paths are app-relative and must resolve to within the same application root
- `output/v1` contains only the latest render result for the target

## Glossary

- App root: `/system/applications/<app>` or `/users/<user>/system/applications/<app>`
- App-relative path: a path string without leading slash, resolved against the app root
- Renderer target: a per-consumer subtree under `renderers/<id>/targets/<kind>/<name>`
- Snapshot: immutable render-ready representation of a scene at a point in time (`revision`)
- Revision: monotonically increasing version used for atomic publish/adoption

## Cross-references

- Update `docs/AI_ARCHITECTURE.md` when changes affect core behavior (paths, NodeData, WaitMap, TaskPool, serialization). Keep examples and path references stable; if files move, update references in the same change.