# Widget Declarative API – Event Routing Inventory

_Last updated: October 31, 2025_

## Auto-render request queue
- **Path**: `<target>/events/renderRequested/queue`
- **Producers**: `SP::UI::Builders::Detail::enqueue_auto_render_event` is invoked from widget bindings (button/toggle/slider/list/tree), focus helpers, and scene hit-testing code to request redraws after state mutations (see `src/pathspace/ui/BuildersDetail.hpp:189`, `src/pathspace/ui/WidgetBindings.cpp`, `src/pathspace/ui/SceneBuilders.cpp`).
- **Payload**: `AutoRenderRequestEvent { sequence, reason, frame_index }` (`include/pathspace/ui/Builders.hpp:208`). `sequence` increments atomically per request, `reason` describes the trigger (e.g. `widget/button`, `hit-test`), and `frame_index` mirrors the last presented frame when available.
- **Consumers**: Path window presenters watch the queue to schedule renders; downstream adapters (HTML, local window) already rely on the existing queue, so changing shape/path would be a breaking change.
- **Notes**: Queue currently stores plain structs with no route metadata beyond the target path. Multiple producers reuse the same queue; diagnostics depend on `reason` to differentiate sources.

## Widget interaction queues

### `widgets/<widget>/ops/inbox/queue`
- **Producers**: `Widgets::Bindings::Dispatch*` functions emit `WidgetOp` entries in response to input (`src/pathspace/ui/WidgetBindings.cpp`). Pointer-driven flows originate in `WidgetInput::HandlePointer*` helpers (`src/pathspace/ui/WidgetInput.cpp`) which diff local state and call the dispatchers.
- **Payload**: `WidgetOp { kind, widget_path, target_id, pointer, value, sequence, timestamp_ns }` with `pointer = PointerInfo { scene_x, scene_y, inside, primary }` (`include/pathspace/ui/Builders.hpp:1268-1335`). `kind` covers hover/press/activate/list/tree ops; `value` encodes analog data (slider position, scroll delta) and is reused for discrete indices by reducers.
- **Consumers**: Reducer pipelines (either app-side or `Widgets::Reducers::ReducePending`) drain the queue, mutate widget state, and optionally publish derived actions. The queue is single-consumer—no fan-out semantics today.
- **Notes**: Entries repeat `widget_path` even though the queue path already scopes the widget. No schema is attached to distinguish device/source; routing relies on UI layout state when the op was generated.

### `widgets/<widget>/ops/actions/inbox/queue`
- **Producers**: `Widgets::Reducers::PublishActions` publishes high-level `WidgetAction` values after processing ops (`src/pathspace/ui/WidgetReducers.cpp`).
- **Payload**: `WidgetAction { kind, widget_path, target_id, pointer, analog_value, discrete_index, sequence, timestamp_ns }` (`include/pathspace/ui/Builders.hpp:2290-2304`).
- **Consumers**: Application logic that prefers semantic actions over raw ops; examples wire the queue into logging and automation harnesses.
- **Notes**: Queue is optional; reducers also return the action vector directly. Like `WidgetOp`, there is no routing metadata beyond `widget_path`.

## Related device/event entry points
- PathIO adapters expose device event queues at `<device mount>/events` (mouse, keyboard, gamepad) with device-specific payload structs (`src/pathspace/layer/io/PathIOMouse.hpp`, `src/pathspace/layer/io/PathIOKeyboard.hpp`, `src/pathspace/layer/io/PathIOGamepad.cpp`). Declarative widgets ingest these events indirectly through existing input mixers before dispatching `WidgetOp` records.

## Observations for declarative routing contract
- Existing queues assume a single logical consumer; the plan now introduces shared route tables under `<scene>/widgets/runtime/routes/...` plus per-widget defaults to unlock multi-route dispatch without breaking legacy queues.
- Producers rely on atomically incremented `sequence` fields for ordering; any new routing metadata must preserve ordering semantics or document new guarantees.
- Pointer metadata is embedded per entry, but there is no shared payload schema that callers can extend. Adding route descriptors may require augmenting `WidgetOp`/`WidgetAction` or introducing an envelope structure.
- Auto-render requests multiplex disparate reasons in one queue. If declarative routing introduces per-widget event routing, ensure the auto-render queue continues to function (or document a migration path).
- Version flagging under `meta/routing/version` will help migrate existing apps to the richer contract incrementally.
