# Widget Schema Reference

> **Update (November 15, 2025):** First publication of the declarative widget schema reference.
> Source of truth: `include/pathspace/ui/declarative/Schema.hpp` and `Descriptor.hpp`.

This appendix summarizes the canonical nodes, handlers, and runtime-managed leaves that declarative widgets rely on. Use it alongside `docs/AI_PATHS.md` when authoring widgets, debugging schema issues, or reviewing plan milestones.

## Reading the tables

| Column | Meaning |
| --- | --- |
| **Path** | Path component relative to the namespace root (application, window, scene, or widget root). Angle brackets denote caller-supplied identifiers. |
| **Kind** | `dir`, `value`, `callable`, `queue`, or `flag` (mirrors `NodeKind`). |
| **Req** | `req` (required), `opt` (optional), or `rt` (runtime-managed). |
| **Notes** | Human-readable description plus cross-references to runtimes or diagnostics. |

Unless noted otherwise, all paths are application-relative and live under `/system/applications/<app>/…`.

## Namespaces

### Application namespace

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/title` | value | req | Human-readable title stored by `SP::App::Create`. |
| `windows/<window-id>` | dir | rt | Declarative window namespaces mounted under the app. |
| `scenes/<scene-id>` | dir | rt | Declarative scenes created via `SP::Scene::Create`. |
| `themes/default` | value | opt | Default widget theme (string id). |
| `themes/<theme>` | dir | opt | Theme definitions; see Theme namespace below. |
| `events/lifecycle/handler` | callable | opt | App-level lifecycle handler (optional). |

### Window namespace

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/title` | value | req | Mirrored into OS windows. |
| `state/visible` | flag | rt | Controlled by runtime presenters. |
| `style/theme` | value | opt | Theme override for the entire window subtree. |
| `widgets/<widget-id>` | dir | rt | Widget roots attached to the window. |
| `events/{close,focus}/handler` | callable | opt | Optional handlers for close/focus events. |
| `render/dirty` | flag | rt | Presenter uses this to schedule redraws. |

### Scene namespace

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `structure/widgets/<widget-path>` | dir | rt | Snapshot buckets consumed by renderers. |
| `structure/window/<window-id>/{focus/current,metrics/dpi,accessibility/dirty}` | value/flag | rt | Focus, DPI, and accessibility mirrors per window. |
| `snapshot/<revision>` | dir | rt | Immutable scene revision artifacts. |
| `snapshot/current` | value | rt | Pointer to current revision. |
| `metrics/<metric>` | value | rt | Scene diagnostics (render ms, residency, etc.). |
| `events/present/handler` | callable | opt | Handler invoked when the scene presents. |
| `views/<view-id>/dirty` | flag | rt | Per-view dirty bit to avoid cross-window stalls. |
| `state/attached` | flag | rt | Indicates whether the scene is attached to a presenter. |
| `render/dirty` | flag | rt | Triggers scene re-synthesis. |
| `runtime/lifecycle/trellis` | dir | rt | PathSpaceTrellis worker fanning in widget dirty queues. |

### Theme namespace

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `colors/<token>` | value | req | Color tokens referenced by widgets. |
| `typography/<token>` | value | opt | Typography tokens. |
| `spacing/<token>` | value | opt | Spacing tokens. |
| `style/inherits` | value | opt | Parent theme id for inheritance. |

## Common widget nodes

