# Scene Graph Implementation — Completed Work

> Archived snapshot of completed milestones (updated October 29, 2025). Active items live in `docs/Plan_SceneGraph_Implementation.md`.

## Handoff Notice

> **Completion (October 29, 2025):** Added widget action callbacks so bindings can register inline lambdas. Dispatchers reuse reducer conversion to emit `WidgetAction` payloads immediately after queueing ops, multiple callbacks fan out safely, and UITests assert both invocation and `ClearActionCallbacks` behaviour.
> **Completion (October 29, 2025):** Updated `examples/widgets_example.cpp` to register a callback on the primary button that logs “Hello from PathSpace!” on press/activate, demonstrating the new action plumbing in a live sample.
> **Handoff note (October 29, 2025 @ shutdown):** Fixed the lingering slider focus dirty-hint regression by persisting slider footprints during widget creation and extending the UITest suite with the unbound focus hand-off case (`Widget focus slider-to-list transition marks previous footprint without slider binding`). Focus hand-offs now queue dirty hints for both the outgoing slider and the newly focused list before bindings exist, and both highlight verification tests pass through the 15× loop harness.
> **Completion (October 29, 2025):** Strengthened the slider → list focus UITest to assert the dirty-rect queue retains coverage for both widgets, ensuring the regression guard runs ahead of the font/resource manager rollout.
> **Completion (October 29, 2025):** Integrated the resource-backed FontManager pipeline: widget demos register PathSpaceSans regular/semibold fonts, `TypographyStyle` records resource roots/revisions/fingerprints, `TextBuilder` emits font fingerprints, and doctests cover manifest digest + bucket metadata updates.
> **Completion (October 29, 2025):** Landed `examples/widgets_example_minimal.cpp`, a lightweight slider/list/tree showcase with keyboard focus navigation and no diagnostics/screenshot plumbing so contributors can get a quick feel for the GUI builder APIs without the full gallery overhead.
> **Handoff note (October 28, 2025 @ shutdown):** Landed canonical `Widgets::Build{Button,Toggle,Slider}Preview` helpers so demos/tests no longer hand-roll preview buckets, reset the default widget theme to the original blue palette to keep the HTML harness baselines green, and added shared label/layout helpers (`Widgets::BuildLabel`, `Widgets::Input::MakeDirtyHint`/`ExpandForFocusHighlight`, etc.) so widgets_example now leans entirely on the API alongside the existing `Widgets::Input` routing. Programmatic widget interactions now flow through `Widgets::Input::ProgrammaticPointer`, eliminating the temporary `PointerOverride` hack and keeping pointer telemetry intact during keyboard/gamepad flows. Widget trace capture/replay lives under `SP::UI::WidgetTrace`, so any app can opt-in via env-configurable helpers instead of relying on the demo’s bespoke implementation. Reducer flows now expose `Widgets::Reducers::ProcessPendingActions`, letting apps drain ops queues, publish actions, and mirror diagnostics without reimplementing the reducer loop; widgets_example and UITests exercise the helper. Peeled the remaining PathRenderer2D color encode helpers into `PathRenderer2DColor.cpp` to finish the refactor slice and reran the 15× loop harness (green).
> **Handoff note (October 23, 2025 @ shutdown):** HSAT inspection now surfaces per-kind and per-mime summaries plus duplicate-path detection (CLI + HtmlAssetInspect cover the new output), so HTML payload regressions flag immediately alongside the canvas parity harness. Hit-test coverage still spans ordering, clip-aware picking, focus routing, the auto-render wait/notify path (`tests/ui/test_SceneHitTest.cpp` asserts ≤200 ms wake latency), DrawableBucket-backed multi-hit stacks (`HitTestResult::hits` with `HitTestRequest::max_results`), and widget focus-navigation helpers with auto-render scheduling. Keyboard/gamepad focus loops and widget golden snapshots live in `tests/ui/test_Builders.cpp`, and presenter diagnostics continue to mirror into `windows/<win>/diagnostics/metrics/live/views/<view>/present`. Include hygiene for the split UI modules now funnels widget-only inlines through `WidgetDetail.hpp`; binary/size guardrails ship via `scripts/compile.sh --size-report`, so we can re-enter the Metal backend follow-ups with size growth tracked automatically.
> **Handoff note (October 24, 2025 @ shutdown):** Updated renderer/architecture/docs path notes (widgets queue layout + builder flow) and synced `docs/AI_PATHS.md`. Loop tests (`ctest --test-dir build --output-on-failure -j 8 --repeat-until-fail 15 --timeout 20`) are green; no pending regressions observed during the run.
> **Handoff note (October 27, 2025 @ shutdown):** Introduced `Builders::Text`, migrated `widgets_example` off the inline glyph tables, and added UITest coverage (`test_TextBuilder.cpp`). Shared text buckets now reuse canonical typography assets and feed the new builder; PathSpaceUITests + perf harness loops stayed green (15× repeat).

# Scene Graph Implementation Plan

> **Context update (October 15, 2025):** Implementation phases now assume the assistant context introduced for this cycle; convert prior context cues to the updated vocabulary during execution.

## Context and Objectives
- Groundwork for implementing the renderer stack described in `docs/Plan_SceneGraph_Renderer.md` and the broader architecture contract in `docs/AI_Architecture.md`.
- Deliver an incremental path from today's codebase to the MVP (software renderer, CAMetalLayer-based presenters, surfaces, snapshot builder), while keeping room for GPU rendering backends (Metal/Vulkan) and HTML adapters outlined in the master plan.
- Maintain app-root atomic semantics, immutable snapshot guarantees, and observability expectations throughout the rollout.

Success looks like:
- A new `src/pathspace/ui/` subsystem shipping the MVP feature set behind build flags.
- Repeatable end-to-end tests (including 15× loop runs) covering snapshot publish/adopt, render, and present flows.
- Documentation, metrics, and diagnostics that let maintainers debug renderer issues without spelunking through the code.

## Status Snapshot (October 21, 2025)
- Material/shader bindings now flow through the shared descriptor cache, and Metal draws mirror the software renderer’s telemetry (`PathRenderer2DMetal::bind_material`, GPU blending UITest in place).
- Metal presenters remain enabled by default; the 15× loop harness (20 s timeout) is green with both software and Metal suites.
- Residency metrics under `diagnostics/metrics/residency/*` now expose raw byte counts alongside dashboard ratios/alerts; extend telemetry where gaps remain.
- Widget bindings publish dirty hints and ops inbox events (`widgets/<id>/ops/inbox/queue`) so interaction reducers can react without full-scene republishes.
- Widgets reducers drain op queues into `widgets/<id>/ops/actions/inbox/queue`; examples/tests confirm button/list actions round-trip through the helpers.
- Widget gallery, HTML tooling, and diagnostics backlog items are tracked in `docs/AI_Todo.task`; no open P0 work after the binding milestone.
- ✅ (October 21, 2025) Scene hit tests now gather z-ordered multi-hit stacks, expose `HitTestResult::hits`, and honour `HitTestRequest::max_results` for drill-down; doctests cover default ordering, clipping, and bounded stacks.
- ✅ (October 21, 2025) Focus navigation helpers landed (`Widgets::Focus::*`), standardising focus rings across widgets, scheduling auto-render events on navigation, and tagging `widgets/<id>/meta/kind` so downstream tooling can detect widget types.
- ✅ (October 21, 2025) Widgets gallery now supports keyboard focus cycling, arrow-key slider/list control, and logs reducer-emitted actions each frame for diagnostics.
- ✅ (October 21, 2025) Window diagnostics sinks mirror presenter metrics under `windows/<win>/diagnostics/metrics/live/views/<view>/present`, keeping central telemetry aligned with per-target `output/v1/common/*` updates.
- ✅ (October 20, 2025) Residency dashboard wiring publishes CPU/GPU soft & hard budget ratios plus status flags under `diagnostics/metrics/residency`, enabling external alerts without bespoke parsers.
- ✅ (October 21, 2025) `examples/widgets_example.cpp` opens the widgets gallery window, renders button/toggle/slider/list widgets with software text overlays, streams present/FPS telemetry to stdout, and republishes the gallery snapshot when LocalWindow mouse events update widget state.
- ✅ (October 23, 2025): `Example App Quit Shortcuts [BUG]` — LocalWindowBridge now captures Command+Q / Ctrl+Q / Alt+F4, drives `RequestLocalWindowQuit()` to close example windows through the shared shutdown path, and docs/AI_Onboarding_Next.md records the manual quit checklist.
- ✅ (October 23, 2025): `Example App UI Extraction [DX]` moved LocalWindow setup, resize sync, and present plumbing into shared helpers; widgets_example, pixel_noise_example, and paint_example now reuse the UI-layer APIs.
- ✅ (October 24, 2025): `PathRenderer2D` helper logic split into `PathRenderer2DDetail*` translation units so rendering orchestration stays isolated from encode/draw utilities; follow-up refactor tasks should target the new detail files rather than the main renderer entrypoint.
- ✅ (October 27, 2025): widgets_example slider drags now repaint neighbouring captions/widgets correctly thanks to widened dirty hints, and the CLI `--screenshot` path drives an automated drag before writing a PNG (via stb_image_write) so regressions reproduce without a live window. Looped UI/renderer suites remain green after the fix.

