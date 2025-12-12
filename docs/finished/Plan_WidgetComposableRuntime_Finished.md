# Plan: Widget Capsule Runtime

_Status:_ Finished - December 12, 2025  
_Owner:_ PathSpace UI Runtime Team  
_Related:_ `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md`, `docs/WidgetDeclarativeAPI.md`, `docs/finished/Plan_SceneGraph_Renderer_Finished.md`

## 1. Motivation
Declarative widgets replaced the imperative builders, but each widget still relies on shared reducers, shared lifecycle workers, and runtime-global render caches. The next refinement aims to make widgets self-contained "capsules" so that:
- The renderer can walk each widget, call a canonical lambda, and receive a deterministic `RenderPackage` without digging through runtime-specific caches.
- Widgets declare the exact event kinds they consume and receive batched messages through a mailbox rather than relying on implicit focus or reducer state.
- Complex controls such as buttons become explicit compositions of smaller primitives (surfaces, glyphs, layout containers), enabling reuse and independent upgrades.
- Instrumentation (telemetry, dirtiness, lifecycle) shrinks from whole-tree scans to per-widget capsules, which improves loop stability and enables selective re-render.

## 2. Objectives
1. Define a `WidgetCapsule` contract that owns state, render lambda, mailbox, and composition metadata per widget path.
2. Teach the renderer to iterate `WidgetCapsule`s and invoke the lambda to obtain a `RenderPackage` (draw commands + texture refs + dirty hints).
3. Replace implicit reducer fan-in with explicit event mailboxes: widgets subscribe to named events (`hover`, `pressed`, `pointer.drag`, etc.) and receive structured batches.
4. Introduce a library of composable primitives (`ClickableSurface`, `TextRun`, `IconGlyph`, `StackLayout`, etc.) so higher-level widgets are built by assembly instead of bespoke code.
5. Maintain compatibility with the existing declarative schemas and readiness helpers while providing migration shims for samples/tests.

## Status Tracker
- ☑ Phase 0 – Research & Instrumentation (capsule mirrors + telemetry hooks)
  - 2025-12-11: Declarative widget audit completed (reducers/render data/events). See "Phase 0 Audit Summary" below for capsule mirror inputs.
  - 2025-12-11: Button + Label capsule mirrors landed with render/mailbox telemetry and UITest coverage (capsule creation, render invocation counters, mailbox dispatch metrics).
- ☑ Phase 1 – Render Walker (WidgetSurface generation + per-widget software raster)
  - 2025-12-11: `WidgetRenderPackage` v1 struct + serializer landed with tests; walker integration and widget ports still pending.
  - 2025-12-12: SceneLifecycle adds `WalkWidgetCapsules` behind `PATHSPACE_WIDGET_CAPSULES`; button/label renders now emit `capsule/render/package` snapshots while legacy buckets stay in place. `widget_pipeline` benchmark now records per-capsule render timing for the capsule walker flag.
  - 2025-12-12: Capsule walker software-rasterizes widget buckets into per-widget framebuffers under `capsule/render/framebuffer`, hashes the rendered pixels into `WidgetSurface` fingerprints, and stores the updated `WidgetRenderPackage` alongside the legacy buckets.
- ☑ Phase 2 – Event Mailboxes (subscription schema + reducer migration)
  - 2025-12-12: WidgetEventTrellis routes capsule mailboxes under `capsule/mailbox/events/<topic>/queue` when `PATHSPACE_WIDGET_CAPSULES` is set; button/label mirrors now declare hover/press/release/activate subscriptions and UITests cover label + button mailbox delivery without legacy ops writes.
  - 2025-12-12: InputTask now drains capsule mailbox queues for button/toggle/label when the feature flag is set, publishing actions without legacy `ops/inbox` writes; toggle capsules mirror state/style + mailbox metrics. UITests cover mailbox-only dispatch across button/toggle/label and trellis mailboxes without legacy ops.
  - 2025-12-12: Inspector surface adds `/inspector/metrics/mailbox` endpoint and UI panel that summarizes capsule mailbox counters (events, failures, last event) per widget and highlights the most recent mailbox activity.
