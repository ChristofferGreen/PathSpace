# Scene Graph Implementation Plan

> **Context update (October 15, 2025):** Implementation phases now assume the “Atlas” AI context; convert prior context cues to the Atlas vocabulary during execution.

## Context and Objectives
- Groundwork for implementing the renderer stack described in `docs/AI_Plan_SceneGraph_Renderer.md` and the broader architecture contract in `docs/AI_ARCHITECTURE.md`.
- Deliver an incremental path from today's codebase to the MVP (software renderer, CAMetalLayer-based presenters, surfaces, snapshot builder), while keeping room for GPU rendering backends (Metal/Vulkan) and HTML adapters outlined in the master plan.
- Maintain app-root atomic semantics, immutable snapshot guarantees, and observability expectations throughout the rollout.

Success looks like:
- A new `src/pathspace/ui/` subsystem shipping the MVP feature set behind build flags.
- Repeatable end-to-end tests (including 15× loop runs) covering snapshot publish/adopt, render, and present flows.
- Documentation, metrics, and diagnostics that let maintainers debug renderer issues without spelunking through the code.

## Workstream Overview
- **Typed wiring helpers** — `Builders.hpp` plus supporting utilities for app-relative path validation, target naming, and atomic parameter writes.
- **Scene authoring & snapshot builder** — authoring tree schema, dirty tracking, layout, `DrawableBucket` emission, and revision lifecycle/GC.
- **Renderer core (software first)** — target settings adoption, traversal of snapshots, draw command execution, color pipeline defaults, and concurrency controls.
- **Surfaces & presenters** — target configuration, render execution coordination, CAMetalLayer-backed window presentation, progressive software mode, and UI-thread integrations.
- **Input & hit testing** — DrawableBucket-driven hit paths, focus routing, and notification hooks for scene edits and event delivery.
- **Diagnostics & metrics** — unified `PathSpaceError` usage, per-target metrics, progressive copy counters, and troubleshooting hooks.
- **Resource residency & tooling** — enforce per-resource policy, adopt fingerprints across snapshots/renderers, refresh compile commands, and extend automation/test harnesses.

Each workstream lands independently but respects shared contracts (paths, atomic writes, revision pinning).

### Helper API Contract
- Location: `src/pathspace/ui/Helpers.hpp` with namespaces `Scene`, `Renderer`, `Surface`, `Window`, and `Diagnostics`.
- All helpers accept a `PathSpace&` plus typed `ConcretePath` aliases (`ScenePath`, `RendererPath`, etc.); no raw string paths cross the boundary.
- Responsibilities:
  - Enforce app-root containment and canonical naming (e.g., `renderers/<id>/targets/...`).
  - Serialize payloads into `NodeData` using existing Alpaca/typed codecs; no helper owns synchronization beyond atomic inserts/takes.
  - Return `SP::Expected<T>` for error propagation aligned with core PathSpace errors.
- Usage: SceneGraph code consumes these helpers exclusively instead of embedding path literals; tests mock them to isolate higher layers.
- Future additions (Metal/HTML adapters) extend these namespaces rather than reaching into core PathSpace directly.

## Immediate Next Steps
- ✅ (October 17, 2025) `PathRenderer2D` executes Rect, RoundedRect, Image, TextGlyphs, Path, and Mesh commands in linear light, respecting opaque/alpha ordering and updating drawable/unsupported metrics.
- ✅ (October 16, 2025) Replaced the macOS window presenter’s CoreGraphics blit with the CAMetalLayer-backed zero-copy path, validated fullscreen resize/perf behaviour manually, and updated the example/docs.
 - ✅ (October 17, 2025) PathSurfaceSoftware and PathWindowView now share an IOSurface-backed framebuffer so the presenter can bind the surface without memcpy; the zero-copy path is the new default on macOS.