## Workstream Overview
- **Typed wiring helpers** — `Builders.hpp` plus supporting utilities for app-relative path validation, target naming, and atomic parameter writes.
- **Scene authoring & snapshot builder** — authoring tree schema, dirty tracking, layout, `DrawableBucket` emission, and revision lifecycle/GC.
- **Renderer core (software first)** — target settings adoption, traversal of snapshots, draw command execution, color pipeline defaults, and concurrency controls.
- **Surfaces & presenters** — target configuration, render execution coordination, CAMetalLayer-backed window presentation, progressive software mode, and UI-thread integrations.
- **Input & hit testing** — DrawableBucket-driven hit paths, focus routing, and notification hooks for scene edits and event delivery.
- **Diagnostics & metrics** — unified `PathSpaceError` usage, per-target metrics, progressive copy counters, and troubleshooting hooks.
- **Resource residency & tooling** — enforce per-resource policy, adopt fingerprints across snapshots/renderers, refresh compile commands, and extend automation/test harnesses.
- **Testing discipline** — when you discover a coverage gap, either land the test or capture the follow-up in this plan / `docs/AI_Todo.task` so the gap is visible for the next cycle.

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
  - ✅ (October 18, 2025) Presenter now skips IOSurface-to-CPU copies unless `capture_framebuffer` is set, eliminating the 22 MB memcpy per frame while retaining a debug/toggle for diagnostics.
  - ✅ (October 18, 2025) CAMetalLayer presenting now reuses a bounded IOSurface pool, preventing range-group exhaustion during long-running sessions.
  - ✅ (October 28, 2025) `paint_example` now composites finished strokes into a persistent canvas image (PNG asset per revision) and reuses a single drawable for the baked content; in-progress strokes render as a live stroke drawable. Long sessions stay at steady FPS and the snapshot no longer grows unbounded.
  - ✅ (October 18, 2025) `Builders::SubmitDirtyRects` now normalizes and coalesces hints automatically, snapping to the surface tile grid and clamping to bounds so apps can pass raw rectangles without bespoke math.
  - ✅ (October 18, 2025) `paint_example` now forwards raw brush rectangles directly to the helper; manual tile snapping and hint merging were removed.
- ✅ (October 20, 2025) `paint_example` accepts `--metal` to opt into the Metal2D backend, auto-enables uploads when available, and keeps software as the default for CI/headless runs.
- ✅ (October 21, 2025) Added the App::Bootstrap end-to-end regression (`tests/ui/test_AppBootstrap.cpp`) that publishes a scene snapshot, renders via `Surface::RenderOnce`, presents with `Window::Present`, and asserts both target and window diagnostics; exercised under the 15× loop (20 s timeout).
- ✅ (October 21, 2025) Presenter wiring coverage now exercises multi-window/multi-surface scenarios (PathRenderer2D UITest); remaining follow-up: keep rerunning the loop harness after each integration.
- ✅ (October 21, 2025) Window diagnostics now surface the progressive-mode metrics (tile counters, copy timings, encode fan-out) mirrored from `output/v1/common` when `PATHSPACE_UI_DAMAGE_METRICS=1`; the PathRenderer2D UITest verifies `windows/<win>/diagnostics/metrics/live/views/<view>/present` receives the tile dirty/total/skipped counters alongside progressive copy telemetry.
- ✅ (October 21, 2025) Phase 5 hit-test coverage now exercises hit ordering, clip-aware picking, focus routing, and auto-render wait/notify latency (`test_SceneHitTest.cpp`); continue tightening DrawableBucket-backed picking and related scheduling hooks as new interaction types land.
- Line up Phase 6 diagnostics/tooling work: extend error/metrics coverage, normalize `PathSpaceError` reporting, expand scripts for UI log capture, and draft the debugging playbook updates before the next hardening pass.

### Incremental software renderer (new priority, October 17, 2025)
- Scope: keep the zero-copy path but eliminate full-surface repaints when only a small region changes.
- Current status:
  - ✅ (October 17, 2025) PathRenderer2D now caches per-target drawable fingerprints/bounds and diffs revisions so unchanged drawables (including id renames) leave the damage region empty; idle frames report zero progressive tile updates in tests.
  - ✅ (October 17, 2025) SceneSnapshotBuilder fingerprints are derived from transforms/bounds/commands (drawable ids no longer influence the hash), so renaming a drawable without changing its content reuses the cached damage state.
  - ✅ (October 17, 2025) `paint_example` skips redundant publishes on resize-only frames and forwards brush dirty rectangles via the new renderer hint path, so dragging a 2560 px canvas keeps >55 FPS except while actively laying down fresh strokes.
- Immediate next steps:
  - ✅ (October 19, 2025) Locked in the progressive tiling strategy: default tile size stays 64×64 px (≈510 tiles at 1080p, ≈920 at 1440p, ≈2 040 at 4K), and new per-frame metrics (`progressiveTileSize`, `progressiveWorkersUsed`, `progressiveJobs`, `encodeWorkersUsed`, `encodeJobs`) now track how much work hits the worker pool so multi-core utilisation remains visible in logs/dashboards.
  - ✅ (October 19, 2025) Surface diagnostics around skipped tiles now land in the renderer metrics: run `./build/benchmarks/path_renderer2d_benchmark --metrics [--canvas=WIDTHxHEIGHT]` to enable `PATHSPACE_UI_DAMAGE_METRICS` and print coverage, dirty/skip ratios, and fingerprint deltas without attaching a debugger.
  - ✅ (October 19, 2025) Captured updated FPS deltas after the encode parallelism landed. `path_renderer2d_benchmark` now accepts `--canvas=WIDTHxHEIGHT`, and on the reference Mac Studio the results are:
    - 3840×2160: full repaint avg 41 ms (≈24 FPS) vs incremental strokes avg 7.0 ms (≈142 FPS), encode ≈26 ms of the full repaint and ≈0.3 ms incremental.
    - 1280×720: full repaint avg 4.9 ms (≈203 FPS) vs incremental strokes avg 1.1 ms (≈897 FPS), encode ≈3.1 ms of the full repaint and ≈0.3 ms incremental.
    - Progressive bytes copied per incremental stroke stay ≈0.07 MB thanks to the tile hint path; full repaints move ≈33 MB at 4K and ≈3.7 MB at 720p.
  - ✅ (October 19, 2025) Parallelized the encode phase: damage regions expand into per-tile encode jobs that run across the shared worker queue, cutting full-surface encode time and matching the progressive tiling model on multi-core hosts.