- ☑ Phase 3 – Primitive Composition Library (BoxLayoutPrimitive, fragment-only helpers)
  - 2025-12-12: Capsule primitives landed for button/label/toggle with `BoxLayout` scaffolding, surface/text/behavior wrappers, and mailbox topics mirrored under `capsule/primitives/*`. Capsule updates rewrite the primitives when labels or toggle state change, and regression coverage checks primitive roots + colors.
  - 2025-12-12: Slider capsules mirror state/style/range into capsule mailboxes and primitives (fill/track/thumb with weighted layout reflecting value). Mailbox reducers run without legacy ops queues, and UITests cover slider capsule primitives + mailbox dispatch.
  - 2025-12-12: List capsules add mailbox-only subscriptions for hover/select/activate/scroll, mirror sanitized items and style into capsule meta, and emit per-row primitives (background + vertical layout of row surfaces/text). Selection rewrites the primitive colors, and UITests cover mailbox topics and primitive updates.
  - 2025-12-12: Tree capsules mirror state/style/nodes into capsule meta, emit per-row primitives (background + depth-aware layouts with toggle/label text), and wire mailbox topics for tree hover/select/toggle/expand/collapse/request_load/scroll. UITests cover expansion/selection rewriting the primitive tree.
  - 2025-12-12: InputField capsules mirror text/placeholder/focus state and text-field style into capsule meta, emit primitives (background + text/placeholder + input behavior), and InputTask/Trellis route `text_*` mailbox topics without legacy ops writes. UITests cover primitive mirroring and mailbox reducer dispatch for input widgets.
  - 2025-12-12: Stack capsules mirror active_panel + panel ids into capsule meta, expose `stack_select` mailbox topics, and emit panel/layout primitives with weighted active panels. InputTask/Trellis treat stack panels as mailbox-only under `PATHSPACE_WIDGET_CAPSULES`, and UITests cover primitive weights plus mailbox dispatch.
  - 2025-12-12: InputTask now drains tree capsule mailbox queues (`tree_hover/select/toggle/expand/collapse/request_load/scroll`) and dispatches handlers without legacy `ops/inbox` writes; UITest coverage updated alongside mailbox metrics. Phase 3 composition + mailbox stack considered complete.
- ☑ Phase 4 – Capsule-Only Runtime (legacy mirror removal, doc/test sweep)
  - 2025-12-12: `PATHSPACE_WIDGET_CAPSULES` is now default-on; WidgetEventTrellis routes subscribed mailbox events without dual-writing legacy `ops/inbox`, and InputTask drains `text_area` + `paint_surface` mailboxes alongside existing widgets to begin the capsule-only transition.
  - 2025-12-12: PaintSurface fragments now mirror capsule metadata (kind, brush, buffer, render lambda) and subscribe to paint stroke mailbox topics so WidgetEventTrellis delivers paint events through capsule queues without legacy `ops/inbox` writes; UITests assert mailbox commits for stroke fuzzing.
  - 2025-12-12: InputTask stops consuming legacy `ops/inbox` queues for mailbox-capable widgets, relying solely on capsule mailboxes and flagging missing subscriptions to continue the reducer inbox removal.
  - 2025-12-12: WidgetEventTrellis + WidgetBindings now emit only capsule mailbox events; reducer helpers/tests were migrated off `/ops/inbox/queue`, completing the legacy reducer inbox removal for capsule widgets.
  - 2025-12-12: `PATHSPACE_WIDGET_CAPSULES_ONLY` bypasses legacy widget action/event inbox writes during InputTask dispatch so capsule-only environments rely exclusively on mailbox queues while keeping legacy paths available when the flag is unset.
  - 2025-12-12: Capsule-only runtime is now the default (`PATHSPACE_WIDGET_CAPSULES_ONLY` defaults to enabled); InputTask no longer mirrors mailbox actions into legacy event queues unless explicitly disabled, and UITests cover compatibility via `PATHSPACE_WIDGET_CAPSULES_ONLY=0`.
  - 2025-12-12: Doc/test sweep completed for capsule-only runtime; plan archived to `docs/finished/Plan_WidgetComposableRuntime_Finished.md`.

## 3. Non-Objectives
- Rewriting the SceneGraph or renderer snapshot pipeline; we only define a new widget-facing contract that existing targets can consume.
- Changing IO Trellis or input-device providers; the plan focuses on how widgets declare and consume events, not on how devices publish them.
- Reintroducing legacy builder APIs.
- Delivering a fully componentized theming system; themes remain as defined in `WidgetDeclarativeAPI.md` with additive hooks for primitives.

