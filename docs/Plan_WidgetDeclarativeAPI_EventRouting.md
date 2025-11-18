# Widget Declarative API – Event Routing Inventory

_Last updated: October 31, 2025_

> **Update (November 14, 2025):** Declarative widgets now register handlers via `HandlerBinding` records. `events/<event>/handler` stores `{ registry_key, kind }` and the helper layer keeps an in-memory registry mapping those ids back to the user-provided lambdas (button press, slider change, etc.). The binding id format is `<widget_path>#<event>#<sequence>` so removals can drop every handler under a widget subtree deterministically. Runtime dispatch will resolve the binding id before running the callback; PathSpace no longer stores `std::function` payloads directly.

> **Decision (November 14, 2025):** Declarative routes were removed. Widgets now execute the `events/<event>/handler` callable directly without intermediate routing tables. The details below describe the shelved design and remain here purely for historical reference.

> **Update (November 17, 2025):** IO Pump Phase 2 introduced canonical per-window event queues at `/system/widgets/runtime/events/<window-token>/{pointer,button,text}/queue` plus `/global/*` fallbacks. Window bridges register device subscriptions under `/system/widgets/runtime/windows/<token>/subscriptions/{pointer,button,text}/devices`. The upcoming routing Trellis will drain those queues instead of bespoke app loops.

> **Update (November 17, 2025):** `CreateInputTask` now resolves `HandlerBinding` records on each `WidgetAction`, invokes the corresponding button/toggle/slider/list/tree/input handlers (paint + stack handlers remain TODO), logs failures to `/system/widgets/runtime/input/log/errors/queue`, and publishes telemetry under `/system/widgets/runtime/input/metrics/{handlers_invoked_total,handler_failures_total,handler_missing_total,last_handler_ns}`.

> **Update (November 17, 2025):** WidgetEventTrellis drains per-window queues, runs `Scene::HitTest`, and now emits `WidgetOp`s for sliders (begin/update/commit), lists (hover/select/activate), trees (hover/toggle/select), text inputs (focus/input), stack panels (`StackSelect`), and paint surfaces (`PaintStrokeBegin/Update/Commit`) in addition to the existing button/toggle coverage.

> **Update (November 17, 2025):** The declarative runtime mirrors every reduced `WidgetAction` into `widgets/<id>/events/inbox/queue` plus per-event queues (`events/press/queue`, `events/toggle/queue`, etc.) and tracks enqueue/drop telemetry via `/system/widgets/runtime/input/metrics/{events_enqueued_total,events_dropped_total}`.

> **Update (November 17, 2025):** Stack previews now emit `StackSelect` widget ops keyed by `stack/panel/<panel-id>` so `events/panel_select/handler` invocations no longer require bespoke routing tables.

> **Update (November 17, 2025):** Paint-surface hit tests produce `PaintStrokeBegin/Update/Commit` ops with pointer-local coordinates. The trellis enqueues each op and InputTask calls the `events/draw/handler` binding after mirroring the same payload into the widget's action queues.

> **Update (November 18, 2025):** WidgetEventTrellis now mutates canonical widget state (button hover/press, toggle checked, slider value/dragging, list hover/selection, tree hover/selection/expanded) and flips `render/dirty` before each `WidgetOp` is published, guaranteeing that declarative widgets stay visually in sync even when user handlers are empty; see `tests/ui/test_WidgetStateMutators.cpp` for unit-level coverage of the state mutators.

> **Update (November 18, 2025):** InputTask writes paint-surface strokes into `state/history/<id>/{meta,points}`, increments `render/buffer/revision`, and rebuilds cached buckets before resolving handler bindings, so declarative paint widgets redraw even when no `draw` handler is installed.

> **Update (November 18, 2025):** Keyboard and gamepad devices now travel the same declarative path: Tab/Shift+Tab and gamepad shoulders step focus through `Widgets::Focus`, D-pad/arrow keys drive slider/list/tree navigation, and Space/Enter/A emit the same press/toggle/list-activate ops that mouse input already produced. Text inputs receive cursor/delete/submit ops without bespoke loops, so declarative apps can remove their ad-hoc keyboard bridges.

> **Update (November 18, 2025):** InputTask now mirrors handler results back to `widgets/<id>/metrics/handlers/{invoked_total,failures_total,missing_total}` in addition to the global `/system/widgets/runtime/input/metrics/*` counters, so dashboards can flag a specific widget whose handler never fires or keeps throwing.

> **Update (November 18, 2025):** `WidgetFragment` now records handler specs and `Widgets::Mount` rebinds them at insertion time; scenes can intercept callbacks via `Widgets::Handlers::{Read,Replace,Wrap,Restore}` without copying handler nodes or adding bespoke routing tables.

