# Widget Contribution Quickstart

_Last updated: October 23, 2025 (post App::Bootstrap coverage)_

This guide captures the minimum contract for shipping a new widget in the PathSpace
UI stack. Follow it before touching the widget builders so the PathSpace trie
layout, interaction queues, reducers, and diagnostics stay aligned across
examples, tests, and tooling.

## Prerequisites
- Read `docs/Plan_SceneGraph_Implementation.md` (Phase 8), `docs/AI_Paths.md`
  (widgets section), and skim existing builder code in
  `src/pathspace/ui/WidgetBuildersCore.cpp`.
- Build the Release tree (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`)
  and keep `cmake --build build -j` handy while iterating.
- Plan to run the full 15× UI loop before committing:
  `ctest --test-dir build --output-on-failure -j --repeat-until-fail 15 --timeout 20`.

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
| `widgets/<id>/authoring/<component>` | Builder | Names tagged into authoring ids (`/authoring/…`) so `Widgets::ResolveHitTarget` can map hit-tests back to widget roots. |
| `widgets/<id>/ops/inbox/queue` | Bindings | FIFO of `WidgetOp` events (hover/press/etc.). |
| `widgets/<id>/ops/actions/inbox/queue` | Reducers | Reduced `WidgetAction` queue for app consumption. |
| `<target>/hints/dirtyRects` | Bindings | Dirty rect hints emitted during interaction to limit redraw. |
| `<target>/events/renderRequested/queue` | Bindings | Auto-render events when interactions need an immediate present. |
| `<app-root>/widgets/focus/current` | Focus helper | Tracks the currently focused widget path. |

Keep `docs/AI_Paths.md` in sync if you introduce new metadata keys or queues.

## Implementation Checklist

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
- Implement `Create<Widget>Binding` and `Dispatch<Widget>` in
  `WidgetBindings.cpp`, making sure dirty rect hints and auto-render scheduling
  respect the new widget’s footprint.

### 4. Update reducers and actions
- Translate new ops into reducer actions inside
  `src/pathspace/ui/WidgetReducers.cpp` and, if necessary, extend
  `Widgets::Reducers::WidgetAction`.
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

### 7. Examples, docs, and onboarding
- Add the widget to `examples/widgets_example.cpp`, including interaction hooks
  and display text.
- Update `docs/Plan_SceneGraph_Implementation.md` (Phase 8 status list) and
  `docs/AI_Onboarding_Next.md` to mention the new widget and any follow-on work.
- If new queues or metadata land, refresh `docs/AI_Debugging_Playbook.md` with
  inspection tips.
- When filing interaction regressions, attach traces generated via
  `scripts/record_widget_session.sh` / `scripts/replay_widget_session.sh` so the
  next maintainer can replay the flow headlessly.

## Testing Checklist
- Rebuild: `cmake --build build -j`.
- Update/record goldens in `tests/ui/test_Builders.cpp` (state snapshots) and
  ensure Html replays stay deterministic (`node`-backed `HtmlCanvasVerify`).
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
- Update `docs/Plan_SceneGraph_Implementation.md` to flip the relevant bullet
  (Phase 8) and capture follow-ups.
- Mirror the change in `docs/AI_Onboarding_Next.md` so the next maintainer sees
  the fresh coverage status.
- Ensure `docs/AI_Paths.md` reflects any new widget metadata or queues.
- Keep this quickstart current whenever you add new widget kinds, metadata
  fields, or change the binding/reducer flow.