- ✅ (October 17, 2025) PathRenderer2D now limits damage to tiles touched by drawables whose fingerprints/bounds changed, so unchanged revisions (including id renames) emit zero progressive updates; fullscreen repaints are reserved for resize/clear-color changes (see “Incremental software renderer” below for remaining diagnostics and hint work).
- ✅ (October 17, 2025) Progressive repaint flicker in `./build/paint_example` is resolved. Damage tracing (`PATHSPACE_TRACE_DAMAGE=1`) now shows the buffered framebuffer retaining previous content, and interactive runs match the automated regression (`Window::Present progressive updates preserve prior content`). Keep the loop harness green and capture traces again if the behaviour regresses.
- ✅ (October 17, 2025) End-to-end `Window::Present` coverage now codifies progressive seqlock behaviour: tests drive a presenter through the builders API, assert the `progressiveSkipOddSeq` metrics, and verify deadline bookkeeping (`waitBudgetMs`/staleness) using the new before-present hook in `Builders::Window::TestHooks`.
- ✅ (October 17, 2025) Added multi-target `Window::Present` scenarios to ensure renderer caches stay per-target: two surfaces backed by the same renderer now exercise independent frame indices, stored framebuffers, and progressive metrics through the builders API.
- ✅ (October 17, 2025) Automated the AlwaysFresh/deadline regression by reusing the before-present hook to force buffered-frame drops, asserting the skip path (`lastPresentSkipped`, `presentedAge*`, `waitBudgetMs`) and guarding against unintended auto-render enqueues.
- ✅ (October 17, 2025) Dirty hint buckets snap to progressive tile boundaries so `PATHSPACE_TRACE_DAMAGE=1` logs stay readable at 4K; doctests cover the presenter metrics emitted when hints restrict the damage region.
- ✅ (October 18, 2025) Achieved FPS parity between small and 4K windows and hardened diagnostics to catch regressions; keep monitoring traces but no longer treat as an open blocker.
- ✅ (October 17, 2025) Progressive tile fan-out now distributes work across all available CPU cores with a fixed 64 px tile size so incremental strokes remain ~140 FPS, and metrics continue to surface `progressiveTilesUpdated` / `progressiveBytesCopied`.
  - ✅ (October 17, 2025) Instrumented the fine-grained benchmark suite with per-phase timings (damage diff, encode, progressive copy, IOSurface publish, presenter present) to pinpoint 4 K full-clear hotspots; benchmark output now reports these averages.
  - ✅ (October 18, 2025) Hardened `paint_example`: stroke segments now batch their dirty hints, idle frames no longer submit full-surface publishes, and surface resizing reuses the existing IOSurface allocation so long sessions stay within range-group limits.
  - 🔜 Skip redundant IOSurface-to-CPU copies after successful zero-copy presents so macOS paint_example no longer spends ~22 MB per frame memcpying pixels; expose a debug knob for fallback copies instead.
  - 💤 (Optional) Add stroke-compositing to `paint_example` so successive brush strokes bake into a persistent texture (or coalesced drawable) rather than growing the snapshot unbounded; keeps long-running demos at steady FPS without exhausting IOSurface range groups.
  - 🔜 Move dirty-rect normalization/coalescing into Builders: provide a helper (or upgrade `SubmitDirtyRects`) that snaps rectangles to progressive tiles and merges overlaps so demos no longer carry bespoke hint code.
  - 🔜 Once the helper lands, strip the manual hint coalescing from `examples/paint_example.cpp` so the sample reflects the streamlined API.
- For Phase 4 follow-ups, finish the broader end-to-end scene→render→present scenarios, expand presenter wiring through `Window::Present`, surface the remaining progressive-mode diagnostics, and rerun the loop harness once integrations land.
- Begin Phase 5 test authoring for hit ordering, clip-aware picking, focus routing, and event delivery latency; follow with DrawableBucket-backed picking and wait/notify integration for dirty markers and auto-render scheduling.
- Line up Phase 6 diagnostics/tooling work: extend error/metrics coverage, normalize `PathSpaceError` reporting, expand scripts for UI log capture, and draft the debugging playbook updates before the next hardening pass.

### Incremental software renderer (new priority, October 17, 2025)
- Scope: keep the zero-copy path but eliminate full-surface repaints when only a small region changes.
- Current status:
  - ✅ (October 17, 2025) PathRenderer2D now caches per-target drawable fingerprints/bounds and diffs revisions so unchanged drawables (including id renames) leave the damage region empty; idle frames report zero progressive tile updates in tests.
  - ✅ (October 17, 2025) SceneSnapshotBuilder fingerprints are derived from transforms/bounds/commands (drawable ids no longer influence the hash), so renaming a drawable without changing its content reuses the cached damage state.
  - ✅ (October 17, 2025) `paint_example` skips redundant publishes on resize-only frames and forwards brush dirty rectangles via the new renderer hint path, so dragging a 2560 px canvas keeps >55 FPS except while actively laying down fresh strokes.
