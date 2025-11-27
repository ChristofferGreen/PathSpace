# PathSpace — New AI Onboarding

_Last updated: October 27, 2025 (shutdown refresh)_

Welcome! This repository just transitioned away from a previous assistant. The notes below get a fresh AI agent productive quickly while we stabilize the hand-off.

## 1. Immediate Checklist

1. **Sync & Branching**
   - `git fetch origin`
   - Create a topic branch from `origin/master` (`feat/<topic>` or `fix/<topic>` per Conventional Commits).

2. **Read-before-you-touch**
   - `docs/AI_Architecture.md` (legacy, but now annotated with hand-off notes).
   - `docs/Plan_SceneGraph.md` — check Phase 7 for the freshly completed Metal streaming work and remaining GPU milestones.
   - `docs/AI_Todo.task` — verify priority ordering and align new work with open items.
   - `docs/Widget_Schema_Reference.md` — per-widget declarative schema tables; read alongside `docs/AI_Paths.md` before touching widget namespaces.
   - `docs/WidgetDeclarativeAPI.md` — declarative runtime workflow (LaunchStandard/App/Window/Scene helpers, handler registry, readiness guard, paint/history helpers, testing discipline). Consult this before modifying declarative widgets or samples.
   - **Important:** The declarative runtime is the supported UI surface. Legacy imperative builders remain only for compatibility work; do not add new features there unless you are migrating an unmigrated consumer and documenting the follow-up plan to remove it.
   - **UITest update (Nov 27, 2025):** The legacy `tests/ui/test_Builders.cpp` suite has been retired. Declarative coverage for widgets/focus/dirty hints now lives in `tests/ui/test_DeclarativeWidgets.cpp`, `tests/ui/test_WidgetEventTrellis.cpp`, `tests/ui/test_DeclarativeSceneLifecycle.cpp`, and the other declarative UITests mentioned throughout this guide. Treat any historical references to the old file name as pointers to the new declarative suites.
   - **Deprecation telemetry:** Every legacy builder entry reports to `/_system/diagnostics/legacy_widget_builders/<entry>/`. The toolchain already exports `PATHSPACE_LEGACY_WIDGET_BUILDERS=error` (plus `PATHSPACE_LEGACY_WIDGET_BUILDERS_REPORT=<build>/legacy_builders_usage.jsonl`) inside `scripts/compile.sh`, the local pre-push hook, and CI so accidental usage fails immediately and leaves JSON artifacts. UI doctests that intentionally cover the legacy API auto-install `SP::UI::LegacyBuilders::ScopedAllow` based on their `tests/ui/*` source paths—telemetry still records their activity, but the guard prevents the hard failure.