## 4. Target Architecture
### 4.1 Widget Capsules
- Each widget path (`/system/applications/<app>/windows/<win>/views/<view>/widgets/<id>`) owns a `WidgetCapsule` record persisted under `capsule/*`:
  - `capsule/state/*` - widget-local state, mirroring existing `state/*` but scoped for the capsule runtime.
  - `capsule/render/lambda` - serialized callable handle that the renderer executes to obtain a `RenderPackage` struct.
  - `capsule/mailbox/subscriptions` - declarative list of event topics and filters.
  - `capsule/primitives/*` - references to primitive fragments used to synthesize the widget tree.
- Capsules expose lifecycle hooks (`capsule/hooks/{mount,update,teardown}`) so composition layers can run initialization logic without modifying global reducers.
- Widget creation shifts from C++ structs (`Button::Args`, etc.) to lightweight constructor helpers. A helper is just a function that writes properties and lambda bindings under the destination PathSpace path (state, style, primitives, mailbox subscriptions). This keeps widget definitions data-driven and language-neutral—any client that can insert PathSpace nodes can author widgets without linking against widget-specific headers.

### 4.2 Render Walker Contract
- `SceneLifecycle::WalkWidgetCapsules(window, view)` becomes the canonical entry point for render passes.
- For each capsule, the walker executes `RenderPackage RenderWidget(const WidgetCapsule& capsule, RenderContext&)`:
  - Inputs: resolved theme, primitive tree, current state snapshot.
  - Output: packed draw commands (vector paths, quads, glyph runs), child references, dirty-rect hints, and a `WidgetSurface` descriptor.
  - The renderer caches the `RenderPackage` hash per capsule so a widget that does not mutate can be skipped entirely.
- `RenderPackage` is transport-agnostic so HTML targets, Metal, and Software2D share the same descriptor.
- Each capsule performs its own software raster pass into a private framebuffer (the `WidgetSurface`). The outer renderer becomes a pure compositor: it only needs to blend 2D images in Z-order, apply transforms, and submit the final surface to the platform presenter. GPU targets can upload the per-widget surfaces as textures, while software targets simply copy pixels; either way the widget runtime owns the detailed draw logic.
- Composition metadata includes opacity, blend mode, clipping, and optional stretch-to-fit flags so parent boxes can scale child surfaces without re-rendering the widget. When a widget dirties, it reruns its software pass and swaps the updated surface handle into the next `RenderPackage`.

### 4.3 Event Mailboxes
- Widgets declare `mailbox/subscriptions/<event>` entries with optional filters (`device=*`, `pointer.kind=touch`, `keyboard.key=enter`).
- `WidgetEventTrellis` routes events into per-widget queues under `mailbox/events/<event>/queue` instead of writing to reducer inboxes.
- Capsules expose `capsule/mailbox/reducer` callables that consume batches (e.g., `vector<EventBatch>`) and produce a `StateDelta + CommandList` (state writes + messages for parents/children).
- Mailboxes record metrics at `capsule/mailbox/metrics/{events_total,dispatch_failures_total,last_dispatch_ns}` for targeted diagnostics.
- All mailbox payloads use the existing normalized event schema from IO Trellis/WidgetEventTrellis—pointer/gamepad/touch/mouse data shows up as cursor coordinates + button metadata regardless of the original device, so widgets never special-case hardware or HTML-vs-native sources.

### 4.4 Composable Primitives
- Introduce `WidgetPrimitive` definitions stored under `capsule/primitives/<id>`:
  - `SurfacePrimitive` - fills/borders/shadows and pointer target shapes.
  - `TextPrimitive` - typographic runs with theme bindings.
  - `IconPrimitive` - references to atlas textures or vector icons.
  - `LayoutPrimitive` - flex/stack/grid containers describing child placement. `BoxLayoutPrimitive` specializes this pattern for horizontal or vertical boxes with axis, distribution mode (even, weighted, intrinsic), spacing, padding, and per-child weight overrides so applications can opt into proportional fills without bespoke layout code.
  - `BehaviorPrimitive` - wrappers that inject mailboxes (e.g., clickable, draggable, focusable).