## Auto-render request queue
- **Path**: `<target>/events/renderRequested/queue`
- **Producers**: `SP::UI::Builders::Detail::enqueue_auto_render_event` is invoked from widget bindings (button/toggle/slider/list/tree), focus helpers, and scene hit-testing code to request redraws after state mutations (see `src/pathspace/ui/BuildersDetail.hpp:189`, `src/pathspace/ui/WidgetBindings.cpp`, `src/pathspace/ui/SceneBuilders.cpp`).
- **Payload**: `AutoRenderRequestEvent { sequence, reason, frame_index }` (`include/pathspace/ui/Builders.hpp:208`). `sequence` increments atomically per request, `reason` describes the trigger (e.g. `widget/button`, `hit-test`), and `frame_index` mirrors the last presented frame when available.
- **Consumers**: Path window presenters watch the queue to schedule renders; downstream adapters (HTML, local window) already rely on the existing queue, so changing shape/path would be a breaking change.
- **Notes**: Queue currently stores plain structs with no route metadata beyond the target path. Multiple producers reuse the same queue; diagnostics depend on `reason` to differentiate sources.

## Widget interaction queues

### `widgets/<widget>/ops/inbox/queue`
- **Producers**: `CreateWidgetEventTrellis` now reads device events, mutates the canonical widget state (hover, pressed, value, selection, expanded) under `widgets/<id>/state/*`, flips `render/dirty`, and then enqueues `WidgetOp` entries using the same schema as the legacy `Widgets::Bindings::Dispatch*` helpers (`src/pathspace/ui/WidgetBindings.cpp`). Imperative apps can still call the bindings directly; declarative apps rely on the trellis-driven mutations.
- **Payload**: `WidgetOp { kind, widget_path, target_id, pointer, value, sequence, timestamp_ns }` with `pointer = PointerInfo { scene_x, scene_y, inside, primary }` (`include/pathspace/ui/Builders.hpp:1268-1335`). `kind` covers hover/press/activate/list/tree ops; `value` encodes analog data (slider position, scroll delta) and is reused for discrete indices by reducers.
- **Consumers**: Reducer pipelines (either app-side or `Widgets::Reducers::ReducePending`) drain the queue, mutate widget state, and optionally publish derived actions. The queue is single-consumer—no fan-out semantics today.
- **Notes**: Entries repeat `widget_path` even though the queue path already scopes the widget. No schema is attached to distinguish device/source; routing relies on UI layout state when the op was generated.

### `widgets/<widget>/ops/actions/inbox/queue`
- **Producers**: `Widgets::Reducers::PublishActions` publishes high-level `WidgetAction` values after processing ops (`src/pathspace/ui/WidgetReducers.cpp`).
- **Payload**: `WidgetAction { kind, widget_path, target_id, pointer, analog_value, discrete_index, sequence, timestamp_ns }` (`include/pathspace/ui/Builders.hpp:2290-2304`).
- **Consumers**: Application logic that prefers semantic actions over raw ops; examples wire the queue into logging and automation harnesses.
- **Notes**: Queue is optional; reducers also return the action vector directly. Like `WidgetOp`, there is no routing metadata beyond `widget_path`.

### `widgets/<widget>/events/inbox/queue`
- **Producers**: `CreateInputTask` mirrors every reduced `WidgetAction` before resolving handler bindings. Successful enqueue also pushes the same payload into `widgets/<widget>/events/<event>/queue` for the event name associated with the handler binding (`press`, `toggle`, `change`, `child_event`, `node_event`, `submit`).
- **Payload**: Identical to `WidgetAction` so tooling can reuse the existing structs/decoders.
- **Consumers**: Diagnostics, automation harnesses, or future middleware that want to observe canonical widget events without touching the reducers. Because the queue is managed by the input runtime, events appear even when apps bypass the legacy actions queue.
- **Notes**: Enqueue/drop telemetry surfaces via `/system/widgets/runtime/input/metrics/{events_enqueued_total,events_dropped_total}`; failures also log to `/system/widgets/runtime/input/log/errors/queue` with the widget path + handler event.

- **Stack select ops (new)**: hits on `stack/panel/<panel-id>` surfaces enqueue a `StackSelect` action, mirror it into `events/panel_select/queue`, and invoke the optional `panel_select` handler with `panel_id` populated so containers can react without manual routing tables.
- **Paint stroke ops (new)**: pointer down/move/up gestures over `paint_surface` widgets emit `PaintStrokeBegin`, `PaintStrokeUpdate`, and `PaintStrokeCommit` actions that capture pointer-local coordinates. The runtime mirrors those actions into `events/draw/queue` and calls the optional `draw` handler after enqueueing.

## Related device/event entry points
- PathIO adapters expose device event queues at `<device mount>/events` (mouse, keyboard, gamepad) with device-specific payload structs (`src/pathspace/layer/io/PathIOMouse.hpp`, `src/pathspace/layer/io/PathIOKeyboard.hpp`, `src/pathspace/layer/io/PathIOGamepad.cpp`). Declarative widgets ingest these events indirectly through existing input mixers before dispatching `WidgetOp` records.

