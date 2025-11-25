# Declarative Widget API Workflow

_Last updated: November 25, 2025_

This guide explains how to build, test, and ship declarative widgets in PathSpace.
It complements `docs/AI_ARCHITECTURE.md`, `docs/AI_PATHS.md`, and
`docs/Widget_Schema_Reference.md` by focusing on workflow: bootstrapping the
runtime, mounting widgets, wiring handlers, validating buckets, and keeping docs
plus tests in sync. Read it before touching declarative UI code or examples.

## 1. Prerequisites
- Skim `docs/Plan_WidgetDeclarativeAPI.md` plus the renderer plans
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
2. `SP::App::Create` returns the canonical app root and registers renderer/theme
   defaults. `SP::Window::Create` mounts the window under the app, seeds
   `views/<view>/{scene,surface,renderer}` bindings, and registers device
   subscriptions at `/system/widgets/runtime/windows/<token>`.
3. `SP::Scene::Create` wires the window to the scene and spawns the lifecycle
   worker that rebuilds buckets whenever widgets flip `render/dirty`.
4. Always call `PathSpaceExamples::ensure_declarative_scene_ready(...)` (from
   `examples/declarative_example_shared.hpp`) before running a UI loop or taking
   screenshots. It waits for lifecycle metrics, scene widgets, and snapshot
   buckets so windows never flash empty frames.
5. Shut down with `SP::System::ShutdownDeclarativeRuntime(space)` in tests to
   avoid leaking the worker threads into later suites.

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
| Scene Lifecycle | `<app>/scenes/<scene>/runtime/lifecycle` | Watches `render/events/dirty`, rebuilds descriptors via `RenderDescriptor`, caches buckets under `scene/structure/widgets/<widget>/render/bucket`, and publishes revisions via `SceneSnapshotBuilder`. |
| Paint GPU uploader | `/system/widgets/runtime/paint_gpu` | Rasterizes stroke history into `assets/texture`, tracks dirty rectangles, and logs upload failures before falling back to CPU buckets. |
| Focus controller | `<app>/widgets/focus/current` + per-window mirrors | Depth-first traversal order is recomputed whenever widgets mount or are removed; wrapping per-container traversal can be disabled by setting `focus/wrap = false` on the container root. |

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
   PathSpaceExamples::run_present_loop(space, app, window, scene, loop_config);
   ```
5. Take screenshots or run GPU smoke tests via the shared CLI wrappers (`paint_example`,
   `pathspace_screenshot_cli`) so readiness gates, overlay hooks, and telemetry stay uniform.

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
   `scenes/widgets/<name>/states/*` still pass in `tests/ui/test_Builders.cpp`,
   and capture new paint/widget screenshots if the layout changes.
6. Keep `docs/AI_Paths.md`, `docs/Widget_Schema_Reference.md`, and this guide in
   sync with any new path nodes, telemetry leaves, or readiness requirements.

## 8. Troubleshooting
- **Empty window / missing buckets**: ensure you called
  `PathSpaceExamples::ensure_declarative_scene_ready`, check
  `scene/runtime/lifecycle/log/events`, and verify widgets are flipping
  `render/dirty` when state changes.
- **Handlers not firing**: inspect `widgets/<id>/metrics/handlers/*` and
  `widgets/<id>/log/events`. Use `Widgets::Handlers::Read` to confirm the
  registry entry exists. Missing bindings often trace back to fragments mounted
  without `Widgets::Mount`.
- **Screenshot diffs**: run
  `scripts/check_paint_screenshot.py --tags 1280x800 paint_720 paint_600` to
  compare against the baselines listed in `docs/images/paint_example_baselines.json`.
  Logs land in `build/test-logs/paint_example/` with overlay diagnostics.
- **Input stalls**: check `/system/widgets/runtime/input/metrics` for growing
  backlog or handler latency; use `PATHSPACE_TEST_TIMEOUT` env to reproduce
  compile-loop conditions.

## 9. Keeping the Guide Current
- Whenever declarative widgets gain new namespaces, telemetry, runtime flags, or
  readiness requirements, update this file in the same PR.
- Cross-link new sections from `docs/AI_Onboarding_Next.md`,
  `docs/Widget_Contribution_Quickstart.md`, and `docs/Plan_WidgetDeclarativeAPI.md`
  so maintainers know the guide exists.
