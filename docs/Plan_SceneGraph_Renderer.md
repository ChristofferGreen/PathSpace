# Handoff Notice

> **Handoff note (October 19, 2025):** This renderer plan documents the milestones completed before the current hand-off. New assistants should coordinate changes via `docs/AI_Onboarding_Next.md` and mirror updates in `docs/SceneGraphImplementationPlan.md`.

# PathSpace — Scene Graph and Renderer Plan

> **Context update (October 15, 2025):** All renderer milestones now track the assistant context introduced in this launch; treat previous context references as historical.
> **Decision update (October 16, 2025):** macOS presenters now use a CAMetalLayer-backed Metal swapchain instead of CoreGraphics blits so fullscreen windows avoid CPU copies; treat the Metal presenter as MVP-critical.
> **Follow-up (October 17, 2025):** Present pipeline must eliminate software framebuffer copies—PathSurfaceSoftware and the presenter will move to a shared IOSurface-backed buffer so the renderer writes directly into the drawable without memcpy.
Scope: UI surfaces, renderers, presenters, multi-scene targets; atomic params and snapshot-based rendering

## Goals
- Application-scoped resources: windows, scenes, renderers, and surfaces live under one app root so removing the root tears everything down
- Multi-scene renderers: a single renderer can render multiple scenes concurrently; consumers select a scene via per-target configuration
- Window-agnostic surfaces: offscreen render targets (software or GPU) that can be presented by multiple windows within the same application
- Typed wiring: small C++ helpers return canonical paths; avoid string concatenation; validate same-app containment
- Atomicity and concurrency: prepare off-thread, publish atomically, and render from immutable snapshots for both target parameters and scene data
- Cross-platform path: software renderer feeds a CAMetalLayer-backed Metal presenter on macOS for fullscreen performance; keep Vulkan as a future option and preserve software-only surfaces for tooling/tests.

## Application roots and ownership

Applications are mounted under:
- System-owned: `/system/applications/<app>`
- User-owned: `/users/<user>/system/applications/<app>`

Everything an application needs is a subtree below the app root. No cross-app sharing of surfaces or renderers. All references are app-relative (no leading slash) and must resolve within the app root. See docs/AI_PATHS.md for the canonical path namespaces and layout conventions.

## App-internal layout (standardized)

Path shorthand guide
- The docs use shorthand like targets/<tid>/... and windows/<win>/views/<view>/... to keep examples concise.
- Shorthand expands relative to the app root to full paths:
  - targets/<tid>/... => <app>/renderers/<renderer-id>/targets/<kind>/<name>/...
  - scenes/<sid>/... => <app>/scenes/<scene-id>/...
  - windows/<win>/views/<view>/... => <app>/windows/<window-id>/views/<view>/...
- In code and tests, prefer full app-root paths and validate containment. Shorthand is documentation-only.

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
          settings                      # single RenderSettings value (atomic whole-object replace)
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

### Widget Path Conventions (October 24, 2025)

- Widget roots live under `<app>/widgets/<widget-id>`. Each root owns widget-local state plus metadata:
  - `state` — the live widget state (e.g., toggle on/off, slider position).
  - `meta/kind` — canonical widget kind (`"button"`, `"toggle"`, `"slider"`, `"list"`, `"tree"`, `"stack"`).
  - `meta/style` (and friends such as `meta/label`, `meta/range`, `meta/items`, `meta/nodes`) — authoring inputs that builders reuse when redrawing or cloning.
  - `ops/inbox/queue` — FIFO queue for interaction events (`WidgetOp`) written by bindings.
  - `ops/actions/inbox/queue` — optional downstream queue populated by reducers so apps can consume high-level actions without polling.
  - Layout containers (stacks today) extend the namespace with `layout/{style,children,computed}` so layout recomputation stays observable.
- Widget display lists publish under `<app>/scenes/widgets/<widget-id>`. Builders stamp `meta/name` and `meta/description`, cache per-state drawables in `states/<state-name>/snapshot`, and point `current_revision` at the active state.
- Focus metadata uses the shared path `<app>/widgets/focus/current` (string name of the focused widget). Helper APIs (`Widgets::Focus::*`) keep focus publishes atomic and optionally queue auto-render events.
- Bindings enqueue dirty hints and auto-render events against the target they drive:
  - Dirty hints: `renderers/<rid>/targets/<kind>/<name>/dirty/rects` via `Renderer::SubmitDirtyRects`.
    - **Update (October 27, 2025):** Bindings must pass the widget’s recorded footprint rectangle verbatim; PathRenderer2D expands that footprint into tile-aligned damage and handles neighbouring redraws.
  - Auto-render: `renderers/<rid>/targets/<kind>/<name>/events/renderRequested/queue` when `auto_render=true`.
- Examples/tests assume identifiers stay app-relative and never cross app roots; helper APIs return typed `WidgetPath`, `ScenePath`, or `ConcretePath` values so callers do not hand-write strings.

By convention, examples mount widget demos under `apps/widgets_demo/` so snapshots, queues, and diagnostics mirror production layout. When adding new widgets, extend both the `<app>/widgets/<id>/meta/*` schema and the `<app>/scenes/widgets/<id>` tree; update `docs/AI_PATHS.md` if the namespace grows.

### Builder Usage Flow (October 24, 2025)

1. **Bootstrap the app container:** `Builders::App::Bootstrap` provisions renderer, surface, window, and present policy under the chosen app root. It returns typed handles plus the resolved target path, applied render settings, and surface descriptor.
2. **Author widgets:** `Builders::Widgets::Create{Button,Toggle,Slider,List,Tree,Stack}` create widget roots and publish their scenes. Helpers validate identifiers, clamp style parameters, and stamp `meta/kind` so diagnostics and focus routing work.
3. **Bind interactions:** `Builders::Widgets::Bindings::Create*Binding` wires a widget to a renderer target, establishing dirty-hint rectangles and the ops queue path. Bindings enqueue `WidgetOp` records (`HoverEnter`, `Press`, `SliderUpdate`, etc.) into `widgets/<id>/ops/inbox/queue`.
4. **Reducer loop:** `Builders::Widgets::Reducers::ReducePending` drains the ops queue, converts entries to `WidgetAction`, and (optionally) republishes actions to `widgets/<id>/ops/actions/inbox/queue`. Apps that do not need action replay can consume the returned vector directly.
5. **Focus navigation:** Configure `Widgets::Focus::MakeConfig` with the app root and optional auto-render target; call `Widgets::Focus::ApplyNavigation` to update `/widgets/focus/current` and emit auto-render events when focus changes. Pulsing highlights are on by default (toggle with `SetPulsingHighlight`); the renderer drives the 1 s pulse and derives ring tint from the widget’s accent color.
6. **Window refresh helpers:** `Builders::App::UpdateSurfaceSize` edits surface/target descriptors and renderer settings atomically, while `Builders::App::PresentToLocalWindow` mirrors IOSurface or framebuffer output into the local debug window (falling back gracefully when Metal textures are not shareable).

The widget gallery (`examples/widgets_example.cpp`) exercises this full stack, and corresponding UITests (`tests/ui/test_Builders.cpp`, `tests/ui/test_WidgetReducersFuzz.cpp`) validate the helper API contracts. Use those references when wiring new widgets or app flows.

### Troubleshooting Notes (October 24, 2025)

- **Stale visuals:** Confirm the renderer target is adopting dirty hints and auto-render events. Inspect `renderers/<rid>/targets/<kind>/<name>/events/renderRequested/queue` and `output/v1/common/frameIndex` to verify frames advance; bindings log reasons such as `present-skipped` or `age-ms` in queued events.
- **Missing widget ops:** Check `widgets/<id>/ops/inbox/queue` for stuck entries. Reducer doctests expect queues to drain; lingering items usually indicate the reducer loop halted. Read `widgets/<id>/state` to ensure reducer mutations are publishing.
- **Focus not updating:** Inspect `/widgets/focus/current` and confirm the path contains the focused widget name. Focus helpers gate auto-render on successful writes; a missing value usually means the widget `meta/kind` field was left unset.
- **Snapshot drift:** Widget scenes store per-state snapshots in `scenes/widgets/<id>/states/*`. If renders look stale, ensure builders republished the state scene and bumped `current_revision`. UI doctests cover this path; re-run with `PATHSPACE_UPDATE_GOLDENS=1` to refresh goldens when intentional.
- **Diagnostics:** `windows/<win>/diagnostics/metrics/live/views/<view>/present` mirrors present stats, while renderer metrics live under `renderers/<rid>/targets/<kind>/<name>/output/v1/common/*`. Errors surface in both `lastError` and `diagnostics/errors/live`; capture logs via `scripts/run-test-with-logs.sh` if the loop harness flakes.

## Entities and responsibilities

Entity (renderer-facing, renderable-only)
- Purpose: the renderable view of a domain object. The true/authoritative object lives elsewhere (physics/sim/scripts); the renderer never serves data back to them.
- Identity: stable per-Entity id suitable for caching, hit testing, and routing; derived deterministically from the source object id plus a stable sub-draw index.
- Components (renderer-facing): world transform (with transformEpoch), geometry/material/texture references (with contentEpoch), pipeline flags, and world-space bounds (sphere/box).
- Snapshot immutability: Entities and their referenced resources are immutable within a published revision; changes produce a new revision unless explicitly marked dynamic (streaming).

Per-item residency/storage policy (standard PathSpace values)
- Policy is defined per resource under authoring: `scenes/<sid>/src/resources/<rid>/policy/residency/*`:
  - allowed: backends the item may reside in, e.g., ["gpu","ram","disk"]
  - preferred: backend ordering hints, e.g., ["gpu","ram"]
  - durability: "ephemeral" | "cacheable" | "durable" (restart-survivable if not ephemeral)
  - max_bytes, cache_priority, eviction_group: admission/eviction controls
- The renderer’s resource manager enforces residency/eviction according to policy and watermarks; backend mapping (RAM/SHM/FS/GPU) is internal and opaque to callers.

Authoring concurrency (proposed vs resolved)
- Producers write proposals under `scenes/<sid>/src/objects/<oid>/proposed/<source>/<component>/*` (e.g., physics, script).
- A coalescer resolves to `.../resolved/<component>/*` using per-component policy: ownership, priority, or merge. Resolved components carry epochs/versions.
- The builder latches a consistent resolved view (epochs) and publishes an immutable snapshot under `scenes/<sid>/builds/<rev>/*`; then updates `current_revision`. Renderers latch `current_revision` per frame and never re-read mid-frame.


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
    - `targets/<target-id>/settings` — single whole `RenderSettings` value (atomic whole-object replace)
    - `targets/<target-id>/render` — execution that renders one frame for this target
    - `targets/<target-id>/output/v1/...` — per-target outputs and stats (software/GPU/HTML)
  - Target-id convention: use the consumer’s app-local path, e.g., `surfaces/<surface-name>` or `textures/<texture-name>`

## Atomicity and concurrency

### Render settings atomicity (per renderer target)

- Single-path settings:
  - Authoritative path: `settings`
  - Writers construct a complete `RenderSettings` object (or nested subtree) off-thread and atomically replace the value at this path in one insert.
  - Do not split settings across multiple child paths if you require atomic reads.
- Renderer latch (per frame):
  - At frame start, read `settings` once and use that snapshot for the entire frame (no mid-frame reads).
- Multi-producer guidance:
  - Prefer a single logical owner (aggregator) that merges inputs and performs the atomic replace; if multiple producers write directly, last-writer-wins applies at the single path.

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
1) Read settings once from `settings` (atomic single-path value for the whole frame). Mid-frame writes to `settings` do not affect the in-flight frame; adoption occurs at the next frame start (no cancellation).
2) Resolve `targets/<tid>/scene` against the app root; validate it stays within the same app subtree
3) Read `scenes/<sid>/current_revision`; latch for this render
4) Traverse `scenes/<sid>/builds/<revision>/...` and render:
   - Software: produce a framebuffer (pixels + stride)
   - GPU: render into an offscreen texture/image
5) Write `targets/<tid>/output/v1/...` and stamp `frameIndex` + `revision`

Presenter (per window view):
1) Read `windows/<win>/views/<view>/{surface, windowTarget, present/{policy,params}}` once per present; resolve to `renderers/<rid>/targets/surfaces/<sid>` or `renderers/<rid>/targets/windows/<wid>`; do not re-read mid-present
2) For surfaces:
   - Optionally call `surfaces/<sid>/render`, which:
     - Writes a whole `RenderSettings` to `renderers/<rid>/targets/surfaces/<sid>/settings`
     - Triggers `renderers/<rid>/targets/surfaces/<sid>/render`
   For windows targets:
   - Acquire the window drawable/swapchain for `<wid>` on the platform UI/present thread. On macOS this is `-[CAMetalLayer nextDrawable]`; presentation remains on that thread. Surface blits are limited to software-only fallbacks (tests/headless).
3) Present (backend/platform specifics):
   - macOS windows: bind the renderer output to an IOSurface-backed `MTLTexture`, write directly into that texture (software maps or GPU renders in-place), then call `-[CAMetalDrawable present]`. The legacy CoreGraphics blit path is retired for fullscreen.
   - Software (offscreen/headless surfaces): read framebuffer and blit into the caller-provided buffer when no Metal surface is available.
   - GPU (surface): draw a textured quad sampling the offscreen texture/image to the window drawable/swapchain.
   - GPU (windows target): present the acquired drawable/swapchain image (direct-to-window).