- Immediate next steps:
  - Lock in the progressive tiling strategy: default tile size stays 64×64 px, which yields ~510 tiles at 1080p, ~920 tiles at 1440p, and ~2 040 tiles at 4K. The renderer will treat tiles as the unit of work and feed them into a per-core queue so an 8‑core CPU gets ~250 tiles for a 4K full repaint (≈32 per core), keeping the full-surface path viable at ≥60 Hz. (2025-10-18: verified post-regression rollback; keep monitoring.)
  - Surface diagnostics around skipped tiles (damage rectangles, fingerprint match counts) so tooling can verify incremental behaviour without attaching a debugger. (2025-10-18: first implementation regressed fullscreen FPS; metrics removed—revisit behind a developer flag and keep an eye on `out.log` for performance while iterating.)
- Runtime targets:
  - Incremental updates should be the steady state: typical UI strokes touch only a handful of tiles, keeping frame cost well under the 16 ms (~60 Hz) budget.
  - Full-surface repaints remain first-class. Camera moves or scene-wide changes explicitly flip the damage region to `set_full()`, then PathRenderer2D fan-outs tiles across all CPU cores (tile-per-thread queue) so repainting a 4K surface still clears ≤16 ms once the inner loops are vectorized.
  - The software path owns these full clears; GPU assists stay optional (e.g. secondary effects), not the default “draw everything every frame,” so the hybrid design preserves the progressive/tiled CPU pipeline.
- Validation:
  - Added renderer doctest coverage for drawable id churn and hint-triggered damage (dirty rect hints on unchanged snapshots); still need incremental stroke/erase/clear color scenarios with explicit damage assertions.
  - Capture comparative FPS traces (small-vs-fullscreen) to confirm the fullscreen slowdown disappears.

## Phase Plan
### Phase 0 — Foundations (1 sprint)
- Audit existing `PathSpace` helpers for app-relative enforcement; add shared utilities if missing.
- Introduce feature flags/CMake options (`PATHSPACE_ENABLE_UI`, `PATHSPACE_UI_SOFTWARE`, `PATHSPACE_UI_METAL`, `PATHSPACE_ENABLE_APP`, `PATHSPACE_ENABLE_EXTRA`) without functionality to keep builds green.
- Update Doxygen/docs stubs to reserve namespaces (`pathspace/ui/*`) and ensure compile_commands integration works.
- ✅ (October 11, 2025) `SP::App` helpers landed in `src/pathspace/app/AppPaths.{hpp,cpp}` with root normalization, app-relative resolution, and target base validation for reuse across UI and tooling layers.
- ✅ (October 11, 2025) Top-level CMake now exposes `PATHSPACE_ENABLE_UI` (default OFF) plus `PATHSPACE_UI_SOFTWARE` / `PATHSPACE_UI_METAL`, `PATHSPACE_ENABLE_APP`, and `PATHSPACE_ENABLE_EXTRA` flags, and the library/tests gate feature-specific sources on these toggles.

### Phase 1 — Typed Helpers & Path Semantics (1 sprint)
Completed:
- ✅ (October 14, 2025) Added doctest coverage in `tests/ui/test_Builders.cpp` for idempotent scene creation, cross-app rejection, and atomic `Renderer::UpdateSettings` behaviour (path containment + queue draining).
- ✅ (October 14, 2025) Validated Builders via `./scripts/compile.sh --loop=15 --per-test-timeout 20` (15 iterations, 20 s per-test timeout).
- ✅ (October 11, 2025) Initial UI helper implementations (`src/pathspace/ui/Helpers.cpp`) enforce app-root containment for scene/renderer/surface/window calls with accompanying doctests in `tests/ui/test_SceneHelpers.cpp`; remaining work will flesh out Builders.hpp and atomic settings writes.
- ✅ (October 14, 2025) Documented Builders usage patterns and invariants in `docs/AI_Plan_SceneGraph_Renderer.md` (idempotent creates, atomic settings, app-root enforcement); no new canonical paths required for `AI_PATHS.md`.