3. **Build/Test Baseline**
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j
   ./scripts/compile.sh --loop=5 --per-test-timeout 20
   ```

4. **Verify Environment Flags**
   - `./scripts/compile.sh` now enables Metal presenter tests by default (builds with `PATHSPACE_UI_METAL=ON` and runs the Metal UITests). Use `--disable-metal-tests` only when the host lacks a compatible GPU.
   - Manual command line runs still respect `PATHSPACE_ENABLE_METAL_UPLOADS=1`; unset it when you explicitly want the software fallback.
   - Use `PATHSPACE_UI_DAMAGE_METRICS=1` only when collecting diagnostics; disable for perf runs.

5. **Quick Test Pass (recommended)**
   ```bash
   ./build/tests/PathSpaceTests
   ./build/tests/PathSpaceUITests
   ./build/tests/PathSpaceUITests --test-case "PathSurfaceMetal integrates with ObjC++ presenter harness"
   ```
   The targeted Metal UITest ensures the GPU bridge stays healthy after the latest ObjC++ harness updates.
  - **Visual sanity check:** `./build/widgets_example --screenshot /tmp/widgets_gallery.png` now runs headless, drives a scripted slider drag, renders once, and writes a PNG. Use this for deterministic focus/highlight checks even on hosts without a GUI session. Pair it with `./build/paint_example --screenshot /tmp/paint_demo.png` to capture the declarative paint surface after its scripted brush replay and compare art-direction tweaks via PNG diffs. All declarative demos call `PathSpaceExamples::ensure_declarative_scene_ready` before presenting, so you can assume lifecycle metrics + scene structure are fully populated before these screenshots fire.

## 2. Current Priorities (October 21, 2025)

| Area | Action | Notes |
| --- | --- | --- |
| Metal renderer | ✅ Completed (October 20, 2025) — material/shader bindings now flow through the shared descriptor cache | `PathRenderer2DMetal` covers rects, rounded rects, text quads, and images (see Phase 7); continue tracking glyph/material parity on the descriptor cache. |
| Diagnostics | ✅ Completed (October 20, 2025) — dashboards consume `textureGpuBytes`/`resourceGpuBytes` plus residency ratios/status under `diagnostics/metrics/residency` | Coordinate with tooling owners before schema changes. |
| Input & hit testing | ✅ Completed (October 21, 2025) — keyboard/gamepad focus navigation now rides the 5× UITest loop (`tests/ui/test_Builders.cpp`) alongside z-ordered hits, focus routing, and auto-render wake latency | Continue monitoring wait/notify latency metrics; extend coverage when new interaction paths land. |
| Widgets | ✅ Completed (October 23, 2025) — tree view widget (metadata, bindings, reducers, UITests) landed alongside the bindings fuzz harness | UITest coverage lives in `tests/ui/test_Builders.cpp`; fuzz harness in `tests/ui/test_WidgetReducersFuzz.cpp`. Keep doctests/examples in sync when adding new widget kinds or op verbs. |
| Widgets focus automation | ✅ Completed (October 27, 2025) — widget bindings now drive focus updates directly, and gallery/example code relies on the shared automation | Pointer presses, slider drags, list/tree selection, and toggle activations now call `WidgetFocus::Set` under the hood. Applications no longer need to mirror focus state manually; use the bindings' state reads when refreshing UI previews. |
| HTML tooling | ✅ Completed (October 22, 2025) — quickstart/troubleshooting note in `docs/HTML_Adapter_Quickstart.md`; extend harness coverage when new asset fields appear | Legacy serializer removed; HSAT is mandatory. |

## 3. Communication & Handoff Hygiene

- Annotate every modified doc with a short hand-off note (keep the pattern used during this transition).
- Document open questions or blockers in the PR body and, when relevant, in `docs/Plan_SceneGraph.md`.
- Use the local pre-push hook (`scripts/git-hooks/pre-push.local.sh`) or `SKIP_LOOP_TESTS=1` only with maintainer approval.

## 4. Reference Index

| Document | Purpose |
| --- | --- |
| `docs/AI_Architecture.md` | Core PathSpace architecture (paths, trie, concurrency). Archived but cross-referenced. |
| `docs/AI_Paths.md` | Canonical path layout and namespace conventions. |
| `docs/Widget_Schema_Reference.md` | Declarative widget namespace + per-widget node tables. |
| `docs/Plan_SceneGraph_Renderer.md` | Renderer and presenter plan, including snapshot semantics. |
| `docs/Plan_SceneGraph.md` | Phase tracker with latest renderer/diagnostics updates. |
| `docs/AI_Debugging_Playbook.md` | Loop test expectations, log locations, and diagnostics tooling. |
| `docs/AI_Todo.task` | Structured backlog (P1/P2) with acceptance criteria. |
| `docs/Widget_Contribution_Quickstart.md` | Checklist for authoring new widgets (paths, reducers, themes, tests). |

## 5. Ready to Work?

- Confirm the build/test loop passes locally (see the quick test pass above).
- Align your planned work with an entry in `docs/AI_Todo.task` (add one if missing).
- Announce the scope in your PR description and keep doc updates synchronized with code changes. Remember to run `ctest -R HtmlCanvasVerify` when touching the adapter or HTML outputs so the headless replay harness stays green.
- When you spot a gap in test coverage, either add the test immediately or log a follow-up in `docs/Plan_SceneGraph.md` / `docs/AI_Todo.task` so the need is visible to the next maintainer.

### Latest Highlights (October 27, 2025)
- `widgets_example` can now capture its own gallery with `./build/widgets_example --screenshot <path>`, making visual verification reproducible even inside scripted runs (October 27, 2025). The command runs headless, performs a scripted slider drag, writes a PNG, and exits automatically.
- Widget theme hot-swap coverage landed in `tests/ui/test_Builders.cpp` (“Widgets::WidgetTheme hot swap repaints button scenes and marks dirty”), exercising default vs sunset palettes in-place and confirming scene/state revisions update cleanly (5× loop, October 23, 2025).
- Widget session capture tooling ships via `scripts/record_widget_session.sh` and `scripts/replay_widget_session.sh`; `SP::UI::WidgetTrace` now backs the flow so other apps can opt in by including `pathspace/ui/WidgetTrace.hpp` and wiring their own env vars (widgets_example still uses `WIDGETS_EXAMPLE_TRACE_RECORD` / `WIDGETS_EXAMPLE_TRACE_REPLAY`) (October 28, 2025).
- Widget builders now publish canonical idle/hover/pressed/disabled scenes under `scenes/widgets/<id>/states/*`; live scenes republish automatically when state changes, and doctests cover the new snapshots (October 21, 2025).
- Metal renderer is now material-aware: `PathRenderer2DMetal` consumes shared descriptors via `bind_material`, GPU frames stay in lock-step with software telemetry, and a new blending UITest asserts pipeline parity (`PathRenderer2DMetal honors material blending state`).
- Scene hit testing now returns z-ordered hit stacks (`HitTestResult::hits`) with bounded drill-down via `HitTestRequest::max_results`; doctests cover overlap, clipping, and limit cases (October 21, 2025).
- Widget focus navigation helpers (`Widgets::Focus`) maintain `widgets/focus/current`, toggle widget highlight states across button/toggle/slider/list types using `meta/kind`, and enqueue auto-render events for keyboard/gamepad traversal (October 21, 2025).
- `widgets_example` now renders the API-provided focus outline (widgets scenes append the highlight when `Widgets::Focus` tags `state.focused`), so keyboard/pointer traversal shows the canonical overlay without bespoke demo code (October 24, 2025).
- Widget UITests now render button/toggle/slider/list state goldens and replay hover/press/disabled sequences; `PATHSPACE_UPDATE_GOLDENS=1` refreshes the fixtures when intentional changes land (October 21, 2025).
- UITest coverage exercises Tab/Shift+Tab and gamepad focus hops through the widget order, asserting focus state updates and `focus-navigation` auto-render scheduling in `tests/ui/test_Builders.cpp` (October 21, 2025).
- Paint demo ships with a `--metal` flag that selects the Metal2D backend and auto-enables uploads for developers; software remains the default for CI.
- `./scripts/compile.sh` always builds with Metal support enabled and runs the Metal UITests unless `--disable-metal-tests` is passed. This keeps the GPU path green by default on macOS hosts.
- HTML adapter parity tests landed, so DOM and canvas command streams stay lock-step with the renderer geometry.
- Residency metrics are live under `diagnostics/metrics/residency/*`; dashboards now read the published ratios/status fields—extend telemetry when new counters appear.
- Presenter telemetry mirrors into window diagnostics sinks under `windows/<window>/diagnostics/metrics/live/views/<view>/present`, so dashboards can ingest present stats without crawling renderer targets (October 21, 2025).
- Widgets gallery supports keyboard focus (Tab/Shift+Tab) and logs reducer actions on every frame, making it easier to validate bindings without auxiliary tooling (October 21, 2025).
- Hit-test auto-render scheduling now has latency coverage (`tests/ui/test_SceneHitTest.cpp`), ensuring the wait/notify path wakes within the 20–200 ms budget (October 21, 2025).
- Widget binding helpers (`Widgets::Bindings::Dispatch{Button,Toggle,Slider,List}`) emit dirty rect hints, auto-schedule renders, and enqueue ops under `widgets/<id>/ops/inbox/queue` so reducers can react without republishing entire scenes.
- List widget builder (`Builders::Widgets::CreateList`) plus `UpdateListState` and `DispatchList` land with doctest coverage, enabling selection/hover/scroll ops and expanding `widgets_example`.
- List preview layout + label placement now flow through `Widgets::BuildListPreview`; widgets_example consumes the helper for gallery previews, leaving the API responsible for row bounds and sanitized style/state (October 27, 2025).
- Stack layout preview metrics now come from `Widgets::BuildStackPreview`; the gallery delegates spacing/padding math to the API so new apps can reuse the computed bounds instead of hand-rolled geometry (October 27, 2025).
- Tree preview bounds/toggle regions now come from `Widgets::BuildTreePreview`; the demo consumes the shared layout metadata and only renders labels, so future apps can rely on the API for indentation, hover, and hit regions (October 27, 2025).
- `Builders::App::Bootstrap` wires a renderer/surface/window + present policy for a scene in one call, trimming boilerplate in examples/tests (October 21, 2025).
- `Builders::App::UpdateSurfaceSize` and `Builders::App::PresentToLocalWindow` now own LocalWindow resize/present scaffolding; widgets_example, pixel_noise_example, and paint_example consume the helpers instead of bespoke loops (October 23, 2025).
- Reducer helpers (`Widgets::Reducers::ReducePending`/`PublishActions`) drain widget ops into `ops/actions/inbox/queue`; widgets_example seeds a sample action and prints the reducer output.
- Stroke rendering is now a first-class primitive: `DrawCommandKind::Stroke` serializes shared point buffers, `PathRenderer2D` rasterizes polylines, the HTML adapter/replay round-trip stroke data, and `paint_example` emits strokes instead of per-dab rects (October 21, 2025).

### Quit Shortcut Checklist (October 23, 2025)
- `LocalWindowBridge` now captures Command+Q (macOS), Ctrl+Q, and Alt+F4 and forwards them through `RequestLocalWindowQuit()`, closing the active window on the main thread and setting `LocalWindowQuitRequested()` for loop coordination.
- **Manual verification:** run `widgets_example`, `paint_example`, `pixel_noise_example`, and `devices_example`; trigger the shortcut for the host platform and confirm the loop exits cleanly (console should not require force termination). `pixel_noise_example` logs a quit message when the request is observed.
- **Window close parity:** using the window close affordance should also terminate the loop; `windowWillClose` sets the quit flag so pending cleanup code can poll `LocalWindowQuitRequested()`.
- **Extending shortcuts:** new UI examples should poll `LocalWindowQuitRequested()` immediately after `PollLocalWindow()`; extend shortcut detection inside `src/pathspace/ui/LocalWindowBridge.mm` whenever additional accelerators are introduced, then mirror the expectations here.

## 6. Shutdown Snapshot (October 27, 2025 @ 08:57 UTC)
- Latest change: `widgets_example` gained a `--screenshot <path>` mode that runs headless, performs a scripted slider drag, renders once, and writes a PNG via stb_image_write—no LocalWindow bridge required. Docs updated to note the workflow.
- Latest change: `paint_example` composites completed strokes into a persistent canvas image (PNG asset per revision) while previewing the active stroke live, keeping long painting sessions within the snapshot/renderer budgets.
- Follow-up (October 27, 2025): retained resize/present helpers (`App::UpdateSurfaceSize`, `App::PresentToLocalWindow`)—new automated capture flow leans on the same bootstrap plumbing.
- Validation: `./scripts/compile.sh --release --test --loop=5 --per-test-timeout 20` (5× PathSpaceTests, PathSpaceUITests, HtmlCanvasVerify, HtmlAssetInspect, PixelNoise harnesses) — all green after the screenshot automation landed.
- Outstanding follow-ups before resuming:
  1. Capture the pixel-noise perf harness frame grab (`images/perf/pixel_noise.png`) now that the paired baselines are checked in.
- Local worktree clean after committing the new tests/docs; no other unpublished edits.
- Next session checklist:
  1. Capture and publish the pixel-noise perf frame grab, referencing the updated baseline workflow.
  2. Keep plan/onboarding/docs in sync with any additional widget coverage or screenshot-driven diagnostic helpers.

Welcome aboard and thank you for keeping the PathSpace docs in sync for the next AI maintainer.