These nodes exist under every declarative widget root (`widgets/<widget-id>/…`).

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state` | dir | req | Widget state payload (struct per widget type). |
| `style/theme` | value | opt | Theme override scoped to the widget subtree. |
| `focus/{order,disabled,wrap,current}` | value/flag | mixed | Focus metadata managed by the runtime. |
| `layout/{orientation,spacing,computed/*}` | value | opt/rt | Layout hints plus computed metrics. |
| `children/<child-name>` | dir | opt | Nested widget fragments. |
| `events/<event>/handler` | callable | opt | Stores `HandlerBinding { registry_key, kind }`; runtime resolves registry ids to lambdas. |
| `render/synthesize` | value | req | `RenderDescriptor` describing the widget kind. |
| `render/bucket` | value | rt | Cached `DrawableBucketSnapshot` rebuilt when `render/dirty` flips. |
| `render/dirty` | flag | rt | Raised by helpers whenever state/style changes. |
| `render/events/dirty` | queue | rt | Widget-path FIFO consumed by the scene lifecycle trellis. |
| `log/events` | queue | rt | Diagnostics for handler failures, staging errors, etc. |

All widgets also inherit the global queues documented elsewhere (e.g., `widgets/<id>/ops/inbox/queue`, `ops/actions/inbox/queue`) plus the auto-render queue under the window target.

## Widget-specific nodes

### Button

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/label` | value | req | Localized button label. |
| `state/enabled` | flag | opt | Disables bindings when false. |
| `events/press/handler` | callable | opt | Button activation handler. |

### Toggle

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/checked` | flag | req | Toggle state. |
| `events/toggle/handler` | callable | opt | Toggle handler. |

### Slider

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/value` | value | req | Current slider value (float). |
| `state/range/{min,max}` | value | req | Range metadata (min/max, optional `step`). |
| `state/dragging` | flag | rt | Runtime-managed drag state. |
| `events/change/handler` | callable | opt | Fired when value changes. |

### List

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `layout/{orientation,spacing}` | value | opt | Layout hints for children. |
| `state/scroll_offset` | value | rt | Scroll position. |
| `events/child_event/handler` | callable | opt | Notified for row activation/selection. |
| `children/<child-name>` | dir | req | Nested widget fragments per row. |

### Tree

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `nodes/<node-id>/state` | dir | rt | Node metadata (expanded, selected, enabled). |
| `nodes/<node-id>/children` | dir | rt | Nested node ids. |
| `events/node_event/handler` | callable | opt | Fired for expand/collapse/select flows. |

### Stack (gallery/panel switcher)

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/active_panel` | value | req | Currently visible panel id. |
| `panels/<panel-id>/state` | dir | rt | Panel metadata (title, enabled, loaded). |
| `events/panel_select/handler` | callable | opt | Panel selection handler. |

### Label

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/text` | value | req | Text payload (UTF‑8). |
| `events/activate/handler` | callable | opt | Optional activation handler for interactive labels. |

### Input Field

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/text` | value | req | Current text contents. |
| `state/placeholder` | value | opt | Placeholder text. |
| `state/focused` | flag | rt | Updated by focus controller + input pipeline. |
| `events/{change,submit}/handler` | callable | opt | Text change + submit handlers. |

### Paint Surface

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/brush/{size,color}` | value | opt | Brush metadata for new strokes. |
| `state/stroke_mode` | value | opt | Draw, erase, or flood mode. |
| `state/history/<stroke-id>` | dir | rt | Ordered stroke history (UndoableSpace-backed). |
| `render/buffer` | value | rt | CPU-readable paint buffer. |
| `render/buffer/metrics/{width,height,dpi}` | value | rt | Buffer metrics derived from layout × DPI. |
| `render/buffer/viewport` | value | rt | Visible rect when buffer > layout. |
| `render/gpu/{enabled,state,dirtyRects,fence/start,fence/end,log/events,stats}` | mixed | opt/rt | GPU staging controls + diagnostics. |
| `assets/texture` | value | rt | GPU texture resource (when staging enabled). |
| `events/draw/handler` | callable | opt | Draw handler translating pointer events into strokes. |

## Handler bindings & descriptors

- Every `events/<event>/handler` leaf stores a `HandlerBinding` struct (`registry_key`, `kind`). Declarative helpers register the lambda in an in-memory registry keyed by `<widget_path>#<event>#<sequence>` and write the binding record into PathSpace. Removing a widget drops every binding with that prefix so stale handlers never fire.
- `render/synthesize` entries store a `RenderDescriptor` (widget kind enum). The runtime converts descriptors + `state/*` + `style/*` into a `WidgetDescriptor` (`Descriptor.hpp`) and synthesizes `DrawableBucketSnapshot` data via the legacy preview builders. Cached buckets live at `render/bucket`, and `SceneSnapshotBuilder` reads them via `structure/widgets/<widget>/render/bucket`.

## Dirty + lifecycle flow

1. Declarative helpers update widget state, flip `render/dirty`, and push the widget path onto `render/events/dirty`.
2. The scene lifecycle trellis (`runtime/lifecycle/trellis`) drains dirty queues, calls into the descriptor synthesizer, and writes updated buckets under `scene/structure/widgets/<widget>/render/bucket`.
3. Renderer targets consume the updated bucket set, publish a new snapshot, and presenters pick up the fresh revision (window `render/dirty` or per-view dirty bits).
4. Focus controller mirrors the active widget under both `widgets/<id>/focus/current` and `structure/window/<window>/focus/current` so input + accessibility bridges stay aligned.

## Related paths & queues

- `widgets/<id>/ops/inbox/queue` — `WidgetOp` events produced by bindings.
- `widgets/<id>/ops/actions/inbox/queue` — Reduced `WidgetAction` payloads from reducers.
- `<target>/events/renderRequested/queue` — Auto-render requests emitted when widgets mutate.
- `structure/window/<window>/accessibility/dirty` + `widgets/<id>/accessibility/*` — macOS bridge uses these nodes to synchronize with VoiceOver (see `Plan_WidgetDeclarativeAPI.md` for details).

Keep this reference synchronized with the schema headers whenever you add a widget kind, introduce new metadata, or retire legacy nodes.
