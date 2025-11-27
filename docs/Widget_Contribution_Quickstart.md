# Widget Contribution Quickstart

_Last updated: October 23, 2025 (post App::Bootstrap coverage)_

This guide captures the minimum contract for shipping a new widget in the PathSpace
UI stack. Follow it before touching the widget builders so the PathSpace trie
layout, interaction queues, reducers, and diagnostics stay aligned across
examples, tests, and tooling.

> **Declarative-first policy (November 25, 2025):** All new widgets, samples, and tests must use the declarative runtime (`include/pathspace/ui/declarative/**`). The legacy imperative builders under `src/pathspace/ui/Widget*.cpp` remain in the tree for compatibility with consumers that have not migrated yet; only touch them when backporting critical fixes or deleting dead code as part of the deprecation plan documented in `docs/Plan_WidgetDeclarativeAPI.md`.

> **Deprecation telemetry:** Legacy builder usage now increments counters under
> `/_system/diagnostics/legacy_widget_builders/<entry>/`. The default enforcement
> mode is `PATHSPACE_LEGACY_WIDGET_BUILDERS=warn`; set it to `error` locally and
> in CI once your branch stays clean so regressions fail fast. The shared status
> block (`/_system/diagnostics/legacy_widget_builders/status/*`) publishes the
> support-window deadline (February 1, 2026) and the authoritative plan link.

## Prerequisites
- Read `docs/Plan_SceneGraph.md` (Phase 8), `docs/AI_Paths.md`
  (widgets section), and—only when diagnosing compatibility bugs—skim the legacy builder code in
  `src/pathspace/ui/WidgetBuildersCore.cpp`.