- Runtime targets:
  - Incremental updates should be the steady state: typical UI strokes touch only a handful of tiles, keeping frame cost well under the 16 ms (~60 Hz) budget.
  - Full-surface repaints remain first-class. Camera moves or scene-wide changes explicitly flip the damage region to `set_full()`, then PathRenderer2D fan-outs tiles across all CPU cores (tile-per-thread queue) so repainting a 4K surface still clears ≤16 ms once the inner loops are vectorized.
  - The software path owns these full clears; GPU assists stay optional (e.g. secondary effects), not the default “draw everything every frame,” so the hybrid design preserves the progressive/tiled CPU pipeline.
- Validation:
  - ✅ (October 19, 2025) Added renderer doctest coverage for incremental stroke, erase, clear-color, and dirty rect hint scenarios (see `tests/ui/test_PathRenderer2D.cpp`), with explicit assertions on damage coverage, fingerprint deltas, and progressive tile counts.
  - ✅ (October 23, 2025) Captured comparative FPS traces for 1280×720 and 3840×2160 via `scripts/capture_renderer_fps_traces.py`, storing the JSON summary in `docs/perf/renderer_fps_traces.json` so we can track incremental vs full-surface performance over time.

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
- ✅ (October 14, 2025) Documented Builders usage patterns and invariants in `docs/Plan_SceneGraph_Renderer.md` (idempotent creates, atomic settings, app-root enforcement); no new canonical paths required for `AI_Paths.md`.

### Phase 2 — Scene Schema & Snapshot Builder (2 sprints)
Completed:
- ✅ (October 14, 2025) `SceneSnapshotBuilder` now emits full SoA drawable data (transforms, bounds, material/pipeline metadata, command offsets/counts, command stream, opaque/alpha/per-layer indices) and documents the layout in `docs/Plan_SceneGraph_Renderer.md` / `docs/AI_Architecture.md`.
- ✅ (October 14, 2025) Added doctests in `tests/ui/test_SceneSnapshotBuilder.cpp` covering round-trip decoding plus retention under burst publishes.
- ✅ (October 15, 2025) Added long-running publish/prune stress coverage with metrics validation and GC metric emission (retained/evicted/last_revision/total_fingerprint_count) in `tests/ui/test_SceneSnapshotBuilder.cpp`.
- ✅ (October 15, 2025) Snapshot metadata now records resource fingerprints per revision; GC metrics aggregate total fingerprint counts for residency planning.
- ✅ (October 15, 2025) Documented the finalized binary artifact split (`drawables.bin`, `transforms.bin`, `bounds.bin`, `state.bin`, `cmd-buffer.bin`, index files) and retired the Alpaca fallback in `docs/AI_Architecture.md` / `docs/Plan_SceneGraph_Renderer.md`.

### Phase 3 — Software Renderer Core (2 sprints)
Completed:
- ✅ (October 15, 2025) Builders `Surface::RenderOnce` / `Window::Present` now call `PathRenderer2D` synchronously via `Renderer::TriggerRender`, return ready `FutureAny` handles, and update target metrics; doctests cover the integration path.
- ✅ (October 16, 2025) `PathRenderer2D` now executes recorded Rect and RoundedRect commands in linear light, keeps the opaque/alpha partitioning path alive, and publishes drawable/cull/command metrics with doctest coverage for success/error flows.
- ✅ (October 16, 2025) Added a deterministic golden framebuffer harness (with `PATHSPACE_UPDATE_GOLDENS`) plus render/present loop regressions that exercise `Surface::RenderOnce` and `Window::Present` under repeated runs; comparisons are tolerance-aware to track subtle output drift.
- ✅ (October 16, 2025) Instrumented the renderer to report sort-key violations, approximate overdraw coverage, and progressive-copy counters under `output/v1/common/*` for tooling/CI.

Remaining:
- ✅ (October 24, 2025) Re-ran the looped test suite (15× repeat, 20 s timeout) and inspected UI goldens; all baselines unchanged.

### Phase 4 — Surfaces, Presenters, and Progressive Mode (2 sprints)
Completed:
- ✅ (October 15, 2025) `PathSurfaceSoftware` progressive buffer + double-buffer landed with dedicated UI tests.
- ✅ (October 15, 2025) `PathWindowView` now presents buffered/progressive frames and writes presenter metrics via `Builders::Diagnostics::WritePresentMetrics`; UI tests split into `PathSpaceUITests` for isolation.
- ✅ (October 16, 2025) End-to-end scene → render → present doctests cover policy permutations, progressive copy assertions, and diagnostics outputs through `Builders::Window::Present`.
- ✅ (October 16, 2025) Presenter integration routes through `Window::Present`, emitting progressive-mode metrics, wait-budget data, and auto-render scheduling hooks that enqueue `AutoRenderRequestEvent` when frames remain stale.
- ✅ (October 16, 2025) Seqlock and deadline behaviour codified via `PathWindowView` and builder tests, ensuring wait-budget clamps and progressive copy skips are observable.
- ✅ (October 16, 2025) Compile/test loop harness revalidated after presenter integration (15× repeat, 20 s timeout) confirming stability.
- ✅ (October 16, 2025) Software presenter now publishes captured framebuffers under `output/v1/software/framebuffer`, enabling downstream inspection of rendered bytes alongside metadata.
- ✅ (October 21, 2025) `Window::Present` mirrors presenter stats into `windows/<win>/diagnostics/metrics/live/views/<view>/present` via `Diagnostics::WriteWindowPresentMetrics`, keeping central dashboards aligned with per-target telemetry.
- ✅ (October 22, 2025) Default software presents now skip serializing `SoftwareFramebuffer`; `Builders::Window::Present` only writes `output/v1/software/framebuffer` when `capture_framebuffer=true` and drains prior captures otherwise, avoiding the extra copy while leaving on-demand captures intact for diagnostics.
- ✅ (October 17, 2025) Added a minimal paint-style demo (`examples/paint_example.cpp`) that wires the scene→render→present stack together, supports dynamic canvas resizing, and interpolates mouse strokes so contributors can exercise the full software path end-to-end.
- ✅ (October 16, 2025) macOS window presentation now uses a CAMetalLayer-backed Metal swapchain (IOSurface copies + present) instead of CoreGraphics blits; fullscreen perf is no longer CPU bound.
- ✅ (October 21, 2025) Shipped `examples/pixel_noise_example.cpp`, a per-pixel noise perf harness that uses the software renderer plus a before-present hook to generate full-surface churn each frame; it now runs windowed by default (uncapped compute with presents throttled via `--present-refresh=<hz>`), and `--headless` keeps the UI out of the loop when you want raw throughput only (use `--report-metrics`/`--report-extended` to print diagnostics).
- ✅ (October 22, 2025) Pixel noise harness defaults to the fast path (framebuffer capture off, metrics muted, window present rate 60 Hz) with CLI escapes for diagnostics/capture (`--headless`, `--capture-framebuffer`, `--report-metrics`, `--report-extended`, `--present-call-metric`, `--present-refresh=<hz>`).
- ✅ (October 22, 2025) Parallelized the pixel noise generator across CPU threads so IOSurface writes scale with hardware concurrency; deterministic seeding keeps runs reproducible while eliminating the prior single-threaded hotspot.
- ✅ (October 22, 2025) Pixel noise window resizing now reconfigures the surface size live, keeping noise samples at native resolution instead of stretching when the window is maximized.
- ✅ (October 22, 2025) Pixel noise perf harness now runs in the 15× loop via the new `PixelNoisePerfHarness` CTest, executing `pixel_noise_example` headless with deterministic seeding and enforcing realtime budgets (≥50 FPS, ≤20 ms avg present/render). The example gained `--min-fps`, `--budget-present-ms`, and `--budget-render-ms` options plus a structured summary line so regressions surface immediately when the loop drops below target.