### Phase 2 — Scene Schema & Snapshot Builder (2 sprints)
Completed:
- ✅ (October 14, 2025) `SceneSnapshotBuilder` now emits full SoA drawable data (transforms, bounds, material/pipeline metadata, command offsets/counts, command stream, opaque/alpha/per-layer indices) and documents the layout in `docs/AI_Plan_SceneGraph_Renderer.md` / `docs/AI_ARCHITECTURE.md`.
- ✅ (October 14, 2025) Added doctests in `tests/ui/test_SceneSnapshotBuilder.cpp` covering round-trip decoding plus retention under burst publishes.
- ✅ (October 15, 2025) Added long-running publish/prune stress coverage with metrics validation and GC metric emission (retained/evicted/last_revision/total_fingerprint_count) in `tests/ui/test_SceneSnapshotBuilder.cpp`.
- ✅ (October 15, 2025) Snapshot metadata now records resource fingerprints per revision; GC metrics aggregate total fingerprint counts for residency planning.
- ✅ (October 15, 2025) Documented the finalized binary artifact split (`drawables.bin`, `transforms.bin`, `bounds.bin`, `state.bin`, `cmd-buffer.bin`, index files) and retired the Alpaca fallback in `docs/AI_ARCHITECTURE.md` / `docs/AI_Plan_SceneGraph_Renderer.md`.

### Phase 3 — Software Renderer Core (2 sprints)
Completed:
- ✅ (October 15, 2025) Builders `Surface::RenderOnce` / `Window::Present` now call `PathRenderer2D` synchronously via `Renderer::TriggerRender`, return ready `FutureAny` handles, and update target metrics; doctests cover the integration path.
- ✅ (October 16, 2025) `PathRenderer2D` now executes recorded Rect and RoundedRect commands in linear light, keeps the opaque/alpha partitioning path alive, and publishes drawable/cull/command metrics with doctest coverage for success/error flows.
- ✅ (October 16, 2025) Added a deterministic golden framebuffer harness (with `PATHSPACE_UPDATE_GOLDENS`) plus render/present loop regressions that exercise `Surface::RenderOnce` and `Window::Present` under repeated runs; comparisons are tolerance-aware to track subtle output drift.
- ✅ (October 16, 2025) Instrumented the renderer to report sort-key violations, approximate overdraw coverage, and progressive-copy counters under `output/v1/common/*` for tooling/CI.

Remaining:
- Re-run the looped test suite and inspect goldens post implementation.

### Phase 4 — Surfaces, Presenters, and Progressive Mode (2 sprints)
Completed:
- ✅ (October 15, 2025) `PathSurfaceSoftware` progressive buffer + double-buffer landed with dedicated UI tests.
- ✅ (October 15, 2025) `PathWindowView` now presents buffered/progressive frames and writes presenter metrics via `Builders::Diagnostics::WritePresentMetrics`; UI tests split into `PathSpaceUITests` for isolation.
- ✅ (October 16, 2025) End-to-end scene → render → present doctests cover policy permutations, progressive copy assertions, and diagnostics outputs through `Builders::Window::Present`.
- ✅ (October 16, 2025) Presenter integration routes through `Window::Present`, emitting progressive-mode metrics, wait-budget data, and auto-render scheduling hooks that enqueue `AutoRenderRequestEvent` when frames remain stale.
- ✅ (October 16, 2025) Seqlock and deadline behaviour codified via `PathWindowView` and builder tests, ensuring wait-budget clamps and progressive copy skips are observable.
- ✅ (October 16, 2025) Compile/test loop harness revalidated after presenter integration (15× repeat, 20 s timeout) confirming stability.
- ✅ (October 16, 2025) Software presenter now publishes captured framebuffers under `output/v1/software/framebuffer`, enabling downstream inspection of rendered bytes alongside metadata.
- ✅ (October 17, 2025) Added a minimal paint-style demo (`examples/paint_example.cpp`) that wires the scene→render→present stack together, supports dynamic canvas resizing, and interpolates mouse strokes so contributors can exercise the full software path end-to-end.
- ✅ (October 16, 2025) macOS window presentation now uses a CAMetalLayer-backed Metal swapchain (IOSurface copies + present) instead of CoreGraphics blits; fullscreen perf is no longer CPU bound.