- High-level widgets become declarative assemblies: e.g., `Button = Behavior::Clickable + Surface::RoundedRect + Layout::Stack(Text, OptionalIcon)`.
- Provide helper builders (`Composable::Button`, etc.) that emit primitives and hook up default mailboxes, enabling applications to build custom controls by mixing primitives.
- Widget composition revolves around fragments: every widget helper is a function such as `CreateButton(path, label, callback)` that mounts the widget under `path`, returns a lightweight `FragmentHandle` containing the final path, and exposes `.addChildren({FragmentHandle...})` so callers can attach additional fragments before inserting the subtree elsewhere. A fragment is just a mini PathSpace rooted at the widget with all of its state/style/primitives/mailboxes; once you finish assembling it, you simply call the standard `space.insert(destination_path, fragment_space)` (which already supports full-subtree inserts) to splice the fragment into the live hierarchy—no bespoke `Widgets::Mount` helper is required, and the existing insert semantics (whole-object replace with wait/notify) keep the contract coherent.
- Widgets differentiate between caller-owned children (`children/*`) and runtime-owned internals (`internal_children/*`). Box layouts and user composition APIs only touch `children/*`, while a widget’s implementation details (e.g., a button’s label or focus ring) reside under `internal_children/*` so tooling can inspect them without exposing them for modification.
- Complex controls compose multiple capsules. For example, a slider owns the range/value math and subscribes to pointer drag events, while its thumb capsule deals with pointer capture and emits `thumb_drag_*` messages upward via the mailbox. The parent slider then updates its value, marks itself dirty, and publishes a position update back to the thumb. This parent↔child messaging pattern keeps behavior modular without global reducers.
- Rendering stays intentionally minimal: capsule draw lambdas rely on a tiny set of primitives—rounded rectangles and text runs cover every existing widget. By keeping the raster API to “fill rounded rect” + “draw text” (with color/opacity/gradient variants), new widgets don’t bring bespoke vector routines and the software renderer can stay compact.

### 4.5 Compatibility Layer
- Maintain `render/*`, `state/*`, and handler nodes for the migration window by mirroring capsule output back into the existing schema.
- Provide shims so current `Button::Create` calls instantiate a capsule with default primitives until callers migrate to primitive assemblies.
- Update readiness helpers (`ensure_declarative_scene_ready`) to wait on `capsule/metrics/*` alongside legacy nodes.

### 4.6 Default Window Composition
- Every window view implicitly mounts a vertical `BoxLayoutPrimitive`. A single child therefore inherits the entire window region with zero additional layout declarations.
- Adding multiple children to the root vertical box automatically divides the height evenly across the children (horizontal boxes do the same for width). Two buttons under the root box render as stacked controls that each occupy half the window height, and the pattern generalizes for any child count.
- Developers can nest explicit `HorizontalBox` or `VerticalBox` primitives anywhere in the tree to carve subregions. These boxes remain invisible, emit no render package unless debugging is enabled, and simply forward pointer focus and mailboxes to their descendants.
- Each child may specify `weight`, `min_size`, or `max_size` metadata within the box layout primitive. Weighted distributions let one child claim 70 percent of the space while others split the remainder evenly, keeping the even-fill default but supporting richer compositions.
- Future mailbox topics (`layout/request_resize`) can inform a parent box that a child prefers a new weight or min size; the capsule reducer applies the request and republishes an updated render package without touching global reducers.

### 4.7 HTML Server Compatibility
- The capsule runtime must continue to mirror outputs required by the HTML adapter plans (`docs/finished/Plan_ServeHtml_Modularization_Finished.md`, `docs/finished/Plan_PathSpaceHtmlServer_Finished.md`). Each `WidgetSurface` publishes an HTML-friendly descriptor (dimensions, premultiplied pixels, optional vector commands) under the same `renderers/<rid>/targets/html/*` namespaces the server already streams.
- Each capsule emits two representations:
  1. A bitmap `WidgetSurface` for native compositors.
  2. A lightweight HTML payload (`capsule/html/{dom,css,svg}`) only for widgets that map cleanly onto native HTML controls (buttons, labels, lists, sliders, toggles, inputs). Rich surfaces (paint canvas, bespoke viewers) never expose an HTML fragment—they stick to the bitmap/canvas path so we do not reinvent complex rendering in the browser. ServeHtml can therefore choose per widget: use the semantic HTML snippet when the widget is part of the “HTML-capable” set, or fall back to the rendered bitmap otherwise.
