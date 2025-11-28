# Declarative Widget API Workflow

_Last updated: November 26, 2025_

This guide explains how to build, test, and ship declarative widgets in PathSpace.
It complements `docs/AI_ARCHITECTURE.md`, `docs/AI_PATHS.md`, and
`docs/Widget_Schema_Reference.md` by focusing on workflow: bootstrapping the
runtime, mounting widgets, wiring handlers, validating buckets, and keeping docs
plus tests in sync. Read it before touching declarative UI code or examples.

## 1. Prerequisites
- Skim `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md` plus the renderer plans
  (`docs/Plan_SceneGraph_Renderer.md`, `docs/Plan_SceneGraph.md`).
- Make sure the Release build tree exists: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`.
- Keep `cmake --build build -j` and `./scripts/compile.sh --clean --test --loop=5 --release`
  handy; the 5× loop is mandatory for any code change that touches runtime or
  samples.
- Familiarize yourself with `examples/declarative_example_shared.hpp`; every
  sample derives from this helper.

## 2. Bootstrapping the Runtime
1. `SP::System::LaunchStandard(space, launch_options)` seeds `/system/themes`,
   `/system/widgets/runtime/*` workers (input, IO pump, widget event trellis),
   renderer targets, and telemetry roots. Disable individual workers only when
   writing targeted tests.
2. Include `<pathspace/ui/PathTypes.hpp>` when you need canonical handles such as
   `SP::UI::WindowPath` or `SP::UI::ScenePath`. The header re-exports the common
   aliases from declarative code so you no longer have to depend on
   `SP::UI::Runtime::*` just to store window/scene/widget paths.
3. Include `<pathspace/ui/WidgetSharedTypes.hpp>` whenever you need the shared
   widget data (`WidgetTheme`, style structs, stack enums, handler payloads,
   `DirtyRectHint`, `WidgetAction`, etc.) without dragging in the rest of
   `runtime/UIRuntime.hpp`/`WidgetSharedTypes.hpp`. Declarative headers/tests now pull from these headers so you can
   manipulate widget data and telemetry independently of the legacy builder
   functions.
4. Include `<pathspace/ui/runtime/RenderSettings.hpp>` when you need renderer
   knobs outside of legacy builders. The header exposes
   `SP::UI::Runtime::{RendererKind,RenderSettings,DirtyRectHint}` so renderer
   helpers, presenter code, and tests can depend on the runtime namespace
   directly. `Builders.hpp` simply aliases the runtime types for compatibility,
   so new code should prefer the runtime header.
2. `SP::App::Create` returns the canonical app root and registers renderer/theme
   defaults. `SP::Window::Create` mounts the window under the app, seeds
   `views/<view>/{scene,surface,renderer}` bindings, and registers device
   subscriptions at `/system/widgets/runtime/windows/<token>`.
3. `SP::Scene::Create` wires the window to the scene and spawns the lifecycle
   worker that rebuilds buckets whenever widgets flip `render/dirty`.
4. Always call `PathSpaceExamples::ensure_declarative_scene_ready(...)` (from
   `examples/declarative_example_shared.hpp`) before running a UI loop or taking
   screenshots. The helper now accepts overrides for the scene window component
   and view (`scene_window_component_override`, `scene_view_override`,
   `ensure_scene_window_mirror`) so doctests with custom naming can monitor the
   exact `/scenes/<scene>/structure/widgets/windows/<window>/views/<view>/widgets`
   subtree they expect. It still waits for lifecycle metrics, scene widgets, and
   snapshot buckets so windows never flash empty frames. When tests need to
   guarantee that a scene publishes a revision before proceeding (e.g., under
   the `PATHSPACE_TEST_TIMEOUT=1` harness), set `force_scene_publish = true` in
   `DeclarativeReadinessOptions`. With `pump_scene_before_force_publish`
   (enabled by default), the helper now calls
   `SceneLifecycle::PumpSceneOnce` before each `ForcePublish` attempt so the
   lifecycle worker synchronously drains pending `render/dirty` widgets and
   populates `runtime/lifecycle/metrics/widgets_with_buckets` even on the first
   build. The forced publish reuses the caller’s `min_revision`/timeout and
   returns the revision immediately, bypassing the passive `current_revision`
   wait. Override `scene_pump_options` only when you need to tweak the pump
   timeout or force a full re-mark of the widget tree.
5. Shut down with `SP::System::ShutdownDeclarativeRuntime(space)` in tests to
   avoid leaking the worker threads into later suites.

6. Once the window + scene are mounted, call
   `SP::UI::Declarative::BuildPresentHandles(space, app_root, window.path, window.view_name)`
   to obtain a lightweight `PresentHandles` record. The handles capture the
   resolved window/view, surface, renderer, and render target paths so
   declarative callers never have to touch `Runtime::App::BootstrapResult`.
   Use `PresentHandles` with the new helpers:
   - `ResizePresentSurface` rewrites the surface/target descriptors, reapplies
     renderer settings, and submits a full-surface dirty rect so resize paths no
     longer call `Runtime::App::UpdateSurfaceSize`.
   - `PresentWindowFrame` renders into the target, records presenter/renderer
     metrics, persists the software framebuffer when requested, and returns a
     `PresentFrame` with the `PathWindowPresentStats` plus optional HTML payload
     (no `Runtime::Window::Present` dependency).
   - `PresentFrameToLocalWindow` still mirrors the legacy
     `PresentToLocalWindow` behaviour so interactive loops can blit IOSurfaces
    or CPU framebuffers without including `runtime/UIRuntime.hpp`.

   `PathSpaceExamples::run_present_loop` now consumes these handles directly,
   and the declarative screenshot helper (`SP::UI::Screenshot::CaptureDeclarative`)
   reuses the same readiness and present plumbing when it needs to read back a
   framebuffer.

### Declarative Screenshot Helper

Call `SP::UI::Screenshot::CaptureDeclarative(space, scene_path, window_path, options)`
to capture a declarative scene without writing 100+ lines of readiness and
force-publish code. `DeclarativeScreenshotOptions` accepts window/view sizing,
output/baseline/diff/metrics paths, telemetry overrides, `force_software`, and
an optional `ScreenshotRequest::Hooks` bundle for post-processing (paint
overlays, software fallback writers, etc.). The helper:

1. Ensures the window/view widgets exist via
   `PathSpaceExamples::ensure_declarative_scene_ready` with overridable
   `DeclarativeReadinessOptions`.
2. Optionally marks the scene dirty, forces a publish, and waits for the next
   revision before invoking `ScreenshotService::Capture`.
3. Infers width/height from the bound surface when the caller leaves them unset
   and auto-derives the telemetry namespace from the owning app.

Advanced callers (paint example, CLI harnesses) pass `hooks` for overlays and
baseline diffs, but simple demos only need:

```cpp
SP::UI::Screenshot::DeclarativeScreenshotOptions screenshot{};
screenshot.output_png = "build/artifacts/example.png";
screenshot.baseline_png = "docs/images/example_baseline.png";
screenshot.view_name = window.view_name;
auto result = SP::UI::Screenshot::CaptureDeclarative(space, scene.path, window.path, screenshot);
```

The helper enforces the same `SceneLifecycle` contract as the compile-loop
tests, so new demos inherit deterministic screenshot behaviour automatically.

When wiring CLIs, include `<pathspace/ui/screenshot/DeclarativeScreenshotCli.hpp>`:
`SP::UI::Screenshot::DeclarativeScreenshotCliOptions` captures the common
`--screenshot*` flags, `RegisterDeclarativeScreenshotCliOptions` wires them into
`SP::Examples::CLI::ExampleCli`, `ApplyDeclarativeScreenshotEnvOverrides`
honors `PATHSPACE_SCREENSHOT_FORCE_SOFTWARE`, and
`CaptureDeclarativeScreenshotIfRequested` bridges the parsed flags into
`CaptureDeclarative`. The widgets and devices demos share this helper, so future
samples can expose the same headless capture workflow with only a few lines of
code. `examples/declarative_hello_example` still skips the CLI helper so the
source stays identical to the onboarding quickstart, but it now honors the
environment variable `PATHSPACE_HELLO_SCREENSHOT=<png>` (and optional
`PATHSPACE_HELLO_SCREENSHOT_FORCE_SOFTWARE=1`) to call
`SP::UI::Screenshot::CaptureDeclarative` once the scene is ready. When
`Window::Present` never yields a framebuffer (common on headless hosts), the
env hook falls back to a deterministic reference render so onboarding docs
retain a screenshot without extra CLI plumbing.

`PresentWindowFrame` once again consumes `/present/params/capture_framebuffer`,
and `ScreenshotService::Capture` now always runs a present pass even when the
caller sets `force_software`, so headless/CI runs that export
`PATHSPACE_SCREENSHOT_FORCE_SOFTWARE=1` still dump the real declarative UI. The
deterministic fallback writer remains as the last resort when both hardware and
software readbacks fail.

7. Include `<pathspace/ui/declarative/ThemeConfig.hpp>` whenever you need to
   provision, load, or switch themes outside the higher-level
   `SP::UI::Declarative::Theme::{Create,SetColor,RebuildValue}` helpers.
   `ThemeConfig::{SanitizeName,Resolve,Ensure,Load,SetActive,LoadActive}` expose
   the canonical declarative entry points; the legacy
   `Runtime::Config::Theme::*` functions now simply forward to them so you no
  longer have to include `runtime/UIRuntime.hpp` to work with theme metadata.

## 3. Building Widgets Declaratively
- **Create vs. Fragment**
  - `Widget::Create(space, parent_path, name, args...)` mounts a widget at a
    deterministic path, writes initial `state/style/layout`, registers handlers
    (`events/<event>/handler = HandlerBinding{...}`), flips `render/dirty`, and
    returns the widget path.
  - `Widget::Fragment(args...)` builds a nested subtree (including handler specs
    and child fragments). Use fragments when composing containers such as
    `List`, `Tree`, `Stack`, or custom stacks used in the paint example.
- **Mounting children**
  - Containers accept `children = { {"name", Fragment(...)}, ... }` in their
    arg structs. `Widgets::Mount(space, parent_path, fragment)` remaps handler
    bindings and copies the fragment’s subtree into `children/<name>`.
  - Fragments respect parent overrides: a parent’s `events/<event>/handler`
    stays in place unless the caller explicitly replaces it via the args struct.
- **Handler lifecycle**
  - Handlers live in an in-memory registry keyed by the stored
    `HandlerBinding`. Use `Widgets::Handlers::Read/Replace/Wrap/Restore` when you
    need to inspect or override callbacks without poking raw PathSpace nodes.
  - InputTask publishes handler telemetry per widget at
    `widgets/<id>/metrics/handlers/{invoked_total,failures_total,missing_total}`
    and mirrors errors to `widgets/<id>/log/events`.
- **Focus + traversal**
  - Focus metadata is runtime-managed. Widgets keep `focus/order` and
    `focus/current` up to date, and the controller mirrors the active widget to
    `structure/window/<window>/focus/current`. Use `Widgets::Focus::Move` to
    advance focus (Tab/Shift+Tab, D-pad, shoulders); never mutate the paths
    manually.
- **History bindings + paint buffers**
  - For undoable widgets (e.g., `PaintSurface`), wrap the subtree with
    `SP::UI::Declarative::HistoryBinding`. Call `InitializeHistoryMetrics`,
    `CreateHistoryBinding`, `RecordHistoryBindingActionResult`,
    `SetHistoryBindingButtonsEnabled`, and `PublishHistoryBindingCard` to keep
    `/widgets/<id>/metrics/history_binding/*` plus the serialized card in sync.
  - Paint surfaces store stroke history under `state/history/<stroke>/{meta,points,version}`
    and buffer metrics under `render/buffer/*`. Descriptors wait for a stable
    `version` before caching buckets.

## 4. Runtime Services Cheat Sheet
| Service | Path roots | Notes |
| --- | --- | --- |
| InputTask | `/system/widgets/runtime/input` | Drains reducer actions, mirrors widget ops into `events/*/queue`, invokes handlers, records per-loop metrics (widgets processed, backlog, handler latency). |
| IO Pump | `/system/widgets/runtime/io` + `/system/widgets/runtime/windows` | Fans `/system/io/events/{pointer,button,text}` into per-window queues. Windows subscribe via `subscriptions/{pointer,button,text}/devices`. |
| Widget Event Trellis | `/system/widgets/runtime/events` | Performs hit tests per window, mutates widget state (hover, pressed, slider value, list selection, tree expand/collapse, text cursor/delete), marks `render/dirty`, and emits `WidgetOp`s. |
| Scene Lifecycle | `<app>/scenes/<scene>/runtime/lifecycle` | Watches `render/events/dirty`, rebuilds descriptors via `RenderDescriptor`, caches buckets under `scene/structure/widgets/<widget>/render/bucket`, publishes revisions via `SceneSnapshotBuilder`, and exposes `SceneLifecycle::PumpSceneOnce` for deterministic bucket synthesis before a forced publish. |
| Paint GPU uploader | `/system/widgets/runtime/paint_gpu` | Rasterizes stroke history into `assets/texture`, tracks dirty rectangles, and logs upload failures before falling back to CPU buckets. |
| Focus controller | `<app>/widgets/focus/current` + per-window mirrors | Depth-first traversal order is recomputed whenever widgets mount or are removed; wrapping per-container traversal can be disabled by setting `focus/wrap = false` on the container root. |

### Reducer helpers
- Prefer `SP::UI::Declarative::Reducers::{WidgetOpsQueue,ReducePending,PublishActions,ProcessPendingActions}` when draining widget op queues or synthesizing `WidgetAction` payloads for tests. The helpers live in `include/pathspace/ui/declarative/Reducers.hpp` and power the input task, manual pump, paint runtime, and declarative UITests so they no longer depend on the legacy builder guards.
- The legacy `SP::UI::Runtime::Widgets::Reducers::*` functions still exist for compatibility, but they now forward to the declarative helpers after running the guard. Use them only in compatibility suites while we delete the remaining legacy builder coverage.

### Text bucket synthesis
- Declarative code should include `pathspace/ui/declarative/Text.hpp` and call `SP::UI::Declarative::Text::BuildTextBucket` when labels or descriptor paths need text drawables. The helper shares the glyph shaping cache with the legacy builders but no longer drags the rest of `runtime/UIRuntime.hpp` into declarative sources.
- `include/pathspace/ui/TextBuilder.hpp` re-exports the declarative API so existing builder call sites continue to work; once legacy samples are gone we can delete the shim entirely.

### PaintSurface GPU staging
- **Enable it at mount time** — set `PaintSurface::Args::gpu_enabled = true`. The fragment helper writes `render/gpu/enabled`, seeds `render/gpu/{state,stats,fence/start,fence/end}`, clears `render/buffer/pendingDirty`, and leaves the widget in `Idle`.
- **Dirty hints + state machine** — every stroke append records a `DirtyRectHint` under both `render/buffer/pendingDirty` and `/render/gpu/dirtyRects`, bumps `render/buffer/revision`, and flips `render/gpu/state` to `DirtyPartial`. When you need a full re-rasterization (e.g., after external code resizes the buffer), set the state to `DirtyFull` before the uploader runs so the next upload increments both the per-widget `full_uploads` counter and the global `full_uploads_total` metric.
- **Layout-driven resizing** — write `layout/computed/size = {width,height}` (layout units) whenever a paint surface’s layout changes, then call `PaintRuntime::ApplyLayoutSize`. The helper resolves the owning scene (`/structure/window/<window>/metrics/dpi`), converts layout units to pixels, rewrites `render/buffer/metrics/{width,height,dpi}` plus `render/buffer/viewport`, increments the buffer revision, and enqueues a full-surface dirty hint so SceneLifecycle and the GPU uploader pick up the new bounds. `tests/ui/test_DeclarativePaintSurface.cpp` (“Paint surface layout resizing updates metrics and viewport”) keeps this flow covered, ensuring grow/shrink cycles preserve stroke history and leave the widget in `render/gpu/state = DirtyFull` when GPU staging is enabled.
- **Uploader lifecycle** — `SP::System::LaunchStandard` starts the uploader unless `LaunchOptions::start_paint_gpu_uploader` is `false`. The worker enumerates GPU-enabled paint widgets under `/system/applications/*/{windows,widgets}` each poll, drains dirty rect queues, replays stroke history via `PaintRuntime::LoadStrokeRecords`, writes the serialized `PaintTexturePayload` to `assets/texture`, and clears pending hints. Upload timings land in `widgets/<id>/render/gpu/stats` while global metrics/logs live at `/system/widgets/runtime/paint_gpu/{metrics,log/errors/queue}`.
- **Presenter coordination** — SceneLifecycle forwards `render/buffer/pendingDirty` hints into the active renderer target, so even when GPU uploads lag the CPU buckets stay in sync. Presenters bind `assets/texture` whenever `render/gpu/state == Ready`; otherwise they continue sampling the CPU buffer.
- **Diagnostics & tests** — `tests/ui/test_DeclarativePaintSurface.cpp` exercises the uploader end-to-end (waiting for `render/gpu/state` to reach `Ready`, verifying staged pixels and stats). `examples/paint_example --gpu-smoke[=png]` plus the `PaintExampleScreenshot` CTest target keep the staged texture in parity with the paint scene and fail fast if uploads fall back to the software rasterizer. Check `/system/widgets/runtime/paint_gpu/log/errors/queue` when uploads stall, and watch `widgets/<id>/render/gpu/stats.failures_total` or the global `*_metrics/failures_total` counter inside the compile-loop logs.

## 5. Example Workflow
1. Bootstrap via the helper in `examples/declarative_example_shared.hpp`:
   ```cpp
   auto space = PathSpace{};
   auto launch = SP::System::LaunchStandard(space);
   auto app = SP::App::Create(space, "demo_app");
   auto window = SP::Window::Create(space, app, "demo_window");
   auto scene = SP::Scene::Create(space, app, window);
   PathSpaceExamples::ensure_declarative_scene_ready(space, scene, window);
   ```
2. Mount widgets:
   ```cpp
   const auto status = Label::Create(space, window, "status", "Ready");
   const auto button = Button::Create(space, window, "submit", ButtonArgs{
       .label = "Submit",
       .on_press = [](ButtonContext& ctx) { Label::SetText(ctx.space, status, "Clicked"); }
   });
   ```
3. For containers:
   ```cpp
   List::Create(space, window, "menu", ListArgs{
       .layout = ListLayout::Vertical,
       .spacing = 8.0f,
       .children = {
           {"row_hello", Label::Fragment("Hello")},
           {"row_button", Button::Fragment("Action", [](ButtonContext&) { /* ... */ })},
       },
   });
   ```
4. Present:
   ```cpp
   auto handles = SP::UI::Declarative::BuildPresentHandles(space, app, window.path, window.view_name);
   if (!handles) { /* handle error */ }
   PathSpaceExamples::run_present_loop(space,
                                       window.path,
                                       window.view_name,
                                       *handles,
                                       loop_config.window_width,
                                       loop_config.window_height);
   ```
5. Take screenshots or run GPU smoke tests via the shared CLI wrappers (`paint_example`,
   `pathspace_screenshot_cli`) so readiness gates, overlay hooks, and telemetry stay uniform.
   `examples/declarative_hello_example` follows the same readiness flow but relies on the
   `PATHSPACE_HELLO_SCREENSHOT` env var instead of a CLI flag to keep the binary focused on
   the quickstart scenario, and it draws a deterministic fallback PNG if the presenter never
   produces a hardware framebuffer.

## 6. Testing & Validation
- **Build**: `cmake --build build -j`
- **Looped tests**: `./scripts/compile.sh --clean --test --loop=5 --release`
  - Honors the pre-push hook and automatically archives loop failures with logs.
  - Set `SKIP_LOOP_TESTS=1` only with explicit maintainer approval.
- **Targeted suites**:
  - `ctest --test-dir build -R PathSpaceUITests --output-on-failure`
  - `ctest --test-dir build -R Declarative --output-on-failure` (widgets,
    lifecycle, paint surface, theme tests).
  - `PaintExampleScreenshot*` + `PixelNoisePerfHarness*` tests require the
    `ui_gpu_capture` resource lock; run them serially when reproducing locally.
    When Metal capture flakes on headless hosts, set
    `PATHSPACE_SCREENSHOT_FORCE_SOFTWARE=1` (or run `pathspace_screenshot_cli
    --force-software`) to force the deterministic software fallback. Without
    the guard, the harness now fails explicitly if it had to fall back so loop
    jobs notice missing hardware captures.
- **Examples**: use the helper CLI wrappers (`examples/widgets_example`,
  `examples/paint_example`, `examples/declarative_hello_example`,
  `examples/devices_example --paint-controls-demo`) only after running the
  readiness guard.

## 7. Migration Checklist (Legacy builders → Declarative)
1. Replace ad-hoc `DrawableBucketSnapshot` code with declarative helpers. The
   runtime now synthesizes buckets from widget state/style, so app code should
   only manipulate `state/*` and handler callbacks.
2. Delete manual focus lists. Depth-first order and focus mirrors are runtime
   managed; emit `Widgets::Focus::Move` calls instead of editing
   `<app>/widgets/focus/current`.
3. Route device input through the IO pump by registering pointer/text/gamepad
   subscriptions with `subscribe_window_devices` (helper wrapper). Stop writing
   bespoke reducers that consume `/system/devices/in/*` directly.
4. Use `HistoryBinding` for widgets that need undo/redo. Legacy `UndoableSpace`
   glue should be replaced with the declarative helper so telemetry stays in
   sync and buttons enable/disable automatically.
5. Update docs/tests: whenever you migrate a widget, refresh 
   `docs/WidgetDeclarativeFeatureParity.md`, ensure state snapshots under
  `scenes/widgets/<name>/states/*` still pass in `tests/ui/test_DeclarativeWidgets.cpp`,
   and capture new paint/widget screenshots if the layout changes.
6. Keep `docs/AI_Paths.md`, `docs/Widget_Schema_Reference.md`, and this guide in
   sync with any new path nodes, telemetry leaves, or readiness requirements.

## 8. Troubleshooting
- **Empty window / missing buckets**: ensure you called
  `PathSpaceExamples::ensure_declarative_scene_ready`, check
  `scene/runtime/lifecycle/log/events`, and verify widgets are flipping
  `render/dirty` when state changes. When the lifecycle worker is throttled (or
  a fresh build hasn’t produced any buckets yet), enable
  `pump_scene_before_force_publish` or call `SceneLifecycle::PumpSceneOnce`
  directly to synchronously rebuild widget buckets before forcing a publish.
- **Handlers not firing**: inspect `widgets/<id>/metrics/handlers/*` and
  `widgets/<id>/log/events`. Use `Widgets::Handlers::Read` to confirm the
  registry entry exists. Missing bindings often trace back to fragments mounted
  without `Widgets::Mount`.
- **Screenshot diffs**: run
  `scripts/check_paint_screenshot.py --tags 1280x800 paint_720 paint_600` to
  compare against the baselines listed in `docs/images/paint_example_baselines.json`.
  Logs land in `build/test-logs/paint_example/` with overlay diagnostics.
  The CLI + Python helper both record `hardware_capture` in the metrics JSON
  and error when a fallback happens without
  `PATHSPACE_SCREENSHOT_FORCE_SOFTWARE=1`, so CI can opt into the software
  path while still surfacing unintended regressions.
- **Input stalls**: check `/system/widgets/runtime/input/metrics` for growing
  backlog or handler latency; use `PATHSPACE_TEST_TIMEOUT` env to reproduce
  compile-loop conditions.
- **Tight-loop UITests**: when `PATHSPACE_TEST_TIMEOUT` shrinks the global loop,
  call `SP::UI::Declarative::PumpWindowWidgetsOnce(space, window_path, view)`
  to synchronously drain the widgets you just mounted. Watch the per-window/app
  counters under `/system/widgets/runtime/input/windows/<token>/metrics/*` and
  `/system/widgets/runtime/input/apps/<app>/metrics/*` to prove the manual pump
  ran before asserting on reducer results—this avoids waiting for the shared
  worker to come back to your test window.

## 9. Keeping the Guide Current
- Whenever declarative widgets gain new namespaces, telemetry, runtime flags, or
  readiness requirements, update this file in the same PR.
- Cross-link new sections from `docs/AI_Onboarding_Next.md`,
  `docs/Widget_Contribution_Quickstart.md`, and `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md`
  so maintainers know the guide exists.

## 10. Deprecation & Documentation Alignment
- README, onboarding guides (`docs/AI_Onboarding*.md`), and the widget contribution
  quickstart now point here as the canonical workflow. Keep their callouts in sync
  whenever this guide changes so new contributors never land on the legacy builder
  instructions by accident.
- When legacy compatibility shims truly disappear, update `docs/WidgetDeclarativeFeatureParity.md`
  and flip the Phase 3 “documentation alignment” + Phase 4 deprecation bullets inside
  `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md`.
- If a doc still references the imperative bucket builders as the preferred
  workflow, treat that as a bug: open an issue or update it immediately as part
  of the declarative deprecation effort.

## 11. Legacy Builder Deprecation Telemetry
- Every legacy `SP::UI::Runtime::*` entry point now records usage under
  `/_system/diagnostics/legacy_widget_builders/<entry>/`:
  - `usage_total` — cumulative invocation count (per-space, so clear it before capturing a new baseline).
  - `last_entry`, `last_path`, `last_timestamp_ns` — identify the latest offending builder and widget path.
  - `status/{phase,support_window_expires,plan}` — publishes the active policy (`phase = warning`,
    `support_window_expires = 2026-02-01T00:00:00Z`, `plan = docs/finished/Plan_WidgetDeclarativeAPI_Finished.md` today).
- Runtime enforcement is driven by `PATHSPACE_LEGACY_WIDGET_BUILDERS`:
  1. `allow` — counters only, no logs or failures.
  2. `warn` — counters plus a one-time log per entry.
  3. `error` — builder calls fail with `Error::NotSupported`, propagating through the existing `std::expected` channels so CI/pre-push can block immediately.
  - `scripts/compile.sh`, `.github/workflows/ci.yml`, and `scripts/git-hooks/pre-push.local.sh` now export `PATHSPACE_LEGACY_WIDGET_BUILDERS=error` automatically so any accidental legacy usage in examples, binaries, or new tests fails fast by default.
- Set `PATHSPACE_LEGACY_WIDGET_BUILDERS_REPORT=<path>` to append JSONL diagnostics (`{"entry":"Widgets::CreateButton","path":"/system/...","timestamp_ns":...}`) for every guard hit. `compile.sh`/CI wire this up to `build/legacy_builders_usage.jsonl` so investigating regressions does not require scraping `_system/diagnostics`.
- Compatibility suites that intentionally exercised the legacy API (all `tests/ui/*`) previously ran under the same environment while the shared test reporter instantiated `SP::UI::LegacyBuilders::ScopedAllow` for those files. The guard has been removed now that the legacy builders are gone, but the telemetry/report files remain so we can spot any straggling binaries.
- Expectations before the February 1, 2026 support-window cutoff:
  - Keep the diagnostics tree at zero; CI can scrape `usage_total` to guarantee no regressions slip in.
  - Flip CI/pre-push to `PATHSPACE_LEGACY_WIDGET_BUILDERS=error` once the repo stays clean for a full validation cycle.
  - Update this guide and `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md` if the timeline or enforcement knobs change so downstream teams do not guess.