Completed:
- ✅ (October 18, 2025) Added fullscreen CAMetalLayer regression coverage in `PathSpaceUITests` and defaulted the presenter to zero-copy unless `capture_framebuffer` is explicitly enabled; perf regressions now fail under the UI harness.
- (Done) IOSurface-backed software framebuffer landed with PathSurfaceSoftware/PathWindowView zero-copy integration; future work should iterate on diagnostics rather than copy elimination.

### Phase 5 — Input, Hit Testing, and Notifications (1 sprint)
- ✅ (October 16, 2025) Added doctest scenarios for hit ordering, clip-aware picking, focus routing, and auto-render event scheduling via `Scene::HitTest`; notifications enqueue `AutoRenderRequestEvent` under `events/renderRequested/queue`.
- ✅ (October 16, 2025) `Scene::HitTest` now emits scene/local coordinates and per-path focus metadata so event routing can derive local offsets without re-reading scene state; doctests cover the new fields.
- ✅ (October 16, 2025) `Scene::MarkDirty` / `Scene::TakeDirtyEvent` surface dirty markers via `diagnostics/dirty/state` and a queue, and tests confirm renderers can wait on the queue without polling.
- Exercise the notification loops in the mandated test harness and document latency/ordering guarantees.

### Phase 6 — Diagnostics, Tooling, and Hardening (1 sprint)
- ✅ (October 18, 2025) Extended diagnostics coverage with unit tests that write/read present metrics (`Diagnostics::WritePresentMetrics`/`ReadTargetMetrics`) and verify error clearing; tooling now has regression coverage for metric persistence.
- ✅ (October 18, 2025) Normalized presenter/renderer error reporting around `PathSpaceError`, wiring `diagnostics/errors/live` and exposing codes/revisions via `Diagnostics::ReadTargetMetrics`.
- ✅ (October 18, 2025) Expanded `scripts/compile.sh`/CTest to cover UI components and capture failure logs via `scripts/run-test-with-logs.sh`, ensuring loop runs retain artifacts on failure.
- ✅ (October 18, 2025) Published `docs/DebuggingPlaybook.md` with the end-to-end diagnostics workflow and re-validated the 15× loop harness (`./scripts/compile.sh --test --loop=15 --per-test-timeout=20`).

### Phase 7 — Optional Backends & HTML Adapter Prep (post-MVP)
- 🚧 (October 18, 2025) Added skipped `PathSpaceUITests` scaffolding for integration replay, ObjC++ Metal harness, and HTML command parity; populate with assertions once the backends land.
- 🚧 (October 18, 2025) Introduced `PathSurfaceMetal` stub (texture allocation + resize) gated behind `PATHSPACE_UI_METAL`, linked Metal/QuartzCore, ready for presenter integration.
- Extended Metal renderer (GPU raster path) gated by `PATHSPACE_UI_METAL`; build atop the baseline Metal presenter, confirm ObjC++ integration, and expand CI coverage. **Implementation plan map (Oct 18, 2025):**
  1. **Renderer kind plumbing**
     - Teach `Builder::Renderer::Create` / metadata to persist `RendererKind::Metal2D`.
     - Resolve renderer kind in `prepare_surface_render_context`, route to software vs. metal code.
  2. **Surface cache & rendering loop**
     - Introduce `MetalSurfaceCache` (mirroring software cache) storing `PathSurfaceMetal` instances.
     - Split `render_into_surface` into backend-specific paths; metal path builds render command buffer for MTLTexture, software path stays unchanged.
     - PathRenderer2D: add backend abstraction so draw traversal fills either CPU framebuffer or Metal encoders; reuse command building, add Metal-specific upload/shader binding (initially simple textured quad pipeline).
  3. **Presenter integration**
     - Extend `PathWindowView::Present` (Apple) to accept either `PathSurfaceSoftware` or `PathSurfaceMetal` stats; when Metal, acquire CAMetalLayer drawable and blit/encode GPU texture to drawable using Metal command queue (move logic from WindowEventPump into core presenter).
     - Update macOS harness (`WindowEventPump.mm`) to detect Metal backend via stats and avoid redundant CPU copies.
  4. **Settings & diagnostics**
     - Define Metal target descriptors (`SurfaceDesc` additions if necessary), ensure RenderSettings/diagnostics include GPU timings and error paths.
     - Persist `diagnostics/errors/live` / `output/v1/common` updates for Metal frames (frameIndex, renderMs, GPU encode time).
  5. **Testing & CI**
     - Add ObjC++ harness tests (PathSpaceUITests) with Metal flag to validate presenter path (can start as skipped until GPU encoding lands).
     - Update scripts/CI docs to run Metal-enabled tests on macOS jobs; ensure graceful skip when Metal unavailable.
  6. **Follow-ups**
     - Shader/material system parity (reuse software pipeline flags).
     - Resource residency (textures/fonts) loading for GPU path.