4) Record presenter metrics: write `frameIndex`, `revision`, `renderMs`, `presentMs`, `lastPresentSkipped`, and `lastError` under `targets/<tid>/output/v1/common/*` via `Builders::Diagnostics::WritePresentMetrics`. These values mirror the most recent present result and back diagnostics/telemetry.

Implementation note: `PathWindowView` now owns the CAMetalLayer, acquires `MTLDrawable` instances, and maps software framebuffers into IOSurface-backed textures so fullscreen presents are zero-copy. It still returns a `PresentStats` struct, and the helpers layer persists the metrics via `WritePresentMetrics`. UI doctests live in the `PathSpaceUITests` target to keep presenter/surface coverage isolated from the core suite.

Present policy (backend-aware)
- Modes:
  - AlwaysFresh
    - Software: UI thread waits up to a tight deadline for a new framebuffer; if not ready by deadline, skip this present rather than blitting stale pixels.
    - GPU: wait on the offscreen render fence until (vsync_deadline − ε) or frame_timeout_ms. If not signaled, either draw the previous offscreen texture or skip present (configurable).
    - HTML: treated as AlwaysLatestComplete (policy ignored).
  - PreferLatestCompleteWithBudget (default)
    - Software: if a fresh framebuffer can complete by now + staleness_budget_ms, wait; else blit last-complete. Never block the UI thread beyond budget.
    - GPU: if the offscreen fence is likely to signal by (vsync_deadline − ε), wait; else draw the previous offscreen texture this vsync.
    - HTML: AlwaysLatestComplete.
  - AlwaysLatestComplete
    - Software/GPU: present whatever output is complete now; do not wait.
    - HTML: same.

- Parameters:
  - staleness_budget_ms: float (default 8.0)
  - max_age_frames: uint (default 1)
  - frame_timeout_ms: float (default 20.0)
  - vsync_align: bool (default true)
  - auto_render_on_present: bool (default true)

- Semantics (per present):
  1) Latch target base and current_revision at present start.
  2) If auto_render_on_present and output age > staleness_budget_ms, enqueue a render (idempotent if already in-flight).
  2a) If output age in frames exceeds max_age_frames, enqueue a render (idempotent). Age in frames counts completed frames since the output’s revision was produced; skipped presents do not reset age.
  3) Compute deadline:
     - AlwaysFresh: deadline = min(vsync_deadline, now + frame_timeout_ms)
     - PreferLatestCompleteWithBudget: deadline = min(vsync_deadline, now + staleness_budget_ms)
     - AlwaysLatestComplete: deadline = now
  4) If a newer frame completes before deadline, present it; else present last-complete if policy allows.
     - AlwaysFresh: skip present when the deadline is missed.
     - If age_frames > max_age_frames and a fresh frame is not ready by the deadline:
       - PreferLatestCompleteWithBudget / AlwaysLatestComplete: present the last-complete and keep the queued render.
       - AlwaysFresh: still skip present.

- Backend notes:
  - Timing source: vsync_deadline comes from platform presentation timing APIs when available (e.g., CVDisplayLink/CAMetalLayer on macOS); otherwise estimate from a monotonic clock and known refresh.
- Software:
    - Keep double-buffering for buffered mode; when presenting to Metal windows, map the staging buffer into an IOSurface-backed `MTLTexture` so updates stream directly into the drawable instead of issuing a separate CoreGraphics blit.
    - Never block the UI thread longer than min(staleness_budget_ms, frame_timeout_ms) − blit_budget_ms. The fallback CPU blit path is reserved for headless/tests.
    - Zero-copy path (October 17, 2025): PathSurfaceSoftware now allocates IOSurface-backed staging/front buffers and PathWindowView exposes the shared IOSurface to CAMetalLayer presenters, eliminating the memcpy step. The legacy CPU copy path remains available for diagnostics and headless tests.
    - Incremental damage (October 17, 2025): PathRenderer2D diffs per-target drawable fingerprints and cached bounds so unchanged drawables—including id renames—leave the damage region empty, keeping repeated frames tile-free. Paint tooling (e.g., `examples/paint_example.cpp`) now forwards brush dirty rectangles via `targets/<tid>/hints/dirtyRects` and skips redundant snapshot publishes on resize-only frames, so large canvases regain interactive frame rates. (October 19, 2025: Builders coalesce dirty-rect hints into tile-aligned regions and expose damage/fingerprint/progressive counters behind `PATHSPACE_UI_DAMAGE_METRICS=1`.) Next steps: shard the encode/raster phase across per-tile worker queues so the software path leverages all CPU cores before we revisit encode offload.
  - GPU (Metal/Vulkan):
    - Use a fence/completion handler for offscreen completion; align waits to (vsync_deadline − ε). On macOS windows reuse the CAMetalLayer swapchain textures to avoid reallocations.
    - If drawable acquisition fails or is late, skip present and set a status message; keep last-complete texture for the next tick.
  - HTML:
    - Policy ignored; DOM/CSS adapter always presents the latest complete output without waiting.

- Configuration (presenter-owned; per view):
  - windows/<win>/views/<view>/present/policy: enum { AlwaysFresh, PreferLatestCompleteWithBudget, AlwaysLatestComplete }
  - windows/<win>/views/<view>/present/params:
    - staleness_budget_ms: float
    - max_age_frames: uint
    - frame_timeout_ms: float
    - vsync_align: bool
    - auto_render_on_present: bool
    - capture_framebuffer: bool (default false; enable only for diagnostics/tests that need CPU-visible pixels)

- Per-call overrides (Builders):
  - present_view(..., optional policyOverride, optional paramsOverride)