- The HTML adapter attaches a small JS bridge to semantic fragments so browser events (click, pointer, keyboard, focus) are converted back into the normalized event schema and forwarded to the capsule’s mailbox queues. From the widget’s perspective there is no difference between native and HTML input sources.
- When a capsule re-renders, it updates both the compositor surface handle and the HTML payload metadata so `/apps/<app>/<view>` can hot-reload without decoding the entire PathSpace tree. HTML targets can diff DOM fragments or request the bitmap, whichever is cheaper for the scenario.
- Mailboxes and primitive metadata stay accessible through the HTML inspector endpoints, enabling ServeHtml controllers to read widget state for diagnostics or remote input injection exactly as they do today.

### 4.8 Code Organization Strategy
- Mirror the successful declarative refactor: stand up a new `src/pathspace/ui/composable/` (and matching `include/pathspace/ui/composable/`) tree that houses the capsule runtime while keeping legacy declarative code untouched during the migration. This keeps translation units small, lets us dual-ship both runtimes under feature flags, and makes eventual cleanup a straight directory delete—exactly how the declarative split retired the old builder monolith.
- Shared utilities (themes, screenshot helpers, readiness guards) move into neutral `ui/common/` folders only when both runtimes need them. Everything capsule-specific (capsule records, mailboxes, box layout primitives, fragment helpers) lives under the new namespace so reviewers can diff changes without noise from unrelated files.
- Tests mirror the structure (`tests/ui/composable/*`) just as declarative tests lived under `tests/ui/declarative/*`, ensuring loop stability runs can exercise each runtime independently until we flip the kill switch.

### 4.9 Dirty/Resize Protocol
- Size propagation flows parent→child: when a layout primitive resolves its slot, it writes `capsule/layout/assigned_size` (and optional DPI data) under each child capsule plus a `resize_event` entry in the child’s mailbox. Widgets therefore always know their pixel bounds before rendering.
- Each capsule maintains a local damage tracker (`capsule/render/damage`), combining resize notifications, explicit events (pointer hover, state changes), and parent-supplied dirty flags. When damage is non-empty, the capsule reruns its software raster to refresh its `WidgetSurface`.
- Parents never repaint children directly; they only notify them of size changes or state messages. A widget decides whether it needs a full repaint or a partial rect update and signals completion by swapping the refreshed surface handle back into its capsule record.
- The render walker inspects `WidgetSurface.revision` per capsule. Only widgets whose surface revision changed since the last walk push new pixels into the compositor, keeping the outer renderer a pure blitter.
- Telemetry mirrors the flow: `capsule/render/metrics/{last_paint_ns,dirty_rect_area}` records repaint decisions, while `capsule/layout/metrics` tracks how often a widget was resized. UITests can assert on these nodes to prove that widgets repaint only when necessary.

## 5. Execution Phases
### Phase 0 - Research & Instrumentation (December 2025)
- Audit existing declarative widgets to catalog their reducers, render data, and event types.
- Add `capsule/` mirrors for two pilot widgets (button, label) without changing behaviour; record render lambda invocations and mailbox usage. **Status:** button + label mirrors plus render/mailbox metrics landed; capsule nodes covered by UITests.
- Extend UITests to assert the presence of capsule nodes and to log noop render passes. **Status:** coverage added in `tests/ui/test_DeclarativeWidgets.cpp`, `tests/ui/test_DeclarativeSceneLifecycle.cpp`, and `tests/ui/test_WidgetEventTrellis.cpp`.

#### Phase 0 Audit Summary (December 11, 2025)
- **Button**
  - Render data: `/state` stores `ButtonState{enabled, hovered, pressed}`, `/meta/style` holds the sanitized style, `/meta/label` keeps the text, optional `/style/theme`, and `render/{synthesize,dirty,dirty_version}` mark paint work.
  - Reducers: `SetButtonHovered` + `SetButtonPressed` (`WidgetStateMutators.cpp`) flip hover/press flags and mark dirty from pointer + focus events (`WidgetEventPointerHandlers.cpp`, `WidgetEventFocusHandlers.cpp`).
  - Events: pointer/focus generate `WidgetOpKind::{Press,Release,Activate}` routed to `events/press` (HandlerKind::ButtonPress) via `InputTask`.