- HTML adapter scaffolding (command stream emitter + replay harness) behind experimental flag. **Implementation plan map (Oct 18, 2025):**
  1. **Adapter core API**
     - Define `Html::Adapter` emit API (DOM/CSS/canvas/assets) and persist options in `Builders` metadata.
     - Ensure snapshot traversal supplies drawable buckets + fingerprints to the adapter.
  2. **Command stream emitter**
     - Implement DOM serializer (respect max node budget, clip handling) and Canvas JSON fallback, reusing PathRenderer2D draw traversal.
     - Emit asset blobs (images/fonts) via fingerprint paths; integrate with resource loader follow-up.
  3. **Replay harness**
     - Add standalone HTML runner (tests/ui + examples) that replays command streams, verifying parity with software renderer output.
     - Provide doctest coverage that compares software framebuffer vs. adapter DOM/Canvas render using golden expectations.
  4. **Builder integration**
     - Extend renderer target wiring to write HTML outputs under `output/v1/html/{dom,css,commands,assets}` behind experimental flag.
     - Add present pipeline hooks for HTML targets (always latest complete) and diagnostics/errors surfaces.
  5. **Tooling & CI**
     - Add headless verification step (e.g., Node/JS or WebKit snapshot) in CI to catch regressions; skip gracefully when tooling absent.
  6. **Documentation**
     - Update HTML adapter decision section with fidelity tiers, options, and troubleshooting guidance once implementation lands.
- Resource loader integration for fonts/images when snapshots require them.

### Phase 8 — Widget Toolkit & Interaction Surfaces (post-MVP)
- **Objective:** ship reusable UI widgets (button, toggle, slider, list) that sit on top of the existing scene/render/present stack while reusing PathSpace paths for state and events.
- **Scene authoring**
  - Define canonical widget scene snippets under `scenes/widgets/<widget>/` with snapshot builders that emit visual states (idle, hover, pressed, disabled).
  - Add lightweight widget builders (e.g., `Builders::Widget::Button`) that publish authoring data, bind to app-relative state paths, and express layout metadata (bounds, z-order).
  - Provide styling hooks (colors, corner radius, typography) so demos can skin widgets without forking scenes.
- **Interaction contract**
  - Wrap Phase 5 hit-test data into a widget interaction API that normalizes hover/press/release and routes to app-defined `ops/` inbox paths.
  - Expose focus navigation helpers (keyboard/gamepad) that reuse the existing `Scene::HitTest` focus metadata and auto-render events to redraw highlight states.
  - Document the path schema for widget state (e.g., `/.../widgets/<id>/{state,enabled,label}`) and ensure updates stay atomic.
- **State binding & data flow**
  - Introduce a small binding layer that watches widget state paths, diffing payloads and pushing dirty hints instead of full-surface republishes.
  - Provide sample reducers that translate widget ops into app state mutations, showcasing wait/notify usage to wake render loops.
- **Testing**
  - Extend `PathSpaceUITests` with golden snapshots and interaction sequences for each widget (hover, press, disabled) using the 15× loop to guard against race regressions.
  - Add doctest coverage for the binding helpers to confirm dirty-hint emission, focus routing, and auto-render scheduling.