- Keep `docs/Widget_Schema_Reference.md` nearby; it mirrors the declarative schema headers so you can confirm every node you author.
- Build the Release tree (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`)
  and keep `cmake --build build -j` handy while iterating.
- Plan to run the full 5× UI loop before committing:
  `ctest --test-dir build --output-on-failure -j --repeat-until-fail 5 --timeout 20`.
- Read `docs/WidgetDeclarativeAPI.md` for the declarative runtime workflow (LaunchStandard/App/Window/Scene helpers, fragment mounting, handler registry, readiness guard, paint/history bindings, and testing discipline). Treat it as the operational playbook for declarative widgets and keep it updated when behaviors change.
- Runtime bootstrap tip (November 17, 2025): `SP::System::LaunchStandard`, `SP::App::Create`, `SP::Window::Create`, and `SP::Scene::Create` now seed `/config/theme`, `/config/renderer/default`, `windows/<id>/views/<view>/{scene,surface,renderer}`, and `scenes/<id>/structure/window/<window>` automatically and start both the `/system/widgets/runtime/input` worker and the `/system/widgets/runtime/io` pump by default. Window creation also registers `/system/widgets/runtime/windows/<token>` entries so bridges can populate `subscriptions/{pointer,button,text}/devices` (device paths such as `/system/devices/in/pointer/default`). Call `SP::System::ShutdownDeclarativeRuntime` in short-lived tests/examples when you do not want either worker thread to survive `PathSpace` teardown.

### Declarative samples (November 19, 2025)
- `examples/widgets_example.cpp` now mounts the full gallery via the declarative API (`Button`, `Toggle`, `Slider`, `List`, `Tree`, `Label`), forwards `LocalWindowBridge` events through `/system/devices/in/{pointer,text}/default`, and uses `Builders::App::PresentToLocalWindow` to present the runtime-generated buckets. The scaffolding lives in `examples/declarative_example_shared.hpp` so future demos can reuse the LaunchStandard → window bootstrap → device subscription flow without rebuilding `App::Bootstrap`.
- `examples/widgets_example_minimal.cpp` is the stripped-down variant (button + slider + status label) built on the same helper; use it when you need a compact repro of declarative widget plumbing.
- `examples/declarative_hello_example.cpp` is the literal quickstart: it creates an app, window, button, list, and label via `SP::UI::Declarative::*`, registers pointer/text devices, and runs the standard present loop. Point new contributors at this sample when they need the smallest runnable declarative app.
- All declarative demos share the helper header. When you add a new sample, include `"declarative_example_shared.hpp"`, call `SP::UI::Declarative::BuildPresentHandles` after `Window::Create`, register the pointer/text device you care about via `subscribe_window_devices`, and wrap the present loop with `run_present_loop`. Before presenting (or kicking off screenshot flows), call `PathSpaceExamples::ensure_declarative_scene_ready` so lifecycle metrics, scene structure, and the first snapshot are populated; this is now required for every demo so we never flash an empty window.

## Widget Data Contract (must stay in sync)

| Path | Owner | Purpose |
| --- | --- | --- |
| `scenes/widgets/<name>` | Builder | Immutable widget scene root published by the snapshot builder. |
| `scenes/widgets/<name>/states/{idle,hover,pressed,disabled}` | Builder | Canonical per-state snapshots reused by bindings, focus helpers, and goldens. |
| `widgets/<id>/state` | Builder/Bindings | Live state payload (struct per widget type). |
| `widgets/<id>/meta/kind` | Builder | Lowercase widget identifier consumed by focus + inspectors. |
| `widgets/<id>/meta/label` | Builder | Accessibility label / display text (buttons, toggles, etc.). |
| `widgets/<id>/meta/style` | Builder | Theme-derived style blob for the widget. |
| `widgets/<id>/meta/range` | Builder | Slider-specific range metadata (min/max/step). |
| `widgets/<id>/meta/items` | Builder | List item vector (id + display text). |
| `widgets/<id>/meta/nodes` | Builder | Tree view node metadata (id, parent, label, enabled, expandable, loaded). |
| `widgets/<id>/authoring/<component>` | Builder | Names tagged into authoring ids (`/authoring/…`) so `Widgets::ResolveHitTarget` can map hit-tests back to widget roots. |
| `widgets/<id>/ops/inbox/queue` | Bindings | FIFO of `WidgetOp` events (hover/press/etc.). |
| `widgets/<id>/ops/actions/inbox/queue` | Reducers | Reduced `WidgetAction` queue for app consumption. |
| `<target>/hints/dirtyRects` | Bindings | Dirty rect hints emitted during interaction to limit redraw. |
| `<target>/events/renderRequested/queue` | Bindings | Auto-render events when interactions need an immediate present. |
| `<app-root>/widgets/focus/current` | Focus helper | Tracks the currently focused widget path. |

Keep `docs/AI_Paths.md` in sync if you introduce new metadata keys or queues.

## Implementation Checklist

The steps below describe the declarative workflow. When legacy builder files are mentioned, treat them as reference only—the actual widget behaviour should be authored via `SP::UI::Declarative::*` helpers and their associated runtime services.

### 1. Publish the canonical scene
- Extend `src/pathspace/ui/WidgetBuildersCore.cpp` and helper inlines
  (`WidgetDrawablesDetail.inl`, `WidgetMetadataDetail.inl`) to author the new
  widget scene, state snapshots, and metadata writes.
- Ensure all authored drawables embed the `/authoring/` marker so hit tests can
  recover the widget root (`kWidgetAuthoringMarker` in
  `src/pathspace/ui/BuildersDetail.hpp`).
- Update `scenes/widgets/<name>/states/*` snapshots via the builder helpers and
  confirm they appear under the app root (check with `PathSpaceUITests`).

### 2. Wire metadata & accessibility
- Populate `meta/kind`, `meta/label`, and any widget-specific metadata
  (`meta/range`, `meta/items`, etc.) alongside `widgets/<id>/state`.
- Keep labels human-readable; focus helpers read `meta/label` to announce
  widgets and to derive default hover/selection text.
- Update `docs/AI_Paths.md` if you add new `meta/*` leaves.

### 3. Extend bindings and op kinds
- Add a `WidgetOpKind` entry in `include/pathspace/ui/Builders.hpp` if the new
  widget needs additional interaction verbs. Update the switch statements and
  random selection logic in `src/pathspace/ui/WidgetBindings.cpp`,
  `tests/ui/test_WidgetReducersFuzz.cpp`, and `examples/widgets_example.cpp`.
  Current operations cover button/toggle/slider/list/tree widgets; mirror the
  tree view pattern (`TreeHover`, `TreeSelect`, `TreeToggle`, `TreeExpand`,
  `TreeCollapse`, `TreeRequestLoad`) when adding new interaction verbs.
- Implement `Create<Widget>Binding` and `Dispatch<Widget>` in
  `WidgetBindings.cpp`, making sure dirty rect hints and auto-render scheduling
  respect the new widget’s footprint. Bindings must supply the exact footprint
  rectangle captured during layout (body + captions/chrome) without padding or
  cross-widget union. The renderer now owns tile selection and neighbouring
  damage coalescing.

### 4. Update reducers and actions
- Translate new ops into reducer actions inside
  `src/pathspace/ui/declarative/Reducers.cpp` and, if necessary, extend
  `SP::UI::Declarative::Reducers::WidgetAction` (the legacy
  `src/pathspace/ui/WidgetReducers.cpp` file now just forwards to the
  declarative helpers after running the guard).
- Confirm reducers publish actions to `widgets/<id>/ops/actions/inbox/queue`
  and that apps/examples drain the queue (see `widgets_example.cpp` for patterns).

### 5. Focus navigation & accessibility
- Teach `Widgets::Focus` about the new widget kind in
  `src/pathspace/ui/WidgetFocus.cpp` (update the `determine_widget_kind`
  helper and focus state transitions).
- Verify focus updates mark widgets dirty and enqueue auto-render events.

### 6. Theme integration
- Extend `Widgets::WidgetTheme` in `include/pathspace/ui/Builders.hpp` plus
  implementations of `MakeDefaultWidgetTheme`, `MakeSunsetWidgetTheme`, and
  `ApplyTheme` in `WidgetBuildersCore.cpp`.
- Update `examples/widgets_example.cpp` so theme swaps (default vs `sunset`)
  restyle the new widget.
- **Update (November 19, 2025):** `examples/declarative_theme_example.cpp` seeds
  sunrise/sunset themes entirely through `SP::UI::Declarative::Theme::{Create,SetColor}`
  and toggles actives to show inheritance; mirror that flow when wiring new
  declarative widgets, and keep token semantics in sync with
  `tests/ui/test_DeclarativeTheme.cpp`.

### 7. Examples, docs, and onboarding
- Add the widget to `examples/widgets_example.cpp` (and the corresponding
  minimal + hello samples) via the declarative API, including interaction hooks
  and display text. Follow the new helper flow: include
  `declarative_example_shared.hpp`, register pointer/text devices, mount the
  widget under the window root, and let the shared present loop drive
  `Window::Present`.
- Point declarative contributors at `declarative_theme_example` when documenting
  theme overrides so the runtime-focused sample stays current.
- Update `docs/WidgetDeclarativeAPI.md` whenever workflow, readiness, or
  testing expectations change so contributors have a single operational guide.
- Update `docs/Plan_SceneGraph.md` (Phase 8 status list) and
  `docs/AI_Onboarding_Next.md` to mention the new widget and any follow-on work.
- If new queues or metadata land, refresh `docs/AI_Debugging_Playbook.md` with
  inspection tips.
- When filing interaction regressions, attach traces generated via
  `scripts/record_widget_session.sh` / `scripts/replay_widget_session.sh` so the
  next maintainer can replay the flow headlessly. The capture helper is now
  exposed as `SP::UI::WidgetTrace` (`pathspace/ui/WidgetTrace.hpp`) if you need
  to integrate tracing into additional demos or UITests with custom env vars.

## Testing Checklist
- Rebuild: `cmake --build build -j`.
- Update/record goldens in `tests/ui/test_DeclarativeWidgets.cpp`
  (state snapshots + descriptor parity) and ensure Html replays stay
  deterministic (`node`-backed `HtmlCanvasVerify`).
- Exercise fuzz + reducers: `tests/ui/test_WidgetReducersFuzz.cpp`.
- Run the full loop: `ctest --test-dir build --output-on-failure -j --repeat-until-fail 15 --timeout 20`.
- If the widget affects Metal uploads or HTML output, re-run the optional
  presenters: `./scripts/compile.sh --enable-metal-tests --test --loop=1`.
- Capture a trace with `scripts/record_widget_session.sh` for any noteworthy UI
  flows and verify it replays cleanly with `scripts/replay_widget_session.sh`
  before sharing bug reports or repro steps.

## Diagnostics & Observability
- Confirm bindings emit dirty rects and auto-render events by inspecting
  `<target>/hints/dirtyRects` and `<target>/events/renderRequested/queue`.
- Check widget telemetry in `widgets_example` (stdout logs list reducer actions
  every frame) and ensure residency metrics remain within budgets.
- Validate HTML parity via `HtmlCanvasVerify` so DOM/canvas replays render the
  new widget correctly.

## Hand-off Requirements
- Update `docs/Plan_SceneGraph.md` to flip the relevant bullet
  (Phase 8) and capture follow-ups.
- Mirror the change in `docs/AI_Onboarding_Next.md` so the next maintainer sees
  the fresh coverage status.
- Ensure `docs/AI_Paths.md` reflects any new widget metadata or queues.
- Keep this quickstart current whenever you add new widget kinds, metadata
  fields, or change the binding/reducer flow.