Completed:
- ✅ (October 18, 2025) Added fullscreen CAMetalLayer regression coverage in `PathSpaceUITests` and defaulted the presenter to zero-copy unless `capture_framebuffer` is explicitly enabled; perf regressions now fail under the UI harness.
- ✅ IOSurface-backed software framebuffer landed with PathSurfaceSoftware/PathWindowView zero-copy integration; future work should iterate on diagnostics rather than copy elimination.

Next:
- ✅ (October 23, 2025) Pixel noise harness follow-ups: the example now ships automated baselines, diagnostics, and matching visuals so perf regressions stay guarded.
- ✅ (October 23, 2025): `Example App Quit Shortcuts [BUG]` — LocalWindowBridge emits a shared quit request on Command+Q / Ctrl+Q / Alt+F4, examples break out of their loops via `LocalWindowQuitRequested()`, and `docs/AI_Onboarding_Next.md` now carries the quit shortcut manual test checklist.
- ✅ (October 23, 2025): `Example App UI Extraction [DX]` — added `Builders::App::UpdateSurfaceSize` and `Builders::App::PresentToLocalWindow`, migrated widgets/pixel_noise/paint examples to `Builders::App::Bootstrap`, and refreshed docs to describe the shared helpers.
  - ✅ (October 22, 2025) Persist baseline metrics (frame time, residency, tile stats) from the harness under `docs/perf/` via `--write-baseline`; JSON captures live stats for regression comparisons.
  - ✅ (October 22, 2025) Landed `scripts/capture_pixel_noise_baseline.sh` to produce/update versioned baselines (default output `docs/perf/pixel_noise_baseline.json`).
  - ✅ (October 22, 2025) Spun a Metal-enabled variant via `--backend=metal`, captured `docs/perf/pixel_noise_metal_baseline.json`, and added the `PixelNoisePerfHarnessMetal` CTest (PATHSPACE_ENABLE_METAL_UPLOADS-gated) so the loop covers both Software2D and Metal2D backends with the same perf budgets.
  - ✅ (October 22, 2025) Mirrored the harness expectations in `docs/Plan_SceneGraph_Renderer.md`, covering the new Metal2D backend switch, paired baselines, and the extended CTest coverage.
  - ✅ (October 22, 2025) Documented baseline workflow in `docs/AI_Debugging_Playbook.md` (Tooling §5.1) covering capture script usage and JSON interpretation.
  - ✅ (October 23, 2025) Captured a representative frame grab (`docs/images/perf/pixel_noise.png`) via the pixel noise example’s new `--write-frame` option; docs now describe capturing the PNG so perf regressions ship with a visual reference alongside metrics.
  - ✅ (October 22, 2025) Added `scripts/check_pixel_noise_baseline.py` and routed `PixelNoisePerfHarness` through it so looped runs fail whenever runtime averages exceed the baseline budgets.

### Phase 5 — Input, Hit Testing, and Notifications (1 sprint)
- ✅ (October 16, 2025) Added doctest scenarios for hit ordering, clip-aware picking, focus routing, and auto-render event scheduling via `Scene::HitTest`; notifications enqueue `AutoRenderRequestEvent` under `events/renderRequested/queue`.
- ✅ (October 16, 2025) `Scene::HitTest` now emits scene/local coordinates and per-path focus metadata so event routing can derive local offsets without re-reading scene state; doctests cover the new fields.
- ✅ (October 16, 2025) `Scene::MarkDirty` / `Scene::TakeDirtyEvent` surface dirty markers via `diagnostics/dirty/state` and a queue, and tests confirm renderers can wait on the queue without polling.
- ✅ (October 18, 2025) Exercised the blocking dirty-event wait/notify loop under the mandated 15× harness; the `Scene dirty event wait-notify latency stays within budget` doctest asserts <200 ms wake latency, preserves sequence ordering, and architecture docs now capture the latency/ordering guarantee plus the Metal→software fallback rule when uploads are disabled.
- ✅ (October 21, 2025) DrawableBucket hit tests collect z-ordered stacks via `HitTestResult::hits` and respect `HitTestRequest::max_results`, enabling drill-down while keeping top-level fields backwards-compatible; doctests cover overlap, clipping, and bounded queries.

### Phase 6 — Diagnostics, Tooling, and Hardening (1 sprint)
- ✅ (October 18, 2025) Extended diagnostics coverage with unit tests that write/read present metrics (`Diagnostics::WritePresentMetrics`/`ReadTargetMetrics`) and verify error clearing; tooling now has regression coverage for metric persistence.
- ✅ (October 18, 2025) Normalized presenter/renderer error reporting around `PathSpaceError`, wiring `diagnostics/errors/live` and exposing codes/revisions via `Diagnostics::ReadTargetMetrics`.
- ✅ (October 18, 2025) Expanded `scripts/compile.sh`/CTest to cover UI components and capture failure logs via `scripts/run-test-with-logs.sh`, ensuring loop runs retain artifacts on failure.
- ✅ (October 18, 2025) Published `docs/AI_Debugging_Playbook.md` with the end-to-end diagnostics workflow and re-validated the 15× loop harness (`./scripts/compile.sh --test --loop=15 --per-test-timeout=20`).
- ✅ (October 20, 2025) Added residency dashboard outputs: `cpuSoftBudgetRatio`, `cpuHardBudgetRatio`, `gpuSoftBudgetRatio`, `gpuHardBudgetRatio`, per-budget exceed flags, and `overallStatus` now live under `diagnostics/metrics/residency/` for every target, allowing dashboards/alerts to trigger without bespoke queries.
- ✅ (October 20, 2025) Widget interaction bindings land: `Widgets::Bindings::Dispatch{Button,Toggle,Slider}` diff widget state, submit targeted dirty rect hints, enqueue ops (`WidgetOp` values) under `widgets/<id>/ops/inbox/queue`, and auto-schedule renders when configured.
- ✅ (October 22, 2025) Audited UI/renderer translation units ≥1 000 lines, documented candidates via `scripts/lines_of_code.sh`, and refactored `ui/Builders.cpp` into focused translation units to move toward one-class-per-file modules while keeping the looped test suite green.
- ✅ (October 23, 2025) Extracted widget helper internals into `WidgetDrawablesDetail.inl` / `WidgetMetadataDetail.inl` and split the widget runtime across `WidgetBuildersCore.cpp`, `WidgetBindings.cpp`, `WidgetFocus.cpp`, and `WidgetReducers.cpp`, keeping each unit <1 000 lines while preserving shared helpers via inline includes.
- ✅ (October 23, 2025) Enforced include hygiene for the split UI modules by isolating widget-only inlines behind `WidgetDetail.hpp`, so non-widget translation units avoid the heavy drawable/metadata helpers while keeping shared helpers available.
- ✅ (October 23, 2025) Added binary/size guardrails via `scripts/compile.sh --size-report`, tracking demo binaries against `docs/perf/example_size_baseline.json` (examples build automatically when the guardrail runs).
- ✅ (October 23, 2025) `WidgetDrawablesDetail.inl` now delegates to per-widget include files (common/button/toggle/slider/list) so each inline module stays well under 1 000 lines while preserving the shared entry point.
- ✅ (October 24, 2025) Split `SceneSnapshotBuilder.cpp` into `SceneSnapshotBuilder.cpp`, `SceneSnapshotBuilderDecode.cpp`, and `SceneSnapshotBuilderFingerprint.cpp`; each stays below the 1 000 line target and CMake now builds the new units.
- ✅ (October 24, 2025) Split `PathRenderer2D`’s damage/progressive helpers into `PathRenderer2DDamage.cpp` with shared declarations in `PathRenderer2DInternal.hpp`, trimming ~600 LOC so the main file now focuses on orchestration (~2.5 k LOC remaining). Next slices will peel encode/color helpers to finish the <1 000 LOC push.
  - ✅ (October 28, 2025) Moved encode job helpers plus the linear-buffer clear/resize utilities into `PathRenderer2DEncode.cpp`, keeping shared math in `PathRenderer2DInternal.*` and cutting another ~80 LOC from `PathRenderer2D.cpp`.
  - Keep shared utilities (`DamageRect`, incremental encode helpers, color math) in `PathRenderer2DInternal.*` so subsequent slices only shuffle call-sites; avoid introducing new headers mid-slice without a passing build.
  - ✅ (October 28, 2025) Peeled the remaining color encode helpers (`encode_pixel`, premultiply conversions) into `PathRenderer2DColor.cpp`, kept the shared contracts in `PathRenderer2DDetail.hpp`, and revalidated the 15× loop harness.
  - After each slice, rerun `ctest --test-dir build --output-on-failure -j --repeat-until-fail 15 --timeout 20` and update the doc status; only chase the next extraction after the previous slice is merged to keep rollback simple.