- Metrics (written by presenter to output/v1/common):
  - presentedRevision: uint64
  - presentedAgeMs: double
  - presentedMode: string
  - stale: bool
  - waitMs: double
  - progressiveTilesCopied / progressiveRectsCoalesced / progressiveSkipOddSeq / progressiveRecopyAfterSeqChange (software-only)
  - skippedPresent: bool (GPU)
  - drawableUnavailable: bool (GPU)

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
- DrawableId: 64-bit stable identifier scoped to a scene; reused only after a generation bump. Builders derive it deterministically as `hash64(sceneId, authoringNodeId, drawableIndexWithinNode)` and persist the accompanying authoring node id plus index in the authoring map.
- Generation: 32-bit counter incremented on reuse from a free-list; handle = (id, generation)
- Free-list and tombstones: removals push ids into a free-list with tombstone count; reuse occurs only when generation++ and all references to the old revision are gone
- Stability: within minor edits (no removal), drawables keep the same (id, generation); indices into SoA arrays may change between revisions, but ids do not
- Authoring map: a per-drawable record of `{DrawableId, authoringNodeId, drawableIndexWithinNode, generation}` stored in `bucket/authoring-map.bin` allows hit testing, tooling, and diagnostics to trace a runtime drawable back to its authored source without replaying commands.
- Font atlas manifest: `bucket/font-assets.bin` stores `{DrawableId, fontResourceRoot, atlasRevision, atlasFingerprint}` so renderers can prefetch the correct atlas revision without inspecting draw commands.

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
  - clip-heads.bin        — per-drawable singly linked-list heads into clip_nodes
  - clip-nodes.bin        — clip stack nodes (rect/path references) for hit testing without replaying commands
  - authoring-map.bin     — ordered authoring provenance (`DrawableId` ↔ authoring node id / drawable index / generation)
  - font-assets.bin       — per-drawable font atlas references (resource root, atlas revision, fingerprint)
  - indices/opaque.bin    — optional opaqueIndices[]
  - indices/alpha.bin     — optional alphaIndices[]
  - indices/layer/*.bin   — optional per-layer indices (see naming/format below)
  - meta.json             — small JSON with revision metadata, font-asset counts, and aggregate authoring-map statistics
  - trace/tlas.bin        — optional (software path tracer; instances carry AABBs)
  - trace/blas.bin        — optional (software path tracer; geometry bounds/AABBs per BLAS)
- Manifest: `scenes/<sid>/builds/<rev>/drawable_bucket` stores a compact binary manifest (version, drawable/command counts, layer ids) that loader helpers read before mapping the individual `*.bin` files. This manifest replaces the earlier Alpaca blob fallback; all consumers must migrate to the split-file schema.
- Binary headers:
  - All *.bin start with: magic(4), version(u32), endianness(u8), reserved, counts/offsets(u64), checksum(u64)
- Per-layer index naming/format:
  - File name: indices/layer/<layer>.bin where <layer> is the decimal text of the uint32 layer id (e.g., indices/layer/3.bin).
  - Encoding: little-endian, 64-bit aligned, with the same header (magic, version, endianness, counts/offsets, checksum).
  - Payload: a tightly packed array of uint32 indices into the drawables SoA for that layer; count is recorded in the header.

Publish/adopt/GC protocol
- Build writes to `scenes/<sid>/builds/<rev>.staging/*`, fsyncs, then atomically renames to `scenes/<sid>/builds/<rev>`
- Publish: atomically replace `scenes/<sid>/current_revision` with `<rev>`
- Renderer adopts by reading `current_revision` once per frame, mapping only `/<rev>/bucket/*` for that frame; the revision is pinned until frame end
- GC: retain last K revisions (default 3) or T minutes (default 2m), whichever greater; never delete a pinned revision. Effective deletion cutoff = min(count-cutoff, time-cutoff, (minActiveLeaseRev - 1) if leases present). Never delete `current_revision` and always retain at least one revision.
- Observability: renderer writes `frameIndex`, `revision`, `renderMs`, `lastError`, plus counters such as `drawableCount`, `opaqueDrawables`, `alphaDrawables`, `culledDrawables`, `commandCount`, `commandsExecuted`, and `unsupportedCommands` under `targets/<tid>/output/v1/common/*` (see “Target keys (final)”).

> Note — Revision allocation and error handling
> - current_revision type and allocation: uint64, builder-assigned, strictly monotonically increasing per scene. Builders are responsible for generating new revisions; do not reuse old values.
> - Renderer behavior on read failure:
>   - If `scenes/<sid>/current_revision` is missing or unreadable, set a concise message in `targets/<tid>/output/v1/common/lastError` and skip the frame (or render a clear color) without crashing.
>   - If `current_revision` points to a missing or partial `scenes/<sid>/builds/<rev>/*`, also report a concise error and skip/clear; recover automatically when a valid revision is published.
> - Adoption invariants: renderers latch `current_revision` once at frame start; all reads during the frame must come from that latched revision.

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
  - DisplayP3 handling: when assets declare DisplayP3 or targets request DisplayP3, convert between working linear space and the target/display space via ICC or well-defined matrix transforms. For software paths, encode to sRGB or DisplayP3 at store-time; for GPU, use appropriate formats and perform conversion on write-out. Avoid double conversion when using sRGB/linear attachments.

> **Current implementation (October 17, 2025):** `PathRenderer2D` composites Rect, RoundedRect, Image, TextGlyphs, Path, and Mesh commands in linear light, honors opaque/alpha pass ordering, and records drawable/command/unsupported metrics alongside progressive-surface data. Analytic coverage improvements and advanced blending remain planned follow-ups.
- Builders’ `Surface::RenderOnce` / `Window::Present` now call this renderer synchronously and return a ready `FutureAny`; async execution queues remain future work.

### Color management (v1)

Policy
- Working space: linear light for all shading and blending.
- Framebuffer encoding: default sRGB 8-bit with linear→sRGB encode on write; optional linear FP formats (e.g., RGBA16F/32F) for HDR.
- Alpha: premultiplied everywhere by default.

Inputs
- Solid colors are authored in sRGB; convert to linear at material upload.
- Images: honor embedded ICC when present; otherwise assume sRGB. Convert to the working space at decode, or sample from sRGB textures with automatic decode-to-linear.
- Text/MSDF: decode in linear; blend premultiplied in linear. LCD subpixel only when the target is sRGB8 and transforms preserve subpixel geometry.

Flags and semantics
- SrgbFramebuffer: target expects sRGB-encoded output; encode on write-out; blending remains in linear.
- LinearFramebuffer: target is linear (e.g., FP16/FP32); do not sRGB-encode on write.
- UnpremultipliedSrc: source is straight alpha; convert to premultiplied in the draw path before blending; discourage use for general content.

Backend notes
- Software: composite in linear float; encode to target color space on store; optional dithering for 8-bit.
- Metal/Vulkan: prefer sRGB formats with automatic decode-to-linear sampling; select FP formats for HDR paths.

Defaults and tests
- Defaults: sRGB working space, srgb8 framebuffer, premultiplied alpha.
- Tests: golden semi-transparent composites, software vs GPU parity, MSDF edge quality and LCD downgrade behavior.

### Progressive present (software, non-buffered)

Overview
- Low-latency mode for the software path using a single shared framebuffer with progressive tile updates.
- Presenter blits only tile-aligned dirty regions that are consistent; no full-frame commit is required.

Shared framebuffer and tiles
- One CPU pixel buffer sized to the target (physical pixels; respect dpi_scale). Pixel format: `RGBA8Unorm_sRGB` with premultiplied alpha. Renderers composite in linear light and encode to sRGB on store; presenters blit the bytes unchanged so no second encode ever occurs.
- Fixed tile size (e.g., 32×32 or 64×64). Per-tile metadata lives next to the pixels and is fully atomic:
  - `seq` (`std::atomic<uint32_t>`): even = stable, odd = writer active.
  - `pass` (`std::atomic<uint32_t>`): 0 = None, 1 = OpaqueInProgress, 2 = OpaqueDone, 3 = AlphaInProgress, 4 = AlphaDone.
  - `epoch` (`std::atomic<uint64_t>`): monotonic counter for AlphaDone completion; readers drop tiles whose epoch lags the most recent frame.

Renderer protocol per tile
- Writers flip `seq` to odd via `fetch_add(1, memory_order_acq_rel)` before touching pixel memory. Optional: set `pass` to `OpaqueInProgress` or `AlphaInProgress` with `memory_order_release` so readers can short-circuit.
- After writing pixels, issue `std::atomic_thread_fence(memory_order_release)` to flush stores, then set the new pass state (`OpaqueDone` or `AlphaDone`) with `memory_order_release`. When a tile reaches `AlphaDone`, store the new `epoch` with `memory_order_release` before making the tile visible.
- Finish by flipping `seq` back to even with `fetch_add(1, memory_order_acq_rel)`. Readers perform `memory_order_acquire` loads of `seq`, `pass`, and `epoch`. If `seq` is odd or differs before/after a copy, the presenter discards the in-flight data to avoid tearing.

Dirty region feed
- Renderer enqueues tile-aligned dirty rects to a lock-free queue.
- Presenter drains and coalesces rects on the UI thread; for each tile:
  - Read seq; if odd, skip; if even, copy; re-check seq after copy; if changed, discard that tile copy (avoid tearing).
- Use platform partial redraw APIs (e.g., setNeedsDisplayInRect) to schedule partial blits.

Two-phase progressive draw (optional)
- Present opaque-complete tiles immediately for quick stabilization; blend alpha tiles progressively as they complete.

Input-priority micro-updates
- Pointer interactions enqueue high-priority dirty regions around the cursor/control (e.g., 64–128 px neighborhood) to maintain perceived low latency.

Policy interaction
- PreferProgressive (software-only) can be selected per view or auto-chosen when budget is tight.
- Mapping:
  - AlwaysLatestComplete: buffered; if presentedAgeMs > staleness_budget_ms, temporarily switch to progressive until stabilized.
  - PreferLatestCompleteWithBudget (default): progressive for late tiles beyond budget; otherwise buffered.
  - AlwaysFresh: prefer buffered; progressive only if a full frame cannot meet the deadline.

- Settings (per view)
- windows/<win>/views/<view>/present/policy: may be PreferProgressive on software targets
- windows/<win>/views/<view>/present/params.software.progressive:
  - enable: bool (default true)
  - tile_size: int (32|64)
  - alpha_two_phase: bool (default true)
  - max_dirty_per_vsync: int (coalescing cap)
- Pacing note: user pacing caps (user_cap_fps) do not change `RenderSettings.time.delta_ms` beyond the actual elapsed time between frames; if frames are skipped due to pacing, `delta_ms` still reflects real elapsed time so animations advance correctly.

Metrics (software presenter)
- progressiveTilesCopied, progressiveRectsCoalesced, progressiveSkipOddSeq, progressiveRecopyAfterSeqChange
- presentLatencyMs (min/avg/max), copyBytesPerSecond

Safety and correctness
- Seqlock per tile avoids locks and prevents torn reads.
- Presenter never blocks the UI thread on renderer work; cap coalesced rects per vsync to avoid starvation.
- macOS note: shared buffer → Core Graphics blits of dirty rects; IOSurface/Core Animation can be considered later to reduce copies.

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
- Regardless of framebuffer format, shading and blending are performed in linear light. When `SrgbFramebuffer` is set, texture sampling decodes to linear and framebuffer writes re-encode to sRGB; ensure no code path applies an extra encode. Add regression tests whenever blend paths change to keep this invariant intact.

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
- Projection: Orthographic with y-down. Screen origin at top-left of the target; +X right, +Y down. Matrices are column-major; vectors are column vectors; compose with post-multiply (world = parentWorld * local; clip = P*V*M*pos).
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
- **Status (October 16, 2025):** `Scene::HitTest` now supplies scene-space and local-space coordinates alongside per-path focus metadata while walking opaque + alpha buckets back-to-front and respecting rect clips. Dirty markers surface via `Scene::MarkDirty` (`diagnostics/dirty/state`) and `Scene::DirtyEvent` queue entries, giving renderers a wait/notify hook for scheduling rebuilds. Shape-specific narrow-phase tests (rounded rect curves, image alpha sampling, text glyph coverage) remain future work.

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

> **Status (October 20, 2025):** `PathRenderer2DMetal` now executes rects, rounded rects, text quads, and textured images directly on the GPU. Material/shader bindings (and glyph/material pipelines) remain on the roadmap; until then the renderer falls back to software for unsupported drawables.

Device and queue ownership
- Each renderer instance owns its GPU device/context and one or more queues:
  - Metal: MTLDevice + MTLCommandQueue (one graphics queue per renderer; optional transfer-only queue in future)
  - Vulkan: VkDevice + graphics queue (and present queue when applicable); command pools are per-thread
- Surfaces/textures render offscreen; windows targets render directly to the window drawable/swapchain. Presenters handle acquisition and present on the UI/present thread (e.g., CAMetalLayer).

Thread affinity and submission
- Command encoding happens on a renderer worker thread per target (no UI thread dependency)
- Metal objects with thread affinity (e.g., CAMetalDrawable) are handled by the presenter. For surfaces/textures, renderers consume offscreen textures/images; for windows targets, the presenter acquires the drawable and the renderer encodes directly against it before the presenter submits/presents on the UI thread.
- Vulkan command pools are thread-bound; allocate and reset per-thread pools

Synchronization model
- Per-target synchronization only; no global renderer lock
- CPU side:
  - Short mutex to read `settings` at frame start; snapshot read is lock-free after latching `current_revision`
- GPU side:
  - One command buffer per frame per target (or a small ring); fences/semaphores wait for completion before resource reuse
  - Avoid cross-target synchronization; each target’s timeline is independent
- Present:
  - Software: blit framebuffer to window on UI thread (unchanged)
  - Metal:
    - Offscreen targets (surfaces/textures): presenter draws a textured quad sampling the offscreen texture into CAMetalDrawable
    - Windows targets: renderer renders directly into CAMetalDrawable acquired by the presenter; presenter calls presentDrawable on the UI thread
  - Vulkan:
    - Offscreen targets: presenter composites the offscreen image/quad into the view’s drawable
    - Windows targets: renderer renders directly into the acquired swapchain image; presenter queues vkQueuePresentKHR and handles SUBOPTIMAL/OUT_OF_DATE by recreating the swapchain

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
- Renderer diagnostics extend the same subtree with `opaqueSortViolations`, `alphaSortViolations`, `approxOpaquePixels`, `approxAlphaPixels`, `approxDrawablePixels`, `approxOverdrawFactor`, `progressiveTilesUpdated`, and `progressiveBytesCopied` so tooling can spot ordering regressions, overdraw spikes, and progressive-copy churn.
- Renderer diagnostics extend the same subtree with `opaqueSortViolations`, `alphaSortViolations`, `approxOpaquePixels`, `approxAlphaPixels`, `approxDrawablePixels`, `approxOverdrawFactor`, `progressiveTilesUpdated`, and `progressiveBytesCopied` so tooling can spot ordering regressions, overdraw spikes, and progressive-copy churn. (2025-10-18: a richer tile/damage/fingerprint metric set was prototyped but rolled back after fullscreen perf regressed; reintroduce behind a perf-safe guard.)
- Presenter metrics now include `presentMode`, `stalenessBudgetMs`, `frameTimeoutMs`, `maxAgeFrames`, `presentedAgeMs`, `presentedAgeFrames`, `stale`, `autoRenderOnPresent`, and `vsyncAlign` under `output/v1/common/*`, reflecting the resolved `present/policy` + `present/params` values for downstream tooling. Progressive copy diagnostics now also capture `progressiveTilesUpdated` + `progressiveBytesCopied`, matching renderer-side counters.
- Optional GPU counters (backend-specific) may be exposed under a debug subtree for diagnostics; avoid mandatory dependencies on profiling APIs

## Error handling and observability (plan)

Goals
- Keep error reporting at the PathSpace layer so renderers, surfaces, windows, scenes, and applications all share the same machinery
- Standardize what metadata accompanies an error and how tooling discovers it
- Provide lightweight logging/metrics hooks that the frame profiler and future dashboards can reuse

### PathSpaceError struct

```cpp
struct PathSpaceError {
  enum class Severity : uint32_t { Info = 0, Warning, Recoverable, Fatal };

  int                                               code;       // partitioned ranges: 0-999 core, 1000-1999 system, 2000-2999 renderer, 3000-3999 UI, 4000-4999 app
  Severity                                          severity;   // coarse impact classification
  std::string                                       message;    // human-readable summary
  std::string                                       path;       // PathSpace node the error is attached to (app- or system-relative)
  uint64_t                                          revision;   // generation/version number associated with the error (scene rev, target rev, etc.)
  std::chrono::steady_clock::time_point             timestamp;  // time of insertion (ordering + GC hints)
  std::string                                       detail;     // optional structured payload (JSON/text); empty when unused
};
```

- Error ownership: the struct lives exactly under the PathSpace node that emitted it (e.g., `renderers/<id>/diagnostics/errors/live`). Lifetime is governed by that node; when a node is removed or its retention policy expires, the attached error is garbage-collected automatically.
- Code partitioning keeps ownership clear while allowing plenty of space for subsystem-specific codes. Subsystems can define their own tables under their allotted range without clashing with core PathSpace errors.
- `revision` and `path` give enough context for tooling to link errors back to specific snapshots, renderer targets, or authoring nodes without walking extra metadata trees.

### Diagnostics layout

- Latest error per domain stored under `diagnostics/errors/live`; significant errors optionally appended under `diagnostics/errors/history/<id>` with the same payload.
- (October 18, 2025) Presenters and renderers now always write a canonical `PathSpaceError` to `diagnostics/errors/live`, mirroring the message string to `output/v1/common/lastError` for legacy consumers.
- Structured logs use small immutable ring segments (`diagnostics/log/ring/<segment>`) with timestamped entries (category, severity, message, optional detail). Segments roll at fixed size so the profiler can tail them cheaply.
- Rolling metrics live under `diagnostics/metrics/live` (render/present timings, drawable counts, cull stats, resource residency, etc.). Writers update once per frame; consumers can compute aggregates on demand.
- Debug overlays (when enabled via `debug.flags`) publish drawable overlays to `diagnostics/overlays/<kind>` so both in-app views and tooling can visualize issues in-frame.
- All diagnostics paths are optional; when absent, consumers fall back to the minimal `output/v1/common/*` data.

### Tooling expectations

- Frame profiler: polls `errors/live`, rings, and metrics to populate UI panels and annotate frame samples; capture requests simply snapshot these paths.
- CLI tooling: `pathspace diag` commands can dump the same paths for CI triage without needing app-specific logic.
- Tests: helper assertions verify that particular operations emit (or clear) expected error codes by reading the attached PathSpaceError payloads.

### Presenter metrics (software path)

`targets/<tid>/output/v1/common/*` now exposes a richer presenter snapshot every time `Window::Present` completes. Alongside the existing counters (`frameIndex`, `revision`, `renderMs`, `presentMs`, `lastPresentSkipped`, `lastError`, `progressive*`), the presenter writes:

- `presentMode` — resolved enum string from `views/<view>/present/policy`.
- `stalenessBudgetMs`, `frameTimeoutMs`, `maxAgeFrames` — effective policy parameters after applying overrides in `views/<view>/present/params/*`.
- `autoRenderOnPresent`, `vsyncAlign` — booleans that document whether the presenter attempted proactive renders or vsync alignment.
- `presentedAgeMs`, `presentedAgeFrames` — age of the output that was just displayed, relative to the prior present; derived from the policy’s frame timeout.
- `stale` — true when `presentedAgeFrames` exceeds `maxAgeFrames` (used to gate auto-render).
- `progressiveTilesUpdated`, `progressiveBytesCopied` — renderer-side estimates of progressive workload for the most recent render.
- `backendKind`, `usedMetalTexture` — backend telemetry (Software2D vs Metal2D and whether a Metal texture was presented) so dashboards can spot software fallbacks while GPU upload remains gated behind `PATHSPACE_ENABLE_METAL_UPLOADS`.
- `gpuEncodeMs`, `gpuPresentMs` — CPU-sampled durations for Metal blit encoding and drawable scheduling when the CAMetalLayer path runs (0.0 for software presents). A PathSpaceUITest (`test_PathWindowView_Metal.mm`) covers this path when `PATHSPACE_ENABLE_METAL_UPLOADS=1` is set on macOS Metal runners.

Downstream tooling should consume these fields instead of recomputing policy state, and CI expectations should pin exact values for deterministic unit scenarios (see `tests/ui/test_PathRenderer2D.cpp` / `test_PathWindowView.cpp`).

### Minimal types (sketch)

```cpp
struct Transform { float m[16]; }; // Column-major 4x4; column vectors; post-multiply (world = parentWorld * local; clip = P*V*M*pos). 3D; 2D via orthographic with z=0

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
- Z-order: by `(zIndex asc, then children[] order)` within a parent. Mapping to runtime: the snapshot builder assigns `layer` from the nearest layer-bearing ancestor (default 0) and derives per-drawable `z` from authoring `zIndex` within that layer; ties are broken by stable `DrawableId` for determinism.

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

Implementation notes (October 23, 2025):
- `Widgets::CreateStack`/`UpdateStackLayout` publish stack metadata under `widgets/<id>/layout/{style,children,computed}` and emit aggregated snapshots (`scenes/widgets/<id>`). Stack children reference existing widget scenes, so layout stays in sync with button/toggle/slider/list state styling.
- `Widgets::Bindings::CreateStackBinding` wires stack layouts to renderer targets; `WidgetBindings::UpdateStack` recomputes layout when spacing/alignment changes and forwards dirty hints/auto-render requests automatically.
- UITests (`tests/ui/test_Builders.cpp`) cover layout measurement, spacing/alignment, and dirty propagation. Gallery integration still pending (track in Plan_SceneGraph_Implementation Phase 8 follow-ups).

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
- Commit barrier monotonicity: `<app>/scenes/<sid>/src/commit` is a monotonically increasing uint64 counter; writers increment it by at least 1 per publish. Readers ignore equal or lower values.
- Renderers may watch target scene subtrees to mark targets dirty
- Modes:
  - Explicit: surfaces/presenters trigger frames
  - On-notify: renderer schedules frames for dirty targets
- **Progress (October 16, 2025):** Pointer hit tests now queue `AutoRenderRequestEvent` values under `renderers/<rid>/targets/<kind>/<name>/events/renderRequested/queue`; extend this with dirty-mark notifications and presenter wakeups.

## Safety and validation

- App-relative resolution: a path without a leading slash is resolved against the app root
- Same-app validation: after resolution, verify the target path still lies within the app root; reject otherwise
- Symlink/alias containment (filesystem-backed paths): when resolving any filesystem path, resolve symlinks/aliases to absolute paths and verify containment within the app root to prevent escapes.
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
  - Param updates (Queue) are client-side coalescing, then a single atomic write to `<target>/settings` (no server-side queue in v1)
  - Snapshot revision flip (single write) for scene publish (builder concern)
- Provide readable errors with context (target-id, frame index, snapshot revision)

Helper API (schema-as-code; returns canonical absolute paths; excerpt validated by doctests in `tests/ui/test_Builders.cpp` and `tests/ui/test_SceneSnapshotBuilder.cpp`):
```cpp
namespace SP::UI {

using AppRoot = std::string;
using UnvalidatedPathView = SP::UnvalidatedPathView;

struct SceneParams    { std::string name; std::string description; };
enum class RendererKind { Software2D, Metal2D, Vulkan2D };
struct RendererParams { std::string name; RendererKind kind; std::string description; };
struct SurfaceDesc    { SizePx size; PixelFormat format; ColorSpace color; bool premultiplied; int progressive_tile_size_px; MetalSurfaceOptions metal; };
struct SurfaceParams  { std::string name; SurfaceDesc desc; std::string renderer; };
struct WindowParams   { std::string name, title; int width=0, height=0; float scale=1.0f; std::string background; };

Expected<std::string> resolve_app_relative(AppRoot const& appRoot, UnvalidatedPathView maybeRel);
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
enum class ParamUpdateMode { Queue, ReplaceActive }; // v1 semantics: Queue = client-side coalesce then atomic single-path replace; no server-side queue

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

} // namespace SP::UI
```

Usage notes:
- Treat all `Create` helpers as idempotent: if the target subtree already exists, the helper returns the canonical absolute path without rewriting metadata. (Verified by `Scene::Create` doctest on October 14, 2025.)
- `Renderer::UpdateSettings` performs a single-path atomic replace after draining any queued settings values under `<target>/settings`; callers should coalesce state before invoking it (doctest coverage ensures stale entries are discarded).
- `Surface::SetScene` and `Window::AttachSurface` enforce shared app roots, rejecting cross-app bindings with a typed `InvalidPath` error so misuse is caught during integration.
- The Builders suite is exercised in the 15× compile loop (`./scripts/compile.sh --loop=15 --per-test-timeout 20`) to guarantee the helpers remain race-free under load.
- `SceneSnapshotBuilder` serializes drawable buckets with explicit SoA fields (world transforms, bounding spheres/boxes with validity flags, material/pipeline metadata, command offsets/counts, command stream, pre-filtered opaque/alpha indices, and per-layer index blocks) plus per-revision resource fingerprints _and_ per-drawable hashes persisted in `fingerprints.bin`; doctests validate round-tripping and retention behaviour, and legacy snapshots recompute hashes on decode.

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
option(PATHSPACE_ENABLE_UI "Build UI (surfaces/renderers/windows)" OFF)
option(PATHSPACE_UI_SOFTWARE "Enable software surface" ON)
option(PATHSPACE_UI_METAL "Enable Metal surface/presenter (Apple)" ${APPLE})

if(PATHSPACE_ENABLE_UI)
  target_compile_definitions(PathSpace PUBLIC PATHSPACE_ENABLE_UI=1)
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
    target_compile_definitions(PathSpace PUBLIC PATHSPACE_UI_SOFTWARE=1)
    target_sources(PathSpace PRIVATE
      src/pathspace/ui/surface/PathSurfaceSoftware.cpp
      src/pathspace/ui/surface/PathSurfaceSoftware.hpp
    )
  else()
    target_compile_definitions(PathSpace PUBLIC PATHSPACE_UI_SOFTWARE=0)
  endif()
  if(APPLE AND PATHSPACE_UI_METAL)
    target_compile_definitions(PathSpace PUBLIC PATHSPACE_UI_METAL=1)
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
  else()
    target_compile_definitions(PathSpace PUBLIC PATHSPACE_UI_METAL=0)
  endif()
else()
  target_compile_definitions(PathSpace PUBLIC
    PATHSPACE_ENABLE_UI=0
    PATHSPACE_UI_SOFTWARE=0
    PATHSPACE_UI_METAL=0
  )
endif()
```

## Examples (creation with parameter structs)

```cpp
#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
using namespace SP;
using namespace SP::UI;

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
  SurfaceDesc sdesc; sdesc.size_px = {1280, 720};
  SurfaceParams sparams{ .name = "editor", .desc = sdesc, .renderer = "2d" };
  auto surfacePath = create_surface(space, app, sparams).value();

  // 4) Create a window
  WindowParams wparams{ .name = "MainWindow", .title = "Notepad — Main", .width = 1280, .height = 720, .scale = 2.0f };
  auto windowPath = create_window(space, app, wparams).value();

  // 5) Wire: attach surface to window view and pick a scene for the surface
  attach_surface_to_view(space, app, "MainWindow", "editor", "editor").value();
  set_surface_scene(space, app, surfacePath, "scenes/main").value();

  // 6) Update settings (queued: client-side coalesce, then a single atomic write) and render once
  RenderSettings rs; rs.surface.size_px = {1280, 720}; rs.surface.dpi_scale = 2.0f;
  update_target_settings(space, app, "renderers/2d/targets/surfaces/editor", rs).value();
  auto fut = render_target_once(space, app, "renderers/2d/targets/surfaces/editor").value();

  // 7) Present the view
  present_view(space, app, windowPath, "editor").value();
}
```
`SurfaceDesc` captures immutable framebuffer dimensions and pixel format; per-present scaling (DPI) is controlled via `RenderSettings::surface.dpi_scale`.

## HTML/Web output (optional adapter)

Motivation: preview/export UI scenes to browsers without changing the core pipeline

Approach: semantic HTML/DOM adapter mapping widgets to native elements with CSS-based layout/animation. A compact Canvas JSON stream is emitted as a fallback when DOM fidelity limits are exceeded; no WebGL path is required in the MVP.

Paths (under a renderer target base):
- `output/v1/html/dom` — full HTML document as a string (may inline CSS/JS)
- `output/v1/html/css` — CSS string, if split
- `output/v1/html/commands` — Canvas JSON fallback command stream (ordered draw commands)
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
- DOM/CSS is preferred when the rendered scene stays within fidelity limits: active node count ≤ `HtmlTargetDesc::max_dom_nodes`, clips are rect or rounded-rect, and blend modes stay within CSS-compatible cases. If any constraint is violated, the adapter emits a Canvas JSON stream alongside the DOM so preview tooling can fall back automatically.
- Canvas JSON carries the same draw order as the snapshot and allows lossless replay in browsers that cannot honor the DOM/CSS output.
- Text fidelity improves if we pre-shape glyphs in the snapshot and emit positioned spans
- Optional adapter; does not affect software/GPU outputs

## Software renderer — path tracing with tetrahedral acceleration

Status: resolved plan for software renderer pipeline

Summary:
- Scenes are often static; we prioritize incremental refinement via path tracing with snapshot-integrated acceleration structures
- Acceleration is snapshot-integrated:
  - TLAS per revision over instances; BLAS per unique geometry (deduped)
  - Tetrahedral face adjacency enables “tet-walk” traversal between neighboring tets for multi-bounce paths
- Emissive geometry (tetrahedral faces and SDF surfaces) doubles as the area-light list; the snapshot builder emits immutable emitter tables so sampling stays consistent with the published revision.
- GPU execution favors portable compute/ray-tracing pipelines (Metal, Vulkan, etc.) with a shared sampling/integration core that also powers the SIMD CPU fallback—no CUDA-specific path.

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
   - Direct light via emitter sampling (tet faces/SDF surfaces) plus DI reservoir + BSDF importance sampling (MIS)
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

## Plan: Resource system (images, fonts, shaders)

Goals
- Publish a canonical asset namespace so renderers, snapshot builders, and tooling share the same handles
- Load and decode assets asynchronously with deterministic lifetimes and clear diagnostics
- Persist reusable products (glyph atlases, decoded images, compiled shaders) across frames and application runs

### Asset layout and metadata
- Standardize per-app resources under `resources/{images,fonts,shaders}/<name>/...` with immutable `builds/<revision>` payloads and an `active` pointer
- Registration records capture file origin, format, color space, and content hashes; references are always app-relative and validated against the app root
- Scenes and renderers store resource fingerprints alongside drawables so revisions can declare exactly which assets they require

### Loader and decode pipeline
- Writers enqueue work at `resources/<kind>/<name>/inbox`; a resource service drains the queue on worker threads
- Each job resolves the asset path, streams the bytes, decodes or shapes them, and publishes the result as a new build revision before atomically updating `active`
- Errors surface under `resources/<kind>/<name>/diagnostics/errors` with `PathSpaceError` payloads so tooling can attribute failures

### Caching and lifetime management
- Maintain an in-memory LRU keyed by `{kind, fingerprint}` with reference counts provided by scenes and renderer targets
- Evict entries when the refcount reaches zero and the entry ages beyond a per-kind TTL; retain at least the revisions referenced by active scenes to avoid thrash
- Persist blob fingerprints in snapshot metadata so renderer targets can skip redundant uploads when a revision reuses the same asset payload

### Font stack and shaping
- Wrap HarfBuzz + ICU (or equivalent) in a `FontManager` that maps logical font requests to fallback chains recorded under `resources/fonts/<name>/<style>/meta/{family,style,weight,fallbacks}`
- Cache shaped glyph runs keyed by `(text, script, direction, font-set, features)` and publish glyph atlas textures under the font resource subtree
- Snapshot publishes the atlas fingerprint per drawable so renderers can pin the correct atlas revision without reshaping

### Shader compilation and persistent cache
- Describe shaders via `resources/shaders/<name>/src` (source, macros, entry points) and compile through backend adapters (Metal library, SPIR-V, etc.)
- Store compiled binaries under `resources/shaders/<name>/builds/<revision>` with metadata summarizing target backend, options, and validation stamps
- Keep a disk-backed cache keyed by `(source hash, compile options, backend)` so subsequent application runs can adopt existing binaries without recompiling
- Renderers mirror the adopted revision to `resources/shaders/<name>/active` and retain live references so GC does not drop binaries in use

### Integration points
- Snapshot builder emits the set of resource fingerprints per scene revision, letting renderer targets prefetch via `resources/<kind>/<name>/prefetch`
- Surfaces and presenters resolve resource paths through the existing typed helpers, ensuring only app-contained assets are bound to drawables
- Loader exposes lightweight metrics (queue depth, decode ms, cache hits) under `resources/<kind>/<name>/diagnostics/metrics` for profiling

## Plan: HTML adapter fidelity targets

- Introduce an `HtmlAdapter` interface so renderer targets can pick a fidelity mode (Canvas JSON replay, DOM/CSS, WebGL) without touching scene graph code. The adapter owns capability detection and wires renderer outputs to the chosen runtime.
- Ship a baseline Canvas replay: emit a compact JSON command stream mirroring the `DrawableBucket` ordering (opaque first, alpha second) plus resource fingerprints; bundle a lightweight JS runtime that replays commands on a `<canvas>` with batched state changes.
- Gate optional WebGL extensions behind capability flags: reuse the same command stream but attach buffer/texture uploads so browsers that support accelerated paths can render via shaders; fall back to Canvas replay when unavailable.
- Provide DOM/CSS shims only for widgets that map directly (text, simple rects) and layer them on top of the Canvas runtime for accessibility tooling; keep parity by sourcing from the same manifest.
- Publish fidelity metadata alongside `output/v1/html/commands` so presenters can pick the highest supported mode and tooling can diff frames irrespective of runtime choice.

### Implementation roadmap (Oct 18, 2025)
1. Finalize `Html::Adapter` emit API, option struct, and experimental flag exposure in builders so HTML targets opt in explicitly.
2. Implement DOM serializer with node budget enforcement, clip/transform support, and Canvas JSON fallback encoder that reuses the draw traversal order.
3. Wire resource fingerprints (images/fonts/shaders) into `output/v1/html/assets/*`, deferring heavy loaders to the resource-integration follow-up.
4. ✅ (October 19, 2025) Added replay harness: doctests compare adapter output against the software renderer, a standalone dump tool emits the canvas command stream, and replay parity is enforced in-tree.
5. ✅ (October 19, 2025) Integrated with the present pipeline: renderer targets publish HTML outputs plus diagnostics/errors, window views bind to `htmlTarget`, and presenters always consume the latest-complete frame.
6. ✅ (October 19, 2025) Added headless verification (Node-based canvas replay script) to CI with graceful skips when Node.js is unavailable and documented operator-facing troubleshooting steps.

Headless verification now runs through `scripts/verify_html_canvas.js`, which spawns the `html_canvas_dump` helper to emit the latest Canvas command stream and compares widget-themed scenes against a native PathRenderer2D render digest. The harness validates structural fields (types, dimensions, radii), palette CSS, and typography-sensitive colors for both the default and sunset themes, while ensuring the replayed framebuffer hash matches the native baseline. The CTest entry `HtmlCanvasVerify` executes the script when Node.js is available and emits a skip message otherwise, keeping CI stable on minimal builders.

### HTML adapter configuration & troubleshooting (October 19, 2025)

- Target knobs  
  - `HtmlTargetDesc::max_dom_nodes` caps DOM output complexity. When the adapter exceeds the limit it automatically downgrades to the Canvas JSON path; `output/v1/html/mode` flips to `canvas` and `usedCanvasFallback=true` so dashboards can alert on fidelity drops.  
  - `HtmlTargetDesc::prefer_dom` selects DOM as the first-choice fidelity even if Canvas would also succeed; set `false` when you want Canvas replay for deterministic testing.  
  - `HtmlTargetDesc::allow_canvas_fallback` controls whether the adapter is allowed to emit Canvas output at all—disabling it forces an error when DOM limits are exceeded, useful for aggressively surfacing missing shims.
- Asset hydration  
  - `Html::Adapter::emit` records every referenced asset under `output/v1/html/assets`. Renderer::RenderHtml now resolves those logical paths against the latched scene revision (`scenes/<sid>/builds/<rev>/assets/...`) so the final payload carries real bytes and accurate MIME types (PNG, WOFF2, etc.) instead of placeholder `application/vnd.pathspace.*` stubs. Consumers can stream assets directly from the PathSpace tree without touching scene snapshots.
  - Asset payloads are serialized with the `HSAT` (Html Surface Asset Table) framing: 4-byte magic (`0x48534154`), 2-byte version (`0x0001`), 4-byte little-endian asset count, followed by per-asset triplets of `[logical_path_length, mime_length, bytes_length]` (each `uint32_le`) and the corresponding UTF-8 strings plus raw byte payload. Writers clamp lengths to ≤4 GiB; readers must treat all integers as little-endian regardless of host endianness.
  - Legacy payloads written via the pre-October 2025 Alpaca serializer are no longer accepted. All render paths must emit HSAT; if a deployment still has Alpaca-encoded blobs, run one render on a build prior to October 20, 2025 to rewrite them or script an out-of-band migration before rolling this version.
- Debug workflow  
  - Inspect `output/v1/html/{mode,commandCount,domNodeCount,usedCanvasFallback}` plus the per-target diagnostics to understand why a fallback triggered.  
  - Run `ctest -R HtmlCanvasVerify` (or invoke `scripts/verify_html_canvas.js <build/bin/html_canvas_dump>`) to check both DOM and Canvas outputs locally; the script mirrors the CI harness and skips automatically when Node.js is missing.  
  - When assets go missing, verify the revision-mounted `assets/` subtree (e.g., `scenes/<sid>/builds/<rev>/assets/images/<fingerprint>.png`) and confirm the adapter’s `assets` array includes populated `bytes` fields.

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
  - `PathSurfaceMetal` allocates/owns an offscreen `MTLTexture` (IOSurface-backed when `iosurface_backing=true`) and tracks frame/revision indices. Builders can already create Metal targets; by default they replay the software raster output onto the Metal surface, with optional GPU uploads gated on the environment variable `PATHSPACE_ENABLE_METAL_UPLOADS=1`.
   - Presenter draws textured quad into `CAMetalLayer` drawable on the UI thread.
   - **Integration roadmap (Oct 18, 2025; updated Oct 19):**
     1. Persist `RendererKind::Metal2D` in renderer metadata; resolve backend in builder context (landed).
     2. Introduce metal surface cache & render flow: PathRenderer2D dispatches Metal path (command buffer encode, minimal pipeline) when backend == Metal2D. Until the GPU encoder is complete we reuse the software raster and optionally upload into the Metal texture when `PATHSPACE_ENABLE_METAL_UPLOADS` is set.
     3. Extend `PathWindowView::Present` / macOS presenter to accept Metal surface handles and present CAMetalLayer drawables without IOSurface copies.
     4. Record Metal timings/errors in `output/v1/common` + diagnostics mirrors once GPU rendering is active.
     5. Provide ObjC++ harness/CI coverage with Metal enabled; keep the environment variable unset in CI so tests continue to run purely in software.

## Open questions

- Text shaping and bidi strategy; font fallback
- Color management (sRGB, HDR) across software/GPU paths
- Direct-to-window bypass for single-view performance-critical cases (optional surface mode)

## Decision: Present policy

Summary:
- Applications choose between always-fresh frames, bounded staleness reuse, or explicit/manual presentation. Defaults live at the app root, with optional overrides per window/view when a surface needs different latency or power trade-offs.
Approach:
- Publish `present_policy.json` under `applications/<app>/windows/` capturing the default policy (`always_fresh`, `allow_stale`, or `manual`) and, for `allow_stale`, thresholds (`max_age_ms`, `max_revision_gap`) that bound reuse; allow overrides at `windows/<id>/views/<view>/present_policy`.
- Extend the typed path helpers so `PathWindowView` resolves the effective policy by walking `view → window → app` scopes, caching the result, and listening for config churn via the existing notification channels.
- Teach `PathSurfaceSoftware::present()` (and future GPU surface implementations) to honor the policy: force a synchronous `render` for `always_fresh`; reuse the last frame for `allow_stale` while the output’s `frameIndex`/`revision` stay within thresholds; require an explicit trigger when `manual` is in effect.
- Emit presentation metrics (`reused_frames`, `forced_renders`, `stale_age_ms`) under `windows/<id>/views/<view>/metrics/present_policy` so tooling can tune thresholds, spot regressions, and keep behavior consistent across backends.

## Decision: Snapshot retention

- Maintain a per-scene `SnapshotRegistry` that tracks revision metadata (`ref_count`, `last_used_ms`, `size_bytes`). Renderers bump the ref count when adopting a revision and release when the frame completes; background jobs (capture, metrics) use the same API so the GC sees every consumer.
- Retain the newest three revisions at all times and pin any revision with a non-zero `ref_count`. When a revision with `ref_count == 0` is both older than two minutes and not among the newest three, evict it immediately.
- Run a lightweight GC task (`SceneSnapshotGcTask`) every 250 ms. The task walks each `SnapshotRegistry`, applies the retention rules, and records aggregate counters (`retained`, `evicted`, `bytes_alive`) under `scenes/<id>/metrics/snapshots`.
- If eviction pressure persists (e.g., disk or memory budget exceeded), emit a `snapshot_evicted` event and annotate the active renderer target via `output/v1/common/lastError` so tooling can inform authors that accumulation restarted.
- Enforce a hard cap (configurable; default 12 revisions) to prevent unbounded growth during bursty edits. When about to exceed the cap, evict the oldest zero-ref revision regardless of age; if none qualify, block new snapshot publication until a consumer releases or a configurable timeout triggers an error.
- Document the policy in `docs/AI_ARCHITECTURE.md` (rendering section) and expose metrics in telemetry so operators can tune thresholds per app.

## Decision: HTML adapter fidelity (resolved)

Summary:
- Adopt DOM/CSS as the default v1 adapter for preview/export of 2D UI scenes.
- Provide an automatic fallback to a compact Canvas JSON + tiny runtime when DOM node budgets or fidelity constraints are exceeded.
- Defer WebGL to a later phase; only consider if performance or blend/clip fidelity requires it.

Scope and constraints (v1):
- Supported: rect/rrect, images, text, 2D transforms, z-order, rect/rrect clipping, shadows; sRGB color and premultiplied alpha semantics approximated via CSS.
- Out of scope: 3D transforms, advanced blend modes beyond normal alpha, non-rectangular hit regions, Display P3 targets, GPU lighting.

Output paths:
- output/v1/html/dom — full HTML document (may inline CSS/JS)
- output/v1/html/css — optional CSS string
- output/v1/html/commands — Canvas JSON fallback command stream
- output/v1/html/assets/* — assets referenced by DOM/CSS or Canvas JSON

Fidelity tiers and selection:
- Tier 1: DOM/CSS (default)
  - Semantic elements and CSS transforms; overflow/clip-path for safe cases.
  - Deterministic stacking via z-index and DOM order; stable ids for tie-breaks.
- Tier 2: Canvas JSON (fallback)
  - Emit a compact command stream (moveTo/lineTo/arc/clip/drawImage/drawGlyph) and include a minimal replay runtime (<10 KB) in the DOM or as an asset.
  - Switch when:
    - Estimated DOM node count exceeds maxDomNodes (default 10k).
    - Scene uses clip/blend features that DOM/CSS cannot match acceptably.
- Tier 3: WebGL (deferred)
  - Not implemented in v1; revisit only if necessary.

Text shaping strategy (v1):
- Primary: consume pre-shaped glyph runs from the snapshot builder (HarfBuzz + FreeType, per existing decision).
- DOM/CSS mode:
  - Emit absolutely positioned glyph spans (one per glyph for complex scripts; coalesce safe clusters), with CSS transforms for subpixel placement.
  - Optional simplified mode for trivial Latin runs: native DOM text to reduce node count.
- Canvas JSON mode:
  - Replay positioned glyphs; MSDF atlas path may be added later but is not required for v1.
- Fonts:
  - Emit @font-face rules referencing output/v1/html/assets/fonts/*; optional later subsetting to WOFF2.

Mapping:
- Rect/rrect → div + border-radius (fallback to clip-path for non-uniform radii as needed).
- Paths more complex than rrect → Canvas JSON fallback; DOM clip-path used only for well-supported, simple shapes.
- Images → img with explicit width/height; use CSS object-fit when needed.
- Transforms → CSS matrix() composed in the snapshot order.
- Stacking/blending → z-index and element opacity; advanced blend modes defer to Canvas JSON.
- Clipping → overflow:hidden for rect/rrect; complex clips use clip-path when safe, otherwise Canvas JSON.
- Color → sRGB CSS colors; assume sRGB target.

Implementation:
- HtmlAdapter::emitDomCss(revision, options) → domString, cssString, assets[]
- Options: { preferDom=true, maxDomNodes=10000, allowClipPath=true, preferGlyphSpans=true }
- Asset pipeline: copy referenced images/fonts to output/v1/html/assets/*; generate @font-face.
- Canvas JSON: encoder walking snapshot draw order; tiny JS runtime embedded or referenced from assets.

Tests and metrics:
- Golden screenshot tests with headless Chromium and Firefox comparing DOM/CSS and Canvas JSON to software renderer goldens for rects/rrects/images/text.
- Text: Latin kerning/ligatures, Arabic joining+bidi, Devanagari reordering, CJK; verify layout stability for glyph-span mode.
- Size/perf: DOM node count, total output size, initial render time; validate tier switch thresholds.
- Path and containment: verify output paths populated and app-relative constraints enforced.
- Fallback harness (October 23, 2025): renderer metrics now expose `textPipeline`, `textFallbackAllowed`, and `textFallbackCount`; debug flags (`RenderSettings::Debug::kForceShapedText`, `kDisableTextFallback`) and env controls (`PATHSPACE_TEXT_PIPELINE`, `PATHSPACE_DISABLE_TEXT_FALLBACK`) let tests force shaped vs glyph-quad paths so the legacy glyph quads renderer remains a supported regression harness.

Milestones:
- M1 DOM/CSS MVP: rect/rrect/images/basic Latin text, transforms, z-index, rect clipping, assets/@font-face, goldens on two browsers.
- M2 Shaped text and safe clip-path: full shaped scripts via glyph spans; fallback selection logic.
- M3 Canvas JSON fallback: encoder + runtime; fidelity tests; thresholds.
- M4 Optimizations: text run coalescing; optional font subsetting; doc polish.

Cross-references:
- Update HTML/Web output section to mention tiers and selection logic.
- Reflect adapter behavior in docs/AI_ARCHITECTURE.md.

## Decision: Lighting and shadows (resolved)

Summary:
- Lighting will ultimately rely on ray queries against the authoritative surface representation with a progressive pixel cache. The earlier “microtriangle tessellation” concept has been superseded by this approach.
- Detailed rollout, cache mechanics, and open questions live in `docs/surface_ray_cache_plan.md`; keep this section and that plan in sync as decisions evolve.
- Implementation sequencing: finish the baseline pixel presenter (simple “set pixel color” per drawable) before layering in the ray cache and RT lighting work. The plan document acts as the blueprint once we reach that phase.

Guiding principles:
- All drawables—even traditional UI widgets—are authored and rendered as true 3D assets; no 2.5D exceptions or special UI lighting paths.
- Area lights originate directly from emissive geometry: tetrahedral mesh faces and signed-distance field surfaces flagged with emission properties. The snapshot builder publishes immutable emitter records (radiance, PDFs, orientation) alongside TLAS/BLAS/SDF acceleration data so renderers can importance-sample in lockstep with geometry updates.
- The progressive path tracer is the authoritative lighting solution. GPU execution uses portable compute or ray-tracing pipelines (Metal, Vulkan, etc.) while the CPU backend shares the same sampling core; CUDA-specific code is intentionally avoided.
- Shadows emerge naturally from the ray-traced solution; no raster fallbacks or analytic shadow heuristics are maintained.
- Accumulation state (sample count, variance, backend flag) is surfaced under each target’s `output/v1/...` subtree so presenters and tooling can tonemap partial results and respond to edits by only resetting affected regions.

Model (v1):
- RenderSettings.MicrotriRT:
  - enabled: bool
  - microtriEdgePx: float (target edge length in pixels; default ≈ 1.0)
  - maxMicrotrisPerFrame: uint (tessellation budget)
  - raysPerVertex: uint (spp for irradiance integration)
  - maxBounces: uint (≥ 1; commonly 1–3)
  - rrStartBounce: uint (≤ maxBounces)
  - useHardwareRT: enum { Auto, ForceOn, ForceOff }
  - environment: { hdrPath: string, intensity: float, rotation: float }
  - allowCaustics: bool
  - clampDirect?: float
  - clampIndirect?: float
  - progressiveAccumulation: bool
  - vertexAccumHalfLife: float (temporal accumulation weight, e.g., 0.1–0.5)
  - seed: uint64
- Materials/BSDF (per draw node or mesh subset):
  - PBR: baseColor, metalness, roughness (GGX), ior, transmission, emissive, normalMap
- Geometry:
  - UI geometry: primarily SDF-defined primitives (buttons, window chrome, etc.) are converted to triangle meshes via isosurface contouring during snapshot build. We use adaptive marching on implicit fields to produce well-conditioned triangles that target ≈ microtriEdgePx after subsequent screen-space tessellation. Each generated vertex receives a stable vertexId derived from (sourceSdfId, cellCoord, isoEdgeIndex).
  - Non-UI geometry: represented as tetrahedral meshes by default. Surface triangles for raster visibility are extracted from the tet boundary; the full tet mesh is used by the GPU RT integrator for robust interior traversal and media transitions (consistent with the tetrahedral acceleration section).
  - The snapshot builder emits the per-view microtriangle buffers from these sources with stable vertexIds enabling temporal accumulation and reuse.
  - **Canonical form:** regardless of authoring primitive (SDF, mesh, analytic surface), we convert to tetrahedral volume representations before any ray queries so the renderer, cache, and RT kernels operate on a unified intersection kernel.

Approach:
- Microtri tessellation (CPU):
  - Adaptively tessellate visible primitives to ≈ microtriEdgePx in screen space; write per-vertex worldPos, normal, uv, materialId.
  - Generate stable vertexIds: hash(sourcePrimitiveId, gridU, gridV, lodSeed) to support temporal accumulation and buffer reuse.
  - Produce an index/vertex buffer for software raster and a GPU-visible vertex buffer for RT.
- Software raster (visibility and composition):
  - Rasterize microtris in tiles; resolve visibility, clipping, stacking contexts, and blending as usual.
  - Shading reads per-vertex irradiance from the lighting buffer and interpolates to pixels; baseColor and alpha come from materials/textures. Optionally add a lightweight view-dependent specular term from roughness.
- GPU ray tracing (per-vertex irradiance):
  - Build/reuse BLAS/TLAS over the full scene on hardware RT; if ForceOff, an optional compute BVH fallback may be used.
  - Dispatch a ray-gen kernel that, for each unique vertexId in the microtri mesh, traces rays to integrate incoming radiance (direct via NEE + BSDF sampling, indirect via path continuation). Write irradiance (rgb) into the vertex lighting buffer at vertexId.
  - MIS for area lights and environment; Fresnel-aware BSDF sampling; Russian roulette after rrStartBounce. Optionally output moments/variance for adaptive temporal filtering.
- Synchronization and data flow:
  - Double-buffer the vertex lighting buffer: while raster reads buffer A, GPU RT writes buffer B; swap atomically at frame end. Latch RenderSettings and snapshot revision at frame start.
- Temporal accumulation and denoise:
  - If progressiveAccumulation, accumulate per-vertex irradiance across frames using vertexId and motion-aware reprojection where available. Clamp variance to reduce ghosting.
- Budgets and fallbacks:
  - Enforce maxMicrotrisPerFrame and raysPerVertex; adapt tessellation density and/or spp when over budget. If useHardwareRT=Auto and unavailable, either skip RT lighting (fallback to ambient/baseColor) or use the compute fallback if enabled.

Data and API changes:
- RenderSettings:
  - Add MicrotriRT struct as above.
- Snapshot builder:
  - Add microtriangle tessellator; emit per-view microtri vertex/index buffers with stable vertexIds.
- Renderer outputs:
  - output/v1/common/: add { vertexLightingEpoch, microtriCount }
  - output/v1/buffers/: optional debug dumps of microtri meshes and lighting buffers (debug builds)
- Scene schema:
  - Add declarative SDF node types for UI (rect/rrect, text SDF, path SDF, composition ops) with parameters; builder performs isosurface contouring to triangles.
  - Support tetrahedral mesh nodes for non-UI content with material regions; surface extraction is derived at build time.
  - Mesh nodes remain supported; PBR materials apply uniformly across SDF-derived, tet-derived, and explicit meshes.

Atomicity and threading:
- RT lighting is computed per frame with double-buffered publish; raster always consumes a complete, immutable lighting buffer for that frame.

Culling and bounds:
- CPU frustum cull source primitives pre-tessellation; microtri meshes are view-local and not persisted between frames.

Validation and safety:
- Validate rrStartBounce ≤ maxBounces; enforce budgets; ensure BSDF parameters are energy-conserving.

Performance targets:
- 1080p: ≈1px microtris with 1–2 rays/vertex on modern RT hardware within present budgets; progressive accumulation improves quality over time.

Tests:
- Microtri density and budget control; fixed-seed determinism for per-vertex lighting; clean fallback when hardware RT is unavailable; concurrency loop=15 remains green.

Docs and examples:
- Add “Hybrid Microtri + RT” to docs/AI_ARCHITECTURE.md with dataflow and buffer lifetimes; provide sample scenes showing area lights, glossy/refraction, and environment lighting.

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
- Delegate retention to the policy defined in “Decision: Snapshot retention”; GC skips revisions with outstanding references.

Notes:
- Renderer behavior is unchanged; it consumes the latest revision agnostic to patch vs rebuild
- If any core semantics change, reflect them in `docs/AI_ARCHITECTURE.md` and update tests accordingly

## Decision: Direct-to-Window targets (resolved)

Summary:
- Add a Direct-to-Window target mode for single-view performance-critical cases to bypass the offscreen surface → present blit and reduce latency.

Semantics:
- Target kind: introduce a new target kind windows for direct-to-window rendering.
- Single consumer: one view binds to a windows target at a time; binding a second view is invalid.
- Sizing and reconfigure: size derives from the window’s drawable/swapchain; window resizes trigger reconfigure and update desc/active.
- Present policy: buffered presentation only (AlwaysLatestComplete or PreferLatestCompleteWithBudget). AlwaysFresh is allowed only if backend can wait without blocking the UI thread.
- Adoption and threading: renderer latches current_revision per frame; present occurs on the platform’s UI/present thread.

Descriptors:
- WindowBackbufferDesc:
  - swapchain: { min_images:uint, present_mode: Fifo | Mailbox | Immediate }
  - pixel_format (enum), color_space (enum)
- desc/active mirrors chosen swapchain parameters (image count, size, format, color_space).

GPU specifics:
- Metal: render directly into CAMetalDrawable.texture and present via presentDrawable on the UI thread.
- Vulkan: own swapchain; render into acquired image; handle VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR by recreating the swapchain.

Status and errors:
- status/* includes device_lost, drawable_unavailable, suboptimal_swapchain, and message.
- On invalid multi-bind, set an error status and write output/v1/common/lastError.

Tests and metrics:
- Verify desc/active reflects window changes; device-lost/suboptimal recovery; skip present when drawable unavailable.
- Latency microbenchmarks comparing windows targets vs offscreen+blit.

Cross-reference:
- Update Target keys to include windows.
- Document backend nuances under GPU backend architecture.

## Decision: Present Policy (resolved)

Summary:
- Presenters use a backend-aware policy to choose between waiting for a fresh frame and showing the latest completed output. The HTML adapter ignores policy (always presents latest complete).
- Modes:
  - AlwaysFresh — wait up to a tight deadline for a new frame; otherwise skip present (don’t show stale).
  - PreferLatestCompleteWithBudget (default) — wait within a small staleness budget; else present last-complete.
  - AlwaysLatestComplete — never wait; present whatever is complete now.
- Software renderer supports an optional Progressive Present mode for low latency (shared framebuffer with tile seqlock and partial blits). GPU paths align waits to vsync and prefer reusing the last-complete texture over blocking the UI thread.
- Configuration is per view: windows/<win>/views/<view>/present/{policy, params}; Builders may provide per-call overrides.
- Metrics: output/v1/common includes presentedRevision, presentedAgeMs, presentedMode, stale, waitMs; GPU may add skippedPresent and drawableUnavailable.

Cross-reference:
- Full semantics, parameters, and progressive mode details are specified in docs/AI_ARCHITECTURE.md under:
  - “UI/Rendering — Present Policy (backend-aware)”
  - “Software Renderer — Progressive Present (non-buffered)”

## Decision: Text shaping, bidi, and font fallback (resolved)

Summary:
- Standardize on HarfBuzz + FreeType for Unicode shaping and font loading on Windows, macOS, iOS, Android, and Linux.
- Use MSDF atlases for medium/large/zooming text and stylized effects; use per-size bitmap atlases (FreeType raster) for very small text requiring hinting/LCD subpixel AA (desktop).
- Build glyphs on demand with a non-blocking glyph cache and dynamic atlas; re-render regions as missing glyphs land.

Approach:
- Shaping:
  - HarfBuzz shapes runs with script, direction (UAX#9 bidi), language, and OpenType features; outputs glyph ids, advances, offsets, clusters.
  - Cache shaped runs keyed by `{fontFace+features+script+direction+text}`; invalidate on style/font change.
- Glyph atlases:
  - MSDF generation from FreeType outlines (msdfgen or equivalent). Choose `pxRange` 8–12 (UI) or 12–16 (heavy zoom); padding ≥ ceil(pxRange)+2 texels.
  - Separate pages by `{fontFace, pxRange}`; pack with skyline/guillotine; generate mipmaps. Evict via LRU; persist pages on disk keyed by `{font hash, face index, style, pxRange, tool versions}`.
  - Small text threshold (~16–20 px): switch to bitmap atlases (optional LCD subpixel on desktop).
- Miss handling:
  - During draw, render present glyphs; enqueue missing glyphs to a worker queue (outline → MSDF → pack → subresource upload). Mark affected runs/regions dirty and re-render next frame.
  - Optional temporary grayscale bitmap fallback while MSDF bakes for user-visible inputs.
- Fallback strategy:
  - Accept CSS-like font family lists; resolve fallback per script. Ship a curated Noto set for coverage; optionally consult platform registries for system fallback.
  - Include variable font axis tuples in cache keys when used; support COLR/CPAL/emoji via bitmap path (not MSDF).
- Cross-platform and build:
  - Dependencies: HarfBuzz (shaping), FreeType (outlines/raster), optional ICU (line breaking/script/lang hints).
  - CMake flags (suggested): `PATHSPACE_TEXT_HARFBUZZ`, `PATHSPACE_TEXT_FREETYPE`, `PATHSPACE_TEXT_ICU` (optional). On Apple, CoreText may be used for discovery only.
- Rendering:
  - TextGlyphs commands carry per-glyph UVs, page id, plane bounds, `pxRange`, and placement; shader decodes MSDF (median RGB) with `fwidth` for crisp edges, supports outline/glow via threshold offsets.

Tests and metrics:
- Golden tests for Latin (kern/lig), Arabic (joining+bidi), Devanagari (reordering), CJK; include fallback spans and variable font axes.
- Metrics: shapedRunCache hit/miss, atlas hit/miss, bake/upload latency, evictions, LCD downgrade counts.

Cross-references:
- Update `docs/AI_ARCHITECTURE.md` to document `TextShaper`/atlas abstractions and builder style fields (`font_family`, `font_size_px`, `font_features`, `lang`, `direction`, `wrap_mode`).
- Keep snapshot schema stable; `TextGlyphs` payload includes atlas and `pxRange` metadata.

## Decision: Entity Residency and Snapshot Storage Policy (resolved)

Summary:
- Entities are renderer-facing, renderable projections of authoritative domain objects that live elsewhere (e.g., physics/simulation). The renderer does not own or serve data back to other systems.
- Each resource (geometry/material/texture/etc.) referenced by an Entity has a configurable residency/storage policy expressed as standard PathSpace values. The backend (RAM, shared memory, filesystem, GPU) is selected internally per item based on policy and platform.
- Snapshot publish/adopt semantics remain uniform: builders publish a complete subtree under `scenes/<sid>/builds/<rev>` and atomically update `scenes/<sid>/current_revision`; renderers latch `current_revision` per frame and read immutable values only.

Per-item residency policy (values under authoring/resources):
- Keys (examples):
  - `scenes/<sid>/src/resources/<rid>/policy/residency/allowed = ["gpu","ram","disk"]`
  - `scenes/<sid>/src/resources/<rid>/policy/residency/preferred = ["gpu","ram"]`
  - `scenes/<sid>/src/resources/<rid>/policy/residency/durability = "ephemeral|cacheable|durable"`
  - `scenes/<sid>/src/resources/<rid>/policy/residency/max_bytes = <uint64>`
  - `scenes/<sid>/src/resources/<rid>/policy/residency/cache_priority = <uint32>`
  - `scenes/<sid>/src/resources/<rid>/policy/residency/eviction_group = "textures|geometry|materials|other"`
- Semantics:
  - allowed/preferred constrain and order backend selection; durability controls restart survivability (non-ephemeral items are reloadable after a renderer crash/restart).
  - max_bytes guides admission/eviction; cache_priority and eviction_group control pressure handling within watermarks.

Storage backends (opaque to callers; exposed as normal PathSpace values):
- RAM: process-local; fastest; not crash-safe; selected only when allowed and durability permits.
- SHM (shared memory): cross-process crash-safe; avoids disk IO; size-limited; selected when durability requires and within thresholds.
- Filesystem: staging + fsync + atomic exposure behind PathSpace; chosen for large or durable items and when SHM is unavailable.
- GPU: device-local residency for draw-time; backed by RAM/SHM/FS per policy for reload; eviction respects watermarks.

Authoring concurrency (proposed vs resolved):
- Producers write to `scenes/<sid>/src/objects/<oid>/proposed/<source>/<component>/*` (e.g., `physics`, `script`).
- A coalescer resolves to `scenes/<sid>/src/objects/<oid>/resolved/<component>/*` using a per-component policy (ownership, priority, or merge). Resolved components carry epochs for MVCC.
- The snapshot builder latches a consistent `resolved/*` view (by epochs) and emits `scenes/<sid>/builds/<rev>/entities/*` and `.../resources/*`. Mid-build changes affect the next revision.

Atomicity and adoption:
- Builders write under `builds/<rev>.staging/*` then atomically expose as `builds/<rev>` via the runtime’s atomic subtree swap/alias (or FS rename internally). Finally, they atomically set `current_revision = <rev>`.
- Renderers latch `current_revision` at frame start; no mid-frame reads; outputs are written under `targets/<tid>/output/v1/*`.

Crash-restart:
- Items with durability ≠ `ephemeral` are re-openable by a new renderer process using only standard PathSpace reads; the runtime maps the correct backend (SHM/FS) internally.

Watermarks and GC:
- Residency honors the existing “Renderer cache watermarks (resolved)” for CPU/GPU caches.
- Snapshot retention and safety continue to follow “Snapshot GC and Leases (resolved)”.

Tests:
- Backend-agnostic publish/adopt remains atomic; no partial visibility of `.staging`.
- Crash-restart rehydrates durable resources; ephemeral items are repopulated by producers.
- Residency decisions respect allowed/preferred and watermarks; eviction degrades gracefully (e.g., proxy textures).

Cross-reference:
- Update docs/AI_ARCHITECTURE.md to reflect Entity residency keys and the proposed/resolved authoring model. Keep existing path shapes and invariants intact.

## Decision: Snapshot GC and Leases (resolved)

Summary:
- Automate cleanup of scene snapshot revisions while guaranteeing no renderer reads a deleted revision.
- Use per-target short-ttl leases; GC respects leases plus K/T retention thresholds; publish/adopt remain atomic.

Protocol:
- Publish:
  - Builder writes to `scenes/<sid>/builds/<rev>.staging/*`, fsyncs, atomically renames to `scenes/<sid>/builds/<rev>`, then atomically updates `scenes/<sid>/current_revision` to `<rev>`.
- Adopt (per frame):
  - Renderer latches `current_revision = R` once at frame start and reads exclusively from `scenes/<sid>/builds/<R>/...` for the entire frame.
  - Renderer creates or refreshes a lease under `scenes/<sid>/leases/<rendererId>/<targetId>` with `{ rev: R, expires_at_ms: now + ttl_ms, epoch: inc }`.
  - On shutdown it deletes its lease; on crash, the lease naturally expires (ttl).

Retention policy (defaults):
- Retain by count: keep last K revisions (default K = 3).
- Retain by time: keep revisions newer than T minutes (default T = 2m).
- Lease safety: never delete `current_revision` or any revision referenced by a non-expired lease.
- Always retain at least one revision even if rules would allow more deletion.

Lease keys and timing:
- Path: `scenes/<sid>/leases/<rendererId>/<targetId>`
- Value: `{ rev:uint64, expires_at_ms:uint64 (monotonic), epoch:uint64 }`
- Recommended defaults:
  - `lease_ttl_ms = 3000` (must exceed max frame/present latency)
  - `min_refresh_ms = 250` (coalesce lease writes; also refresh when `rev` changes)

GC algorithm (idempotent, crash-safe):
- Enumerate `scenes/<sid>/builds/<rev>` (uint64 increasing).
- Determine active leases where `expires_at_ms > now_ms`; compute `minActiveLeaseRev` if any.
- Compute cutoff by count/time; effective cutoff = min(countCutoff, timeCutoff, minActiveLeaseRev if present).
- For each rev older than cutoff:
  - Skip `current_revision`, any rev in active lease set, and any `*.staging`.
  - Atomically rename `builds/<rev>` → `builds/<rev>.todelete`, then recursively delete `*.todelete` (retry-safe).
- Never touch the currently publishing `.staging` or the latest live revision.

Observability (optional but recommended):
- `scenes/<sid>/gc/lastRunMs: double`
- `scenes/<sid>/gc/reclaimedCount: uint32`, `scenes/<sid>/gc/retainedCount: uint32`
- `scenes/<sid>/gc/reason: string` (e.g., "count", "time", "lease")
- Per-revision metadata (optional): `scenes/<sid>/builds/<rev>/.meta { created_at_ms:uint64, size_bytes:uint64 }`

Configuration (per scene):
- `scenes/<sid>/settings/gc { keep_last:int=3, keep_ms:int=120000, lease_ttl_ms:int=3000, min_refresh_ms:int=250 }`

Invariants:
- Builders publish via `.staging` → live rename, then update `current_revision` atomically.
- Renderers read only from the latched revision for a frame; adoption is lock-free post-latch.
- GC never deletes `current_revision` or revisions protected by non-expired leases.

Test considerations:
- Concurrency: rapid publish while multiple renderers render; assert no deleted-read.
- Crash: set lease, crash renderer; ensure GC reclaims only after lease expiry.
- Churn: high-frequency revisions; validate K/T retention and never deleting `current_revision`.

## Decision: IO logging and stdout/stderr mirrors (resolved)

Summary:
- Each application writes errors and other levels to authoritative per-app log paths under `<app>/io/log/*`; read-only mirrors forward to per-app stdout/stderr and then to global system streams. All logs are bounded rings.

Authoritative per-app logs
- `<app>/io/log/error` — the only write target for error text (UTF-8, newline-delimited)
- `<app>/io/log/info`, `<app>/io/log/debug` (optional), etc.
- Apps write only to `<app>/io/log/*`; mirrors are derived.

Mirrors and aggregation
- Local mirrors (read-only to apps):
  - `<app>/io/stderr` — tails `<app>/io/log/error` and appends the same bytes
  - `<app>/io/stdout` — tails `<app>/io/log/info` (and optionally debug) and appends the same bytes
- Global mirrors (system-owned):
  - `/system/io/stderr` — tails every `<app>/io/stderr`; prefixes provenance `[app=<app-path>]`
  - `/system/io/stdout` — tails every `<app>/io/stdout`; prefixes provenance
- Forwarders maintain durable offsets; if they lag beyond eviction, they fast-forward and increment a loss counter.

Semantics
- Encoding: UTF-8 only; replace invalid sequences with U+FFFD.
- Framing: newline-delimited lines; CRLF normalized to LF; partial lines buffered until LF.
- Non-blocking: writers never block on mirrors; forwarding is best-effort.
- Level routing: INFO/DEBUG → stdout; WARN/ERROR/FATAL → stderr.

Bounded retention (caps)
- Every log place is a bounded ring with at least:
  - `max_messages`: hard cap on retained complete lines
  - Optional: `max_bytes` and `max_age_ms`
- Eviction policy: drop oldest whole messages to satisfy caps; track `dropped_oldest_total`.
- Status/metrics per path:
  - `status/current_messages`, `status/max_messages`, `status/committed_messages_total`,
    `status/dropped_oldest_total`, `status/lag_bytes` (mirrors), `status/lost_to_forwarder_total`,
    `status/last_eviction_ms`
- Recommended defaults (tunable per deployment):
  - `<app>/io/log/error`: 10k; `<app>/io/log/info`: 20k; `<app>/io/stderr`: 20k; `/system/io/stderr`: 1M.

ACLs and safety
- Writes:
  - App → `<app>/io/log/*` only
  - Local tee → `<app>/io/std{out,err}`
  - System aggregator → `/system/io/std{out,err}`
- Readers: app devs/tools read app-local logs; ops/admin tools read system logs.
- Loop prevention: local tee reads only from `<app>/io/log/*`; aggregator reads only from `<app>/io/std{out,err}`.

Interop with renderer observability
- When a renderer updates `output/v1/common/lastError` (or trace), emit a concise one-line to `<app>/io/log/error` (once per change) with a corr tag `(rendererId:targetId:frameIndex)` for correlation.
- Keep structured, per-target metrics under `renderers/<rid>/targets/.../output/v1/*`; the `io/*` streams remain text-only.

Failure behavior
- If a mirror or aggregator is down, authoritative logs continue; upon restart, forwarders resume from durable offsets or fast-forward if necessary (with loss accounting).

## Decision: Resource system (resolved)

Summary:
- Canonical app asset namespace with deterministic references in snapshots.
- Snapshot-local asset table keyed by content digest (sha256); renderer caches by digest across targets.
- Async load/decode with fallback behavior; eviction via LRU with pins for in-use revisions.

App asset layout (under the app root):
- Images: `<app>/assets/images/<name>/{data,meta.json}`
- Fonts: `<app>/assets/fonts/<family>/<style>/{file,meta.json}`
- Shaders: `<app>/assets/shaders/<name>/{msl|spirv,meta.json}`

meta.json (per asset):
- Common: `{ digest_sha256, byte_size, created_at, updated_at }`
- Images: `{ color_space: "sRGB|DisplayP3|Linear", premultiplied: bool, icc_embedded: bool, width, height, format }`
- Fonts: `{ family, style, weight, stretch, format: "ttf|otf|woff2", index? }`
- Shaders: `{ stage, language, compiler, defines[] }`

Snapshot integration (per revision):
- `scenes/<sid>/builds/<rev>/assets/index.json` lists assets used by the snapshot:
  - `[{ kind: "image|font|shader", logical: "images/<name>" | "fonts/<family>/<style>" | "shaders/<name>", digest_sha256, local_id }]`
- Drawable/material payloads reference `local_id` (not paths) for stability and compactness.
- Snapshots do not embed raw asset bytes; renderers resolve `digest_sha256` via caches.

Renderer cache behavior:
- Key: `digest_sha256` (content-addressed).
- Async load/decode off-thread on first use; pin assets referenced by the latched revision until frame end.
- Eviction: LRU with soft/hard watermarks; separate pools for CPU (software) and GPU.
- Fallbacks on miss:
  - Images: placeholder draw + single error message per target/revision in `output/v1/common/lastError`.
  - Fonts: configured fallback family/style; mark degraded text via debug flags.

Authoring references and helpers:
- Nodes:
  - Image: `.src = "assets/images/<name>"`
  - Text: `.font = { family, style, weight }` resolved to `assets/fonts/...`
- Builders.hpp additions:
  - `upload_image(...) -> "assets/images/<name>"`
  - `register_font(...) -> "assets/fonts/<family>/<style>"`

Change detection and incremental rebuilds:
- Writers update `{data,meta.json}` atomically and bump a small `assets/index` counter.
- Snapshot builder watches `assets/*`; on change:
  - Mark affected nodes `contentEpoch++`.
  - Invalidate text shaping cache entries keyed by `{font+features+script+dir+text}` when fonts change.
  - Rebuild only impacted draw payloads; publish a new revision.

Color pipeline integration:
- Respect `meta.color_space` and `premultiplied`.
- Decode to linear working space or sample from sRGB textures with automatic decode.
- Set `pipelineFlags` appropriately (`SrgbFramebuffer`, `UnpremultipliedSrc` conversion when required).

Failure modes and observability:
- Missing asset path during build: emit placeholder entry and authoring error; renderer surfaces concise status in `output/v1/common/lastError`.
- Decode failure: cache records digest→error; avoid repeated work until files change.

Metrics (debug):
- `assetsLoaded`, `assetsPending`, `assetBytesResident`, `assetEvictions`.

Minimal tests:
- sRGB vs Linear PNG render goldens (color correctness).
- Font registration + mixed-script shaping; verify cache invalidation after font swap.
- Live image asset change triggers partial snapshot rebuild without tearing.

## Decision: Renderer cache watermarks (resolved)

Summary:
- Bound renderer memory with soft/hard watermarks for CPU- and GPU-resident caches. Evict cold items predictably to avoid OOM and stalls, while pinning assets referenced by the latched revision for frame safety.

Scope:
- CPU cache: decoded images, procedurally generated outputs, shaped text/MSDF atlas pages, prepared geometry blobs.
- GPU cache: textures/atlas pages, vertex/index/uniform buffers, optional intermediates. Pipeline/descriptor caches are small and not watermark-managed in v1.

Configuration (per renderer):
- Path: `renderers/<rid>/settings/cache`
  - `cpu_soft_bytes: uint64`
  - `cpu_hard_bytes: uint64`
  - `gpu_soft_bytes: uint64`
  - `gpu_hard_bytes: uint64`
  - Optional per-kind caps (v1 optional): `text_atlas_soft_bytes`, `image_soft_bytes`, etc.

Semantics:
- Soft watermark: when resident_bytes > soft, begin background eviction (LRU or size-aware LRU) until below soft.
- Hard watermark: if an allocation/upload would exceed hard, perform synchronous eviction; if still over, deny/skip the allocation and surface a concise error. Placeholders are allowed to render.
- Pinning: assets referenced by the currently latched `revision` are pinned until frame end and cannot be evicted; optionally pin a small MRU window to reduce churn.
- Eviction policy: maintain per-kind LRU lists to prevent large images from evicting text infrastructure first; prefer evicting largest-cold items. Never evict pinned or in-flight items.
- Miss handling: CPU miss triggers async decode/generation; GPU miss uploads when CPU bytes become ready. Render placeholders until assets land.

Defaults (starting points):
- Desktop dev: CPU soft 256 MiB, hard 512 MiB; GPU soft 256 MiB, hard 512 MiB.
- Constrained/iGPU: CPU soft 128 MiB, hard 256 MiB; GPU soft 128 MiB, hard 256 MiB.
- Implementations may scale GPU watermarks by VRAM when available (e.g., reserve 5–10% for UI caches).

Observability:
- Expose under `renderers/<rid>/targets/<tid>/output/v1/debug/cache/*` (debug-only):
  - `assetBytesResidentCPU`, `assetBytesResidentGPU`
  - `assetEvictionsTotal`, `assetEvictionsByKind/*`
  - `assetsPending`, `assetsLoaded`
  - `lastEvictionMs`, `lastEvictionReason`
- On hard watermark denial or repeated thrash, set `targets/<tid>/output/v1/common/lastError` once per change.

Tests:
- Watermark eviction stress: exceed soft via many assets; assert background eviction reduces usage without stalling the frame loop.
- Hard cap enforcement: attempt an allocation that would exceed hard; verify eviction runs, and if still over, allocation is denied and a placeholder renders with a single error line.
- Pinning safety: mark assets as in-use by a revision; ensure no evictions until frame end.

Cross-references:
- See “Decision: Resource system (resolved)” for asset digests and cache keys. This section formalizes eviction behavior and configuration.

## Schemas and typing (v1)

This section specifies the C++ types bound to target keys and the versioning policy for renderer I/O.

C++ types per key:
- `scene` — `std::string`
  - App-relative path to the scene root, e.g., `"scenes/<sid>"`. Must resolve within the same app root
- `desc` — `SurfaceDesc | TextureDesc | WindowBackbufferDesc | HtmlTargetDesc` (per target kind)
  - `SurfaceDesc`:
    - `size_px { int w, int h }`
    - `pixel_format` (enum): `RGBA8Unorm | BGRA8Unorm | RGBA8Unorm_sRGB | BGRA8Unorm_sRGB | RGBA16F | RGBA32F`
      - Platform notes: on Apple/Metal, `BGRA8Unorm[_sRGB]` is the common swap/present format; the software renderer uses `RGBA8Unorm`.
    - `color_space` (enum): `sRGB | DisplayP3 | Linear`
      - Write-out obeys pipeline flags (`SrgbFramebuffer` vs `LinearFramebuffer`); sRGB textures are linearized on sample.
    - `premultiplied_alpha` (bool) — default true for UI; renderers expect premultiplied inputs. If false, sources are converted at draw time (see `UnpremultipliedSrc`).
    - `progressive_tile_size_px` (int, default 64) — tile dimensions used by the progressive software presenter.
    - `metal { storage_mode (enum: Private | Shared | Managed | Memoryless), texture_usage (bitmask flags ShaderRead | ShaderWrite | RenderTarget | Blit), iosurface_backing (bool) }`
  - `TextureDesc`:
    - `size_px { int w, int h }`
    - `pixel_format` (enum): `RGBA8Unorm | BGRA8Unorm | RGBA8Unorm_sRGB | BGRA8Unorm_sRGB | RGBA16F | RGBA32F`
    - `color_space` (enum): `sRGB | DisplayP3 | Linear`
    - `usage_flags` (bitmask)
  - `WindowBackbufferDesc`:
    - `present_mode` (enum: Fifo | Mailbox | Immediate)
    - `min_images` (uint)
    - `pixel_format` (enum): `BGRA8Unorm | BGRA8Unorm_sRGB | RGBA16F`
      - Platform notes: swapchains commonly expose `BGRA8Unorm`; availability of explicit sRGB variants is platform-dependent. When unavailable, use `color_space` plus pipeline flags for sRGB encoding.
    - `color_space` (enum): `sRGB | DisplayP3 | Linear`
  - `HtmlTargetDesc`:
    - `size_px { int w, int h }`
    - `dpi_scale: float` (default 1.0)
    - `prefer_dom: bool` (default true)
    - `max_dom_nodes: uint32` (default 10000)
    - `allow_clip_path: bool` (default true; safe subset only)
    - `inline_assets: bool` (default false)
    - `embed_css: bool` (default true)
    - Defaults: prefer DOM/CSS (Tier 1). When thresholds/fidelity require fallback, emit Canvas JSON to `output/v1/html/commands` and include CSS under `output/v1/html/css` when split.
- `desc/active` — mirror of the adopted descriptor (`SurfaceDesc | TextureDesc | WindowBackbufferDesc`) for introspection
- `settings` — single `RenderSettingsV1` value (atomic whole-object replace)
  - `RenderSettingsV1`:
    - `time { double time_ms, double delta_ms, uint64_t frame_index }`
    - `pacing { std::optional<double> user_cap_fps }` (effective = min(display, cap))
    - `surface { {int w,int h} size_px, float dpi_scale, bool visibility, metal SurfaceDesc::MetalSurfaceOptions }`
    - `std::array<float,4> clear_color`
    - `camera` (optional): `{ enum Projection { Orthographic, Perspective }, float zNear, float zFar }`
    - `debug { uint32_t flags }` (optional)
    - `renderer { backend_kind (RendererKind), metal_uploads_enabled: bool }`

- `render` — execution that renders one frame for this target (no payload)
- `output/v1/common/*` — single-value registers with latest metadata:
  - `frameIndex: uint64_t` (monotonically increasing per target; practically non-wrapping; if wrap ever occurs, consumers must use unsigned modular comparisons)
  - `revision: uint64_t` (monotonically increasing per scene; practically non-wrapping)
  - `renderMs: double`
  - `lastError: std::string` (empty on success)
- `output/v1/software/framebuffer` — `SoftwareFramebuffer` (populated only when `capture_framebuffer=true`; absent otherwise to avoid default copies):
  - `std::vector<uint8_t> pixels`
  - `int width, int height, int stride`
  - `enum pixel_format`, `enum color_space`, `bool premultiplied_alpha`
- `output/v1/metal/texture` — `MetalTextureHandle` (opaque handle or registry id + size/format/color_space)
- `output/v1/vulkan/image` — `VulkanImageHandle` (opaque handle or registry id + size/format/color_space/layout)
- `output/v1/window/presentInfo` — `WindowPresentInfo` (no pixel payload; windows targets present directly)
  - `uint32_t image_count`
  - `enum present_mode`
  - `bool suboptimal`

### Pixel noise perf harness coverage (October 22, 2025)

- `examples/pixel_noise_example.cpp` now accepts `--backend=<software|metal>` (default Software2D) so perf runs can target either renderer backend without code changes.
- Baselines live under `docs/perf/`: `pixel_noise_baseline.json` tracks the Software2D path and `pixel_noise_metal_baseline.json` captures the Metal2D run with `PATHSPACE_ENABLE_METAL_UPLOADS=1`.
- `scripts/check_pixel_noise_baseline.py` reads the baseline’s `backendKind`, forwards the matching `--backend` switch, and automatically enables Metal uploads when the baseline expects Metal2D.
- CTest registers `PixelNoisePerfHarness` (Software2D) and `PixelNoisePerfHarnessMetal` (Metal2D, gated by `PATHSPACE_UI_METAL`) so the mandated 15× loop covers both backends against the same FPS/latency budgets (≥50 FPS, ≤20 ms average present/render and present-call).
- Refresh both JSON baselines with `scripts/capture_pixel_noise_baseline.sh` (add `--backend=metal` plus `PATHSPACE_ENABLE_METAL_UPLOADS=1` for the Metal capture) whenever intentional perf shifts occur; commit the pair together so CI expectations stay aligned.

App-relative resolution helpers:
- `is_app_relative(UnvalidatedPathView)`
- `resolve_app_relative(AppRoot, UnvalidatedPathView)`
- `ensure_within_app(AppRoot, ConcretePathView resolved)`

Versioning policy:
- Settings and descriptors: unversioned at the path level (pure C++ in-process; producers/consumers recompile together). Keep the `V1` suffix in C++ type names for source-level evolution
- Outputs: versioned at the path level under `output/v1`. If an incompatible change is needed, add `output/v2` and keep `output/v1` during a deprecation window (update docs/tests accordingly)

## Target keys (final)

Target base:
- `<app>/renderers/<rendererName>/targets/<kind>/<name>`
- `kind ∈ { surfaces, textures, windows, html }`

Keys under a target:
- `scene` — app-relative path to the scene root to render (must resolve within the same app root)
- `desc` — descriptor for the target (`SurfaceDesc | TextureDesc | WindowBackbufferDesc | HtmlTargetDesc`)
- `desc/active` — mirror written by renderer after reconfigure
- `status/*` — e.g., `reconfiguring`, `device_lost`, `message`
- `settings` — single `RenderSettings` value (atomic whole-object replace)
- `render` — execution to render one frame for this target
- `output/v1/...` — latest outputs for this target:
  - `common/` — timings and metadata (`frameIndex`, `revision`, `renderMs`, `lastError`)
  - `software/framebuffer` — pixel buffer + metadata (width, height, stride, format, colorSpace, premultiplied)
  - `metal/texture` or `vulkan/image` — opaque GPU handles and metadata
  - `window/presentInfo` — present metadata (image_count, present_mode, suboptimal)
  - `html/dom`, `html/css`, `html/commands`, `html/assets/*` — optional web outputs
  - `html/mode`, `html/commandCount`, `html/usedCanvasFallback` — adapter fidelity metadata
  - `html/metadata/activeMode` — resolved runtime mode (dom | canvas | webgl)

## View keys (final)

View base:
- <app>/windows/<win>/views/<view>

Keys under a view:
- surface — app-relative path to renderers/<rid>/targets/surfaces/<name>
- windowTarget — app-relative path to renderers/<rid>/targets/windows/<name>
- htmlTarget — app-relative path to renderers/<rid>/targets/html/<name>
- present/policy — AlwaysFresh | PreferLatestCompleteWithBudget | AlwaysLatestComplete
- present/params — backend-aware parameters such as staleness_budget_ms, frame_timeout_ms
- status/* — latest present metadata (e.g., chosenMode, waitMs); optional

Semantics:
- Exactly one of surface, windowTarget, or htmlTarget must be set at a time; switching is atomic and should notify the presenter.
- Presenters read binding and policy once per present; no mid-present re-reads.
- Threading: windowTarget presents must occur on the platform UI/present thread; surface blits occur on the UI thread after render completes.
- HTML views ignore present policy (always latest-complete) and simply expose the current DOM/CSS/Canvas payload.

Cross-references:
- See “Decision: Present Policy (resolved)” for policy semantics.
- See “Target keys (final)” for target key details.

## RenderSettings v1 (final)

- `time: { time_ms: double, delta_ms: double, frame_index: uint64 }`  // frame_index is monotonically increasing per target; delta_ms reflects real elapsed time even when pacing skips frames
- `pacing: { has_user_cap_fps: bool, user_cap_fps: double }`  // set `has_user_cap_fps=false` when the cap is unset
- `surface: { size_px:{w:int,h:int}, dpi_scale: float, visibility: bool }`
- `clear_color: [float,4]`
- `camera: { enabled: bool, projection: Orthographic | Perspective, zNear:float, zFar:float }`
- `debug: { enabled: bool, flags: uint32 }`
- `cache: { cpuSoftBytes:uint64, cpuHardBytes:uint64, gpuSoftBytes:uint64, gpuHardBytes:uint64 }`
- `ray_cache: { budget:{primary_rays_per_frame:uint32, refinement_pixels_per_frame:uint32, rt_bounces_per_frame:uint32}, path:{max_bounces:uint32, rr_start_bounce:uint32}, cache:{search_radius_px:float, invalidate_on_epoch_change:bool}, seed:uint64 }`

Invariants:
- Writers replace the entire `RenderSettings` at `settings` in a single atomic write (single-path whole-object)
- Renderer latches the `settings` value once at frame start and uses it for the duration of the frame; mid-frame writes do not affect the in-flight frame (adoption occurs next frame)
- Multi-producer policy (final): there is no server-side merge/queue in v1; producers must aggregate/coalesce on the client side and perform one atomic replace (last-writer-wins at the single path)
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
- See `docs/AI_ARCHITECTURE.md#ui--rendering` for the current UI/Rendering overview and Mermaid diagrams; keep the sources in `docs/images/` synchronized when the path contracts evolve.
- Implementation roadmap: `docs/SceneGraphImplementationPlan.md` details the phased delivery plan and test strategy for this design.

## TODO — Clarifications and Follow-ups

These items clarify edge cases or finalize small inconsistencies. Resolve and reflect updates in `docs/AI_ARCHITECTURE.md`, tests, and any affected examples. (Items already reflected in the main text have been removed from this list.)