- **Toggle**
  - Render data: `/state` `ToggleState{enabled, checked, hovered}`, `/meta/style`, render synthesize/dirty nodes.
  - Reducers: `SetToggleHovered`, `ToggleToggleChecked` mutate state on hover and release-inside actions; wired in pointer + focus handlers.
  - Events: `WidgetOpKind::Toggle` hits `events/toggle` (HandlerKind::Toggle); press/release ops still flow for telemetry.
- **Slider**
  - Render data: `/state` `SliderState{enabled, value, hovered, dragging}`, `/meta/style`, `/meta/range` (min/max/step), render synthesize/dirty nodes.
  - Reducers: slider begin/update/commit set `dragging`, clamp `value`, and mark dirty via `write_slider_state`/`update_slider_hover` in `WidgetEventHelpers.cpp` invoked from pointer drag handlers.
  - Events: `WidgetOpKind::{SliderBegin,SliderUpdate,SliderCommit}` resolve to `events/change` (HandlerKind::Slider); pointer hover tracks inside/outside for hit-testing.
- **List**
  - Render data: `/meta/items` (ids + labels), `/meta/style`, `/state` `ListState{hovered_index, selected_index, enabled}`.
  - Reducers: `SetListHoverIndex`, `SetListSelectionIndex` adjust hover/selection when pointer presses/releases inside rows (`WidgetEventPointerHandlers.cpp`).
  - Events: `WidgetOpKind::{ListSelect,ListActivate}` route to `events/child_event` (HandlerKind::ListChild) carrying row id/index.
- **Tree**
  - Render data: `/meta/nodes` (flat tree spec), `/meta/style`, `/state` `TreeState{hovered_id, selected_id, expanded_ids, enabled}`.
  - Reducers: `SetTreeHoveredNode`, `SetTreeSelectedNode`, `ToggleTreeExpanded` mutate hover/selection/expansion; fed by pointer handlers for row/toggle targets.
  - Events: `WidgetOpKind::{TreeSelect,TreeToggle,TreeExpand,TreeCollapse,TreeRequestLoad}` route to `events/node_event` (HandlerKind::TreeNode) with the node id.
- **Stack**
  - Render data: `/state/active_panel`, per-panel metadata under `/panels/<id>/{order,visible,target}`, children under `/children/<id>`.
  - Reducers: `SetActivePanel` and `update_panel_visibility` flip visibility + dirty when selection changes (pointer + focus stack handlers).
  - Events: `WidgetOpKind::StackSelect` targets `events/panel_select` (HandlerKind::StackPanel) with the panel id suffix.
- **Label**
  - Render data: `/state/text`, `/meta/typography`, `/meta/color`, render synthesize/dirty nodes.
  - Reducers: `SetText` writes `/state/text` and marks dirty; no hover/press reducers.
  - Events: optional `events/activate` (HandlerKind::LabelActivate) registered by the fragment but currently not dispatched by `InputTask` (falls through as unsupported), so capsules should treat it as future-facing only.
- **InputField**
  - Render data: `/state/text`, `/state/placeholder`, render synthesize/dirty nodes; text editing uses `TextFieldState` under `/state` (cursor, selection, composition) for trellis updates.
  - Reducers: text focus + editing handlers read/write `TextFieldState` via `read_text_state`/`write_text_state` on movement, deletion, composition, clipboard, and scroll events (`WidgetEventFocusHandlers.cpp`).
  - Events: `WidgetOpKind::{TextInput,TextDelete,TextMoveCursor,TextSetSelection,TextComposition* ,TextClipboard*,TextScroll}` go to `events/change` (HandlerKind::InputChange); `TextSubmit` goes to `events/submit` (HandlerKind::InputSubmit).
- **PaintSurface**
  - Render data: brush params under `/state/brush/*`, GPU toggle `/render/gpu/enabled`, buffer defaults via `PaintRuntime::EnsureBufferDefaults`, render synthesize/dirty nodes.
  - Reducers: pointer paint handlers manage stroke ids and last-local tracking; strokes are emitted as ops and leave render-dirty markers when buffers change.
  - Events: `WidgetOpKind::{PaintStrokeBegin,PaintStrokeUpdate,PaintStrokeCommit}` address `events/draw` (HandlerKind::PaintDraw) with components `paint_surface/stroke/<id>`.