- ✅ (October 27, 2025) widgets_example gallery previews now mirror the focus highlight overlay used by live widgets, so the screenshot workflow once again shows the active focus target without relying on the runtime bindings.
- Refresh `docs/AI_Architecture.md` and renderer diagrams once files move so architecture snapshots continue to match the code layout.
- ✅ (October 23, 2025) Added a performance regression guardrail (`scripts/perf_guardrail.py`) that runs the PathRenderer2D benchmark plus the pixel noise presenter, compares results against `docs/perf/performance_baseline.json`, records history under `build/perf/`, and integrates with both `scripts/compile.sh --perf-report` and the local `pre-push` hook to block pushes on regressions.
- ✅ (October 24, 2025) Added a renderer fault-injection harness (`tests/ui/test_RendererFaultInjection.cpp`) that toggles Metal upload flags, simulates surface descriptor mismatches, and renders after drawables are removed mid-frame to verify diagnostics and error paths remain actionable.
- ✅ (October 23, 2025) Added optional ASan/TSan build/test modes via `scripts/compile.sh` (plus pre-push toggles) so maintainers can trigger sanitized loops on demand without a nightly job.
  - `./scripts/compile.sh --asan-test` / `--tsan-test` auto-select sanitizer build directories, disable Metal by default, and export recommended runtime env vars; pre-push hook honours `RUN_ASAN=1` / `RUN_TSAN=1` with optional `ASAN_LOOP` / `TSAN_LOOP` overrides.
- ✅ (October 23, 2025) Document how the expanded diagnostics map into dashboards/alerts (metrics → panels/thresholds) so maintainers know where to monitor residency, progressive tiles, and performance regression outputs.

### Phase 7 — Optional Backends & HTML Adapter Prep (post-MVP)
- ✅ (October 20, 2025) `PathSpaceUITests` now cover HTML canvas replay parity and the ObjC++ Metal presenter harness, replacing the skipped scaffolding.
- ✅ (October 24, 2025) `PathSurfaceMetal` now allocates IOSurface-backed textures when `iosurface_backing=true`, aligning the Metal cache with CAMetalLayer expectations and clearing the stub status.
- ✅ (October 28, 2025) Builders can now provision Metal targets end-to-end; optional GPU uploads stay gated on `PATHSPACE_ENABLE_METAL_UPLOADS=1` for CI safety, and the architecture doc now mirrors the PathRenderer2D/Builder translation-unit split so contributors can navigate the current file layout (see docs/AI_ARCHITECTURE.md update).
- ✅ (October 19, 2025) PathSurfaceMetal now caches the shared material descriptors emitted by PathRenderer2D so GPU uploads reuse the same shading telemetry when `PATHSPACE_ENABLE_METAL_UPLOADS=1`.
- ✅ (October 19, 2025) Renderer residency metrics now aggregate software surfaces and cached image payloads, publishing accurate CPU/GPU byte totals for diagnostics before enabling GPU shading.
- ✅ (October 19, 2025) Adaptive progressive tile sizing tightens tile dimensions for localized damage so brush-sized updates stick to 32–64 px tiles instead of full-surface fan-out, trimming encode work ahead of FPS parity tuning.
- Extended Metal renderer (GPU raster path) gated by `PATHSPACE_UI_METAL`; build atop the baseline Metal presenter, confirm ObjC++ integration, and expand CI coverage. **Implementation plan map (Oct 18, 2025, updated Oct 19):**
1. **Renderer kind plumbing**
    - ✅ (October 19, 2025) `Builder::Renderer::Create` metadata now persists the requested `RendererKind`, and helpers/tests consume the new params signature.
    - ✅ (October 19, 2025) `prepare_surface_render_context` resolves renderer kind per target, routing targets through cached software or Metal surfaces while keeping the software fallback as default when uploads stay disabled.
2. **Surface cache & rendering loop**
    - ✅ (October 19, 2025) Split render helpers so Metal targets reuse the shared cache but funnel through a backend-aware `render_into_target`; contexts fall back to software when uploads are disabled and tests exercise the unified path.
- ✅ (October 19, 2025) PathRenderer2D now streams Metal frames directly into the cached CAMetalLayer texture (skipping the software upload hop when Metal uploads are enabled) while keeping residency metrics in sync.
- ✅ (October 19, 2025) `Renderer::TriggerRender` now reuses the shared surface caches (software and Metal) so ad-hoc renders avoid reallocating per call.
- ✅ (October 20, 2025) `PathRenderer2DMetal` now renders rects, rounded rects, text quads, and textured images directly on the GPU, with shared material/shader keys driving pipeline state so GPU frames mirror software telemetry; glyph/material pipeline parity will continue to build on the descriptor cache.
    - ✅ (October 23, 2025) Glyph-quads text path hardened as an explicit fallback: renderer exposes `textPipeline`/`textFallback{Allowed,Count}` metrics, debug flags (`RenderSettings::Debug::kForceShapedText`, `kDisableTextFallback`) plus env toggles (`PATHSPACE_TEXT_PIPELINE`, `PATHSPACE_DISABLE_TEXT_FALLBACK`) control selection, and `tests/ui/test_PathRenderer2D.cpp` now exercises both shaped-request and glyph-quad flows to keep the regression harness alive.
 3. **Presenter integration**
     - ✅ (October 21, 2025) `PathWindowView::Present` (Apple) now handles both `PathSurfaceSoftware` and `PathSurfaceMetal` stats, acquiring the CAMetalLayer drawable and issuing the Metal blit inside the presenter so builders/tests exercise the shared path instead of relying on the legacy window pump.
     - ✅ (October 19, 2025) Replaced the sample-specific `WindowEventPump.mm` with the shared `LocalWindowBridge` in the UI library; examples now consume the bridge and keep only input wiring.
     - ✅ (October 19, 2025) PathWindowView now drives CAMetalLayer presents via the UI library, records GPU encode/present timings, and exposes configuration hooks; example harnesses only forward window/layer handles. Remaining platform scaffolding will shrink to input/event dispatch as the shared bridge matures.
4. **Settings & diagnostics**
     - ✅ (October 19, 2025) Extended `SurfaceDesc`/`RenderSettings` with Metal options (storage mode, usage flags, iosurface backing) and recorded the resolved backend/Metal upload state per frame so diagnostics retain GPU context alongside timings/errors.
     - ✅ (October 19, 2025) Persist `diagnostics/errors/live` / `output/v1/common` updates for Metal frames (frameIndex, renderMs, GPU timings) and gate residency/cache metrics under `diagnostics/metrics/residency` so dashboards can track resource pressure.
     - ✅ (October 19, 2025) PathRenderer2D now maintains a shared material descriptor cache so Metal and software paths emit identical shading telemetry across backends.