### Handler bindings (new)

- Declarative helpers now emit a small `HandlerBinding` struct at `events/<event>/handler` with `{ registry_key, kind }`. The registry key is generated as `<widget_root>#<event>#<sequence>` so removal simply drops every entry matching the widget root prefix.
- Actual `std::function` callbacks live in an in-memory registry keyed by that id. Consumers resolve the binding id at dispatch time (button press, slider change, etc.) and invoke the stored lambda with the appropriate context (`ButtonContext`, `SliderContext`, ...).
- The helper automatically registers/unregisters bindings when fragments mount or `Widgets::Remove` is called, so PathSpace never stores raw function objects.

### Runtime handler telemetry

- `/system/widgets/runtime/input/metrics/{handlers_invoked_total,handler_failures_total,handler_missing_total,last_handler_ns}` provide a running count of successful dispatches, registry/handler mismatches, total failures (binding mismatches, exceptions), and the most recent successful handler timestamp, respectively.
- `/system/widgets/runtime/input/log/errors/queue` receives stringified failure entries (missing registry keys, handler kind mismatches, exceptions thrown from handlers) so diagnostics only need to watch a single queue for reducer + dispatch errors.
- Missing handler bindings (i.e., no `events/<event>/handler` node) are treated as opt-out: the dispatcher simply records the action for downstream consumers without logging a failure, enabling widgets that intentionally decline to attach handlers.

## Observations for declarative routing contract
- Existing queues assume a single logical consumer; the plan now introduces shared route tables under `<scene>/widgets/runtime/routes/...` plus per-widget defaults to unlock multi-route dispatch without breaking legacy queues.
- Producers rely on atomically incremented `sequence` fields for ordering; any new routing metadata must preserve ordering semantics or document new guarantees.
- Pointer metadata is embedded per entry, but there is no shared payload schema that callers can extend. Adding route descriptors may require augmenting `WidgetOp`/`WidgetAction` or introducing an envelope structure.
- Auto-render requests multiplex disparate reasons in one queue. If declarative routing introduces per-widget event routing, ensure the auto-render queue continues to function (or document a migration path).
- Version flagging under `meta/routing/version` will help migrate existing apps to the richer contract incrementally.

## Route merger implementation notes
- The (abandoned) merger concept would have lived fully in the UI layer, consuming three namespaces: widget defaults (`<widget>/events/<event>/route`), scene overrides (`<scene>/widgets/runtime/routes/<widget>/<event>/route`), and shared tables (`<scene>/widgets/runtime/routes/shared/<event>/route`). No core PathSpace changes were required.
- Merge modes:
  - `replace` swaps the entire handler list with the override.
  - `prepend` injects override handlers ahead of defaults respecting their `priority`.
  - `append` places handlers after defaults while still sorting by `priority`.
- Cached plans are keyed by `(scene, widget, event, routingVersion)`. Cache invalidation triggers when:
  - Any source node mutates (detected via `notify` watching the relevant paths).
  - `meta/routing/version` changes on either the widget or override node.
  - The dispatcher reports a handler failure that requests a refresh (e.g., missing handler path).
- Diagnostics:
  - `<scene>/widgets/runtime/routes/<widget>/<event>/stats/handlerCount`
  - `<scene>/widgets/runtime/routes/<widget>/<event>/stats/overridesApplied`
  - `<scene>/widgets/runtime/routes/<widget>/<event>/stats/lastRefreshMs`
  - `<scene>/widgets/runtime/routes/<widget>/<event>/stats/cacheHits` / `cacheMisses`
- Warnings for ignored overrides (version mismatch, malformed schema) land under `<scene>/widgets/runtime/routes/<widget>/<event>/log`.

## Dispatcher notes
- `src/pathspace/ui/WidgetEventDispatcher.*` drains `WidgetOp` queues, fetches merged plans, and executes handlers in ascending `priority`.
- Result handling:
  - `Handled` stops iteration for `exclusive` handlers and marks `events/<event>/lastResult = handled`.
  - `Declined` advances to the next handler and records the intermediate decision under `events/<event>/log`.
  - `Error` records the error payload, increments `stats/errors`, attempts the configured `fallback`, and emits a notification for tooling.
- Telemetry mirrors dispatcher activity at `<widget>/events/<event>/stats`:
  - `dispatchCount`, `handledCount`, `declinedCount`, `errorCount`
  - `fallbackCount`, `exclusiveShortCircuits`, `sharedInvocations`
  - `lastDispatchTimestampMs`, `lastErrorTimestampMs`
- Keyboard and pointer bridges record the `WidgetOp` they produced so the dispatcher can add context (device ID, modifiers) to the log entry without altering the queue schema.
- Legacy behaviour remains available: when no routing metadata exists the dispatcher synthesizes a single-handler plan pointing at `<widget>/events/<event>/handler` and emits `stats/mode = "legacy"`.