### Phase 1 - Render Walker (January 2026)
- Implement `RenderPackage` struct + serializer.
- Add `SceneLifecycle::WalkWidgetCapsules` and integrate it into renderer targets behind a feature flag (`PATHSPACE_WIDGET_CAPSULES=1`).
- Port button + label render paths to the walker, keeping legacy render cache writes for parity.
- Expand the widget_pipeline benchmark to record per-capsule render time.

### Phase 2 - Event Mailboxes (January-February 2026)
- Define mailbox schemas and update `WidgetEventTrellis` to route into them when the feature flag is enabled.
- Port button/toggle to mailbox reducers; ensure pointer/keyboard/gamepad flows operate without the legacy reducer.
- Update telemetry + inspector views to display mailbox stats.

### Phase 3 - Primitive Composition Library (February 2026)
- Land the primitive schema + helper headers (`include/pathspace/ui/declarative/primitives/*`).
- Rebuild button, toggle, slider, list using primitives; prove that composed widgets emit the same render packages and event responses.
- Publish docs (`docs/WidgetPrimitives.md`) and migration guides.

### Phase 4 - Capsule-Only Runtime (Complete — December 2025)
- Delivered December 12, 2025; objectives retained for historical reference.
- Flip declarative widgets to emit only capsules; delete legacy reducer inboxes and render cache plumbing once perf + telemetry parity holds.
- Update samples/tests/docs to reference primitives and mailboxes exclusively.
- Archived at `docs/finished/Plan_WidgetComposableRuntime_Finished.md`.

## 6. Deliverables
- New contract headers: `WidgetCapsule.hpp`, `WidgetRenderPackage.hpp`, `WidgetMailbox.hpp`, `WidgetPrimitives.hpp`.
- Scene lifecycle + renderer updates for the capsule walker.
- Per-widget software rendering surfaces (`WidgetSurface`) plus compositor changes so targets only need to blend 2D textures/images.
- IO/Trellis routing changes for mailbox subscriptions.
- Primitive composition helpers and updated samples (`widgets_example`, `paint_example`, inspector UI).
- Documentation + migration guides.
- Rewrite the declarative examples (widgets gallery, paint demos, hello app) to compose the new fragments/capsules so contributors have working references for the updated API. This includes refreshing the CLI helpers, ServeHtml embedding, and screenshot flows to rely on the composable widgets.

## 7. Risks & Mitigations
- **Render regressions:** Mitigate via feature flags, per-capsule benchmarking, and dual-writing render caches until parity proven.
- **Event loss/latency:** Batch delivery may hide ordering bugs; add exhaustive UITests + fuzzers that compare mailbox vs. legacy reducer outputs.
- **Complexity creep:** Limit the primitive set and require design reviews before adding new primitive types to avoid a fragmented ecosystem.
- **Migration fatigue:** Provide automated shims (code mods + docs) so contributors can upgrade gradually; maintain compatibility mirrors until February 2026 support window.

## 8. Metrics & Acceptance
- `widget_pipeline` benchmark shows ≤5% regression when capsule walker enabled with two pilot widgets.
- UITest suite runs green under `PATHSPACE_WIDGET_CAPSULES=1` across the mandated 5× loop.
- inspector shows capsule + mailbox diagnostics for all sample widgets.
- Composed button/toggle demonstrate that replacing primitives (e.g., alternate text widget) requires no runtime changes.

## 9. Dependencies
- Renderer/lifecycle code from `Plan_SceneGraph_Renderer_Finished.md` must stay stable; walker work depends on that pipeline.
- Declarative runtime infrastructure from `docs/WidgetDeclarativeAPI.md` (LaunchStandard, readiness helpers).
- IO Trellis + WidgetEventTrellis from `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md`.

## 10. Open Questions
1. Should capsules live alongside existing widget nodes or under a new namespace (`capsules/<id>`) for non-UI consumers?
2. How do we expose primitive composition to scripting layers (PrimeScript, Carta Linea) without leaking C++-specific types?
3. Do we permit nested capsules (widget containing sub-widgets that export their own capsules) or confine capsules to leaf widgets?
4. What is the fallback when a capsule render lambda throws - skip the widget, or render a diagnostic surface?