5. **Testing & CI**
    - ✅ (October 19, 2025) Added a PATHSPACE_ENABLE_METAL_UPLOADS-gated ObjC++ PathSpaceUITest (`tests/ui/test_PathWindowView_Metal.mm`) that exercises the CAMetalLayer presenter when Metal uploads are enabled.
    - ✅ (October 20, 2025) Added GPU blending coverage (`tests/ui/test_PathWindowView_Metal.mm`: "PathRenderer2DMetal honors material blending state") to confirm material-driven pipeline state matches software behaviour.
     - ✅ (October 19, 2025) Local `pre-push` hook now auto-enables Metal presenter coverage (`--enable-metal-tests`) on macOS hosts, while respecting `DISABLE_METAL_TESTS=1` to fall back when GPU access is unavailable.
     - ✅ (October 19, 2025) GitHub Actions now runs a macOS job that invokes `./scripts/compile.sh --enable-metal-tests --test --loop=1`, while tests continue to skip gracefully when Metal uploads remain disabled or unsupported.
     - Builders/UI diagnostic suites leave Metal-specific assertions to the gated UITest so the core builders coverage stays backend-agnostic even when PATHSPACE_ENABLE_METAL_UPLOADS=1.
    - ✅ (October 20, 2025) `./scripts/compile.sh` enables Metal tests by default; use `--disable-metal-tests` only when the host cannot provide a compatible GPU.
6. **Follow-ups**
     - ✅ (October 19, 2025) Renderer stats and diagnostics now cover GPU error paths end-to-end; tests assert Diagnostics::ReadTargetMetrics surfaces Metal presenter failures.
     - ✅ (October 19, 2025) Shader/material system parity established by deriving shared shader keys from software pipeline flags and exposing them to the Metal surface.
     - ✅ (October 19, 2025) Resource residency metrics now aggregate texture usage for GPU paths; Metal surfaces track resource fingerprints and publish residency totals.
- ✅ (October 19, 2025) PATHSPACE_ENABLE_METAL_UPLOADS coverage now exercises the full Metal present path, checks shader/material descriptor parity, and asserts residency/cache metrics (cpu/gpu bytes) are published so dashboards/CI immediately flag resource pressure regressions (`tests/ui/test_PathWindowView_Metal.mm`: "Metal pipeline publishes residency metrics and material descriptors").
- **HTML follow-ups (October 19, 2025):** remaining work is documenting fidelity troubleshooting (see Phase 7 items 5–6); new CI harness (`HtmlCanvasVerify`) is in place—keep it updated when adapter schemas evolve.
  - ✅ (October 22, 2025) Added themed widget scene replays to `HtmlCanvasVerify`, hashing native vs HTML replays so palette and typography drifts surface immediately when either pipeline diverges.
  - ✅ (October 22, 2025) Published `docs/HTML_Adapter_Quickstart.md` with the harness workflow, HSAT tooling steps, and troubleshooting signatures so future cycles can validate HTML fidelity without rediscovering the flow.
  - ✅ (October 23, 2025) `pathspace_hsat_inspect` now reports kind/mime summaries, duplicate logical paths, and empty-asset counts; `HtmlAssetInspect` exercises the new fields so HSAT regressions fail fast.
  - **Oct 21, 2025 update:** PathSpace core now preserves `Html::Asset` vectors across translation units (see commit `fix(ui): ensure html assets survive cross-tu reads`). The new `Html assets round-trip without HtmlSerialization include` UITest guards the regression. Resume the documentation follow-ups under Phase 7 items 5–6.
- HTML adapter scaffolding (command stream emitter + replay harness) behind experimental flag. **Implementation plan map (Oct 18, 2025):**
  1. **Adapter core API**
     - ✅ (October 19, 2025) `Html::Adapter::emit` now produces DOM/CSS/canvas outputs and honours emit options.
     - ✅ (October 19, 2025) Snapshot traversal supplies drawable buckets and fingerprints directly to the adapter.
  2. **Command stream emitter**
     - ✅ (October 19, 2025) DOM serializer and canvas JSON fallback reuse the drawable command stream while respecting node budgets and fallback policy.
     - ✅ (October 21, 2025) Html adapter now resolves asset blobs (images/fonts) via fingerprint paths using the renderer-provided loader, so emitted assets carry real bytes without post-processing.
  3. **Replay harness**
     - ✅ (October 19, 2025) Added standalone HTML runner (tests/ui + example) that replays canvas command streams and checks parity with PathRenderer2D output.
     - ✅ (October 19, 2025) Doctest coverage compares PathRenderer2D framebuffer against replayed canvas output to guard regressions.
  4. **Builder integration**
     - ✅ (October 19, 2025) Renderer targets now emit HTML outputs under `output/v1/html/{dom,css,commands,assets}` using the adapter and persist the applied options.
     - ✅ (October 19, 2025) HTML `Window::Present` now enforces the `AlwaysLatestComplete` policy, writes diagnostics/residency metrics, and is covered by doctests (`Window::Present writes HTML present metrics and residency`).
  5. **Tooling & CI**
     - ✅ (October 19, 2025) Added the Node-based `HtmlCanvasVerify` CTest (backed by `scripts/verify_html_canvas.js`) so CI exercises DOM vs Canvas outputs when `node` is present and emits a skip notice otherwise.
  6. **Documentation**
     - ✅ (October 19, 2025) Documented HTML adapter fidelity tiers, configuration knobs (`max_dom_nodes`, `prefer_dom`, `allow_canvas_fallback`), asset hydration, and debugging steps in `docs/Plan_SceneGraph_Renderer.md`; keep the section current as adapter behavior evolves.
- ✅ (October 20, 2025) Resource loader integration (HTML + renderers) now hydrates image/font assets and persists them under `output/v1/html/assets` using a stable binary `Html::Asset` codec; `Renderer::RenderHtml hydrates image assets into output` and the new `Html::Asset vectors survive PathSpace round-trip` doctest both pass with hydrated bytes.
- ✅ (October 20, 2025) Documented the HTML asset codec (HSAT framing, legacy migration) in `docs/Plan_SceneGraph_Renderer.md`; update the renderer plan when fields or versions change.
- ✅ (October 20, 2025) Removed the legacy Alpaca serializer fallback; HSAT payloads are now required for `output/v1/html/assets`.
- ✅ (October 21, 2025) Added the `pathspace_hsat_inspect` CLI plus `HtmlAssetInspect` regression (Node harness) to decode HSAT payloads; extend the fixture when new asset fields (e.g., material descriptors) land.

### Phase 8 — Widget Toolkit & Interaction Surfaces (post-MVP)
- **Objective:** ship reusable UI widgets (button, toggle, slider, list) that sit on top of the existing scene/render/present stack while reusing PathSpace paths for state and events.
- **Scene authoring**
  - ✅ (October 21, 2025) Defined canonical widget scene snippets under `scenes/widgets/<widget>/states/*`, publishing idle/hover/pressed/disabled snapshots for each widget builder so state transitions reuse authored revisions.
- ✅ (October 19, 2025) Added lightweight builders for buttons and toggles (`Builders::Widgets::CreateButton`, `CreateToggle`) that publish authoring data, bind to app-relative state paths, and express layout metadata (bounds, z-order).
- ✅ (October 20, 2025) Added a slider builder (`Builders::Widgets::CreateSlider`) with theme metadata, range/state storage, and snapshot generation for the minimal 2D theme.
- ✅ (October 20, 2025) Added a list builder (`Builders::Widgets::CreateList`) that publishes canonical list scenes, metadata (`meta/style`, `meta/items`), and default selection state while validating item identifiers.
- ✅ (October 21, 2025) Added a reusable stroke primitive (polyline/triangle strip) so paint examples track a point list per stroke instead of emitting per-dab rects; renderer + HTML adapter now consume the command and paint_example switched to the new primitive.
  - ✅ (October 21, 2025) Provide styling hooks (colors, corner radius, typography) so demos can skin widgets without forking scenes. `Widgets::WidgetTheme` centralises per-widget styles, builders publish the metadata, and `widgets_example` now loads a theme selectable via `WIDGETS_EXAMPLE_THEME` (default or `sunset`).