- **Tooling & docs**
  - Expand `examples/` with a `widgets_demo` showcasing button/toggle interactions, FPS diagnostics, and the zero-copy presenter path.
  - Update `docs/AI_Plan_SceneGraph_Renderer.md` and `docs/AI_ARCHITECTURE.md` with widget path conventions, builder usage, and troubleshooting steps.
  - Capture authoring guidelines in `docs/SceneGraphImplementationPlan.md`'s appendix so contributors can add new widgets consistently.

## Dependencies and Ordering
- Helpers (Phase 1) unblock snapshot builder and surfaces/presenters by standardizing paths.
- Snapshot builder (Phase 2) must land before renderer to provide immutable input revisions.
- Renderer (Phase 3) depends on RenderSettings semantics and DrawableBucket schema from Phase 2.
- Surfaces/presenters (Phase 4) rely on renderer outputs and present policy definitions; progressive mode requires renderer metrics hooks.
- Input/hit testing & notifications (Phase 5) relies on renderer outputs, snapshot metadata, and presenter hooks established in earlier phases.
- Diagnostics/tooling (Phase 6) builds on metrics and error reporting emitted by previous phases; schedule alongside hardening once core flows stabilize.

## Cross-Cutting Concerns
- **Atomicity** — All settings and publish operations must remain single-path replacements; tests should assert no partial reads/writes occur.
- **Concurrency** — Rely on PathSpace’s atomic reads/writes, revision epochs, and wait/notify primitives; avoid external mutexes and validate the data-driven coordination model via tests.
- **GC & retention** — Implement revision pinning and cleanup thresholds (retain ≥3 revisions or ≥2 minutes) early to avoid storage issues.
- **Instrumentation** — Ensure every phase exposes metrics compatible with the frame profiler plan (`output/v1/common`, progressive stats, diagnostics rings).
- **Notifications** — Align dirty markers, wait queues, and auto-render triggers with the scheduling model; cover producer/consumer wakeups in tests.
- **Resources** — Respect per-item residency policies, propagate fingerprints consistently, and validate cache eviction rules with tests.
- **Documentation discipline** — Mirror behavioral changes in `AI_ARCHITECTURE.md`, update diagrams as necessary, and capture CLI/tool scripts.

## Testing Strategy
- Extend `scripts/compile.sh` to cover new targets and maintain the 15× loop with 20 s timeout.
- Add deterministic golden outputs (framebuffers, DrawableBucket metadata) with tolerance-aware comparisons.
- Stress tests: concurrent edits vs renders, present policy edge cases, progressive tile churn, input-routing races, error-path validation (missing revisions, bad settings).
- CI gating: require looped CTest run and lint/format checks before merging.

## Open Questions to Resolve Early
- Finalize `DrawableBucket` binary schema (padding, endianness, checksum) before snapshot/renderer work diverges.
- Decide on minimum color management scope for MVP (srgb8 only vs optional linear FP targets).
- Clarify resource manager involvement for fonts/images in MVP vs deferred phases.
- Validate CAMetalLayer drawable lifetime, IOSurface reuse, and runloop coordination during resize/fullscreen transitions for the Metal presenter.
- Nail down metrics format (`output/v1/common` vs `diagnostics/metrics`) to keep profiler expectations stable.
- Define sequencing for path-traced lighting and tetrahedral acceleration work (post-MVP vs incremental alongside GPU backends).

## Documentation & Rollout Checklist
- Update `README.md` build instructions once UI flags ship.
- Keep `docs/AI_Plan_SceneGraph_Renderer.md` cross-references aligned; link to this implementation plan from that doc and vice versa.
- Add developer onboarding snippets (how to run tests, inspect outputs) to `docs/`.
- Track milestone completion in `AI_TODO.task` or equivalent planning artifact.

## Related Documents
- Specification: `docs/AI_Plan_SceneGraph_Renderer.md`
- Core architecture overview: `docs/AI_ARCHITECTURE.md`

## Maintenance Considerations
- Ensure feature flags allow partial builds (e.g., disable UI pipeline when unsupported).
- Monitor binary artifact sizes for snapshots; consider tooling to inspect revisions.
- Plan for future GPU backend work without blocking MVP—keep interfaces abstract and avoid hard-coding software assumptions.
- Establish guardrails for progressive mode defaults to avoid regressing latency-sensitive apps.
- Respect residency policies and track resource lifecycle metrics so caches stay healthy.