- **Interaction contract**
  - ✅ (October 21, 2025) Hit-test authoring ids now embed canonical `/widgets/<id>/authoring/...` paths and `Widgets::ResolveHitTarget`/`WidgetBindings::PointerFromHit` helpers normalize hover/press routing into the existing bindings + `ops/` queues.
  - ✅ (October 21, 2025) Focus navigation helpers (`Widgets::Focus`) reuse `Scene::HitTest` metadata, maintain canonical focus state under `widgets/focus/current`, toggle widget scene states, and enqueue auto-render requests so highlight transitions redraw immediately.
  - ✅ (October 21, 2025) Added UITest coverage for keyboard (Tab/Shift+Tab) and gamepad focus hops (`tests/ui/test_Builders.cpp`), asserting focus state transitions and `focus-navigation` auto-render scheduling so the 15× loop flags regressions immediately.
  - ✅ (October 21, 2025) Documented the canonical widget state schema and confirmed updates stay atomic:
    - `widgets/<id>/state`, `widgets/<id>/enabled`, and `widgets/<id>/label` hold the live state tuple authored by builders.
    - Interaction queues live under `widgets/<id>/ops/inbox/queue` and reducer outputs land in `widgets/<id>/ops/actions/inbox/queue`.
    - Canonical state snapshots reside in `scenes/widgets/<id>/states/{idle,hover,pressed,disabled}` and are reused by bindings, reducer tests, and golden renders.
  - ✅ (October 24, 2025) Surfaced the currently focused widget in `widgets_example` with a PathSpace-tracked overlay tied to `widgets/focus/current`; keyboard and pointer focus now repaint immediately with a visible highlight.
  - ✅ (October 24, 2025) Promoted focus highlight rendering into the widget builders: `Widgets::Focus` now tags `state.focused`, button/toggle/slider/list/tree drawables append the canonical highlight, and `widgets_example` no longer maintains a bespoke overlay.
  - ✅ (October 27, 2025) Implemented an API-driven pulsing highlight (1 s period, lightening/darkening cycle) that re-tints the canonical focus drawable without app-side timers. Pulsing is enabled by default via `Widgets::Focus::MakeConfig`, can be overridden with `SetPulsingHighlight`, the renderer modulates the highlight each frame, and UITests/renderer tests cover the animated state. Highlight tint now keys off each widget’s accent color (default theme = blue) instead of the previous yellow ring.
  - ✅ (October 27, 2025) Moved glyph rasterization / text bucket assembly into `Builders::Text`, migrated widgets_example and added UITest coverage so demos consume canonical typography assets instead of embedding bitmap glyph tables.
  - ✅ (October 27, 2025) Provided an API-level list widget preview/layout helper (`Widgets::BuildListPreview`) that emits drawable buckets plus row/label geometry; widgets_example now consumes it instead of custom layout math.
  - ✅ (October 27, 2025) Centralized tree view geometry (row indentation, toggle bounds, hover regions) inside `Widgets::Tree` builders via `BuildTreePreview`; widgets_example now consumes the shared layout metadata instead of recomputing bounds.
  - ✅ (October 27, 2025) Expose stack layout preview metrics via the widgets API, replacing the custom stack spacing/padding math in widgets_example with a reusable helper.
  - ✅ (October 28, 2025) Add an input routing helper that maps window events to widget bindings and focus updates, eliminating the hand-rolled pointer/focus handlers in widgets_example.
  - ✅ (October 28, 2025) Ship canonical widget preview drawables (button/toggle/slider) from the API so demos don’t recreate bucket geometry (`Widgets::BuildButtonPreview`, `BuildTogglePreview`, `BuildSliderPreview` now power widgets_example and tests).
  - ✅ (October 28, 2025) Expose shared label + layout utilities (`Widgets::BuildLabel`, `Widgets::Input::MakeDirtyHint` / `ExpandForFocusHighlight` / `BoundsFromRect`) so examples/tests can rely on the API instead of duplicating text/dirty-hint wiring.
  - ✅ (October 28, 2025) Replaced the sample’s `PointerOverride` helper with the exported `Widgets::Input::ProgrammaticPointer` affordance so keyboard/gamepad flows can dispatch ops without mutating pointer state; widgets_example now relies on the API helper.
  - ✅ (October 28, 2025) Promoted the `WidgetTrace` capture/replay utility into shared `SP::UI` tooling (`include/pathspace/ui/WidgetTrace.hpp`) so apps can configure trace env vars and reuse the recorder without copying widgets_example internals.
  - ✅ (October 28, 2025) Provide an API surface for draining and publishing widget reducer actions, eliminating the bespoke `process_widget_actions` loop in widgets_example (`Widgets::Reducers::ProcessPendingActions` now wraps reduce + publish and PathSpaceUITests cover the flow).
  - ✅ (October 29, 2025) Expanded focus dirty hints to match highlight footprints: `WidgetFocus::{Set,Clear}` inflate rectangles via `Widgets::Input::FocusHighlightPadding()`, and `tests/ui/test_Builders.cpp` now asserts highlight edge coverage so blur repaint regressions trip the looped UITests.
  - ✅ (October 28, 2025) Expose keyboard/analog slider adjustment helpers (`Widgets::Input::AdjustSliderByStep`, `AdjustSliderAnalog`, and `SliderStep`) so apps don’t need to reimplement slider math or pointer overrides for commit events; widgets_example and UITests now exercise the helpers.
- **State binding & data flow**
- ✅ (October 19, 2025) Introduced initial state update helpers for buttons/toggles that coalesce redundant writes and mark the owning scene `DirtyKind::Visual` only when values change.
- ✅ (October 20, 2025) Binding layer (`Widgets::Bindings::Dispatch{Button,Toggle,Slider}`) watches widget state, emits dirty hints, and writes interaction ops (press/release/hover/toggle/slider events) into `widgets/<id>/ops/inbox/queue`. Reducer samples live in this plan’s appendix; schema covers `WidgetOpKind`, pointer metadata, value payloads, and timestamps for reducers to consume via wait/notify.
- ✅ (October 20, 2025) Added list state/update helpers (`Widgets::UpdateListState`) plus bindings (`CreateListBinding`/`DispatchList`) that emit `ListHover`, `ListSelect`, `ListActivate`, and `ListScroll` ops with dirty rect + auto-render integration.
- ✅ (October 20, 2025) Introduced reducer helpers (`Widgets::Reducers::ReducePending` / `PublishActions`) so apps can drain widget op queues into `ops/actions/inbox/queue`; example + doctests cover button activation and list selection flows.
- **Testing**
  - ✅ (October 21, 2025) `PathSpaceUITests` now render button/toggle/slider/list goldens and replay hover/press/disabled sequences in `tests/ui/test_Builders.cpp`, keeping the 15× loop sensitive to widget regressions.
  - ✅ (October 22, 2025) Added doctest coverage that exercises binding helpers with auto-render disabled (`Widgets::Bindings::DispatchButton honors auto-render flag`) ensuring dirty hints still emit without queueing `renderRequested` events; existing focus/navigation tests continue to cover focus routing.
  - ✅ (October 21, 2025) Added adjacent-widget dirty propagation coverage (`tests/ui/test_Builders.cpp`: "Widgets dirty hints cover adjacent widget bindings"), confirming overlapping dirty hints schedule renders for neighbouring widgets while their state remains unchanged.
  - ✅ (October 23, 2025) Introduced a widget reducers/bindings fuzz harness (`tests/ui/test_WidgetReducersFuzz.cpp`) that randomizes pointer and keyboard sequences, validates dirty hints and auto-render events, drains reducers, and republishes actions to assert queue invariants.
- **Tooling & docs**
- ✅ (October 19, 2025) Expanded `examples/widgets_example.cpp` to publish button + toggle widgets and demonstrate state updates; grow into a full gallery as additional widgets land.
- ✅ (October 20, 2025) widgets_example now instantiates slider and list widgets, exercises the state helpers, and prints the relevant path wiring to guide gallery expansion; continue instrumenting interaction telemetry in follow-up work.
- ✅ (October 21, 2025) widgets_example now drives the gallery window, renders all shipped widgets with inline text labels, logs per-second FPS/present telemetry using the software presenter, and feeds LocalWindow mouse input through widget bindings to republish the gallery snapshot on interaction.
- ✅ (October 21, 2025) widgets_example adds keyboard focus cycling (Tab/Shift+Tab), arrow-key slider/list control, and reducer action logging so gallery runs surface queue activity without external tooling.
- ✅ (October 21, 2025) Introduced `Builders::App::Bootstrap` helper so examples/tests can create renderer/surface/window defaults for a scene with one call; docs/onboarding updated alongside the helper.
- ✅ (October 21, 2025) widgets_example now consumes `Widgets::WidgetTheme` output (and `WIDGETS_EXAMPLE_THEME`) so demos can swap color palettes and typography without rewriting scenes; theme defaults live next to the builders for reuse.
  - ✅ (October 23, 2025) Added a theme hot-swap UITest (`tests/ui/test_Builders.cpp`: "Widgets::WidgetTheme hot swap repaints button scenes and marks dirty") that applies default/sunset themes in-place, asserts scene/state revisions bump, and verifies metadata + drawable colors update without stale cache artifacts.
  - ✅ (October 23, 2025) Expanded `tests/ui/test_AppBootstrap.cpp` to cover present policy configuration, renderer setting overrides, initial dirty rect emission, and invalid view identifiers so helper adoption stays guarded under the PathSpaceUITests loop.
  - ✅ (October 23, 2025) Added widget session capture/replay tooling (`scripts/record_widget_session.sh`, `scripts/replay_widget_session.sh`) plus trace env support (`WIDGETS_EXAMPLE_TRACE_RECORD`, `WIDGETS_EXAMPLE_TRACE_REPLAY`) and published `docs/Widget_Contribution_Quickstart.md` so contributors can share deterministic pointer/keyboard traces alongside new widget work.
  - ✅ (October 23, 2025) Landed the initial layout container widgets (vertical/horizontal stacks) with `Widgets::CreateStack`/`UpdateStackLayout`. They measure child widget scenes, compute spacing/alignment metadata, publish aggregated stack snapshots, and surface dirty hints through bindings. UITests cover layout computation and binding dirty propagation; gallery follow-ups remain to showcase the containers in the demo and add grid variants.
  - ✅ (October 23, 2025) Built a tree view widget supporting expand/collapse, selection, and async data loading hooks, complete with bindings, reducer support, canonical paths, and UITest coverage (gallery demo follow-up tracked below).
- ✅ (October 24, 2025) Extended `widgets_example` to showcase layout containers and the tree view, wiring stack/tree bindings with keyboard controls (`[`/`]` spacing, `H` axis toggle), pointer/keyboard navigation, and reducer telemetry so contributors can exercise them interactively.
- ✅ (October 24, 2025) Updated `docs/Plan_SceneGraph_Renderer.md` and `docs/AI_Architecture.md` with widget path conventions, builder usage guidance, and troubleshooting notes.
- ✅ (October 20, 2025) Documented widget ops schema: queue path (`widgets/<id>/ops/inbox/queue`), `WidgetOp` fields (kind, pointer metadata, value, timestamp) and reducer sample wiring.
- ✅ (October 20, 2025) Reducer samples now live in `Widgets::Reducers`, publishing actions under `widgets/<id>/ops/actions/inbox/queue`; keep telemetry/docs in sync when new action fields or op kinds land.
  - ✅ (October 27, 2025) Added automated window capture: `widgets_example --screenshot <path>` now runs headless, drives a scripted slider drag, renders once, and writes a PNG via stb_image_write—useful for visual regression checks and focus/highlight debugging even on GUI-less hosts.
**Widget ops schema (October 20, 2025)**
- Queue path: `widgets/<id>/ops/inbox/queue` (per-widget FIFO consumed via `take<WidgetOp>`).
- `WidgetOp` payload: `{ kind: WidgetOpKind, widget_path: string, pointer: { scene_x, scene_y, inside, primary }, value: float, sequence: uint64, timestamp_ns: uint64 }`.
- Supported kinds (October 20, 2025): `HoverEnter`, `HoverExit`, `Press`, `Release`, `Activate`, `Toggle`, `SliderBegin`, `SliderUpdate`, `SliderCommit`, `ListHover`, `ListSelect`, `ListActivate`, `ListScroll` (document new ones alongside widget additions and clamp indices via widget metadata before mutating app state).
- Reducer shape: wait/notify loop blocks on the queue, translates ops into app actions (`ops/<action>/inbox`) and calls `Widgets::Update*State` helpers to keep scenes in sync without republishing whole snapshots.
  - Capture authoring guidelines in `docs/Plan_SceneGraph_Implementation.md`'s appendix so contributors can add new widgets consistently.

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
- **Documentation discipline** — Mirror behavioral changes in `AI_Architecture.md`, update diagrams as necessary, and capture CLI/tool scripts.

## Testing Strategy
### Current Automation Snapshot (October 19, 2025)
- `./scripts/compile.sh --test --loop=1 --per-test-timeout 20` offers a fast smoke run when chasing regressions.
- `./scripts/compile.sh --enable-metal-tests --test --loop=1 --per-test-timeout 20` exercises the Metal presenter path on macOS hosts; keep it gated behind `PATHSPACE_ENABLE_METAL_UPLOADS` in CI.
- The local `pre-push` hook (`scripts/git-hooks/pre-push.local.sh`) enables Metal presenter coverage by default; export `DISABLE_METAL_TESTS=1` when GPU access is unavailable.
- CI currently runs the Linux matrix plus a macOS job invoking `./scripts/compile.sh --enable-metal-tests --test --loop=1` to guard the presenter path.
- Draft a Linux/Wayland bring-up checklist (toolchain flags, presenter shims, input adapters, CI coverage) so non-macOS contributors know the remaining gaps for the UI stack.
- ✅ (October 24, 2025) Extended `scripts/compile.sh` to auto-run the PixelNoise perf harness (software + Metal) inside the mandated 15× loop, default the per-test timeout to 20 s, and enable `BUILD_PATHSPACE_EXAMPLES=ON` whenever tests run.
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
- Keep `docs/Plan_SceneGraph_Renderer.md` cross-references aligned; link to this implementation plan from that doc and vice versa.
- Add developer onboarding snippets (how to run tests, inspect outputs) to `docs/`.
- Track milestone completion in `AI_Todo.task` or equivalent planning artifact.
- As of October 19, 2025, `docs/AI_Onboarding.md`, `docs/AI_Architecture.md`, `docs/Plan_SceneGraph_Renderer.md`, and `docs/AI_Debugging_Playbook.md` already capture the Metal presenter metrics, residency counters, and looped test workflow; keep future changes synchronized across those references.

## Related Documents
- Specification: `docs/Plan_SceneGraph_Renderer.md`
- Core architecture overview: `docs/AI_Architecture.md`

## Maintenance Considerations
- Ensure feature flags allow partial builds (e.g., disable UI pipeline when unsupported).
- Monitor binary artifact sizes for snapshots; consider tooling to inspect revisions.
- Plan for future GPU backend work without blocking MVP—keep interfaces abstract and avoid hard-coding software assumptions.
- Establish guardrails for progressive mode defaults to avoid regressing latency-sensitive apps.
- Respect residency policies and track resource lifecycle metrics so caches stay healthy.
