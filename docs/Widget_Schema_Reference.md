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

## Relationship to `docs/AI_PATHS.md`

`docs/AI_PATHS.md` remains the authoritative map for application, window, scene, renderer, device, and logging namespaces. Rather than repeat that content, this appendix focuses on **widget-specific** nodes and the declarative runtime leaves that hang off the base paths. Use the documents together:

- Consult `AI_PATHS.md` when you need the overall layout (e.g., where `scenes/<id>/builds/<revision>` or `renderers/<rid>/targets/...` live) or when changing non-widget namespaces.
- Use this appendix when editing `widgets/<widget-id>/...`, `widgets/focus/current`, or the declarative lifecycle plumbing (`scene/structure/widgets/*`, `runtime/lifecycle/*`, handler/descriptor nodes).
- When you introduce or retire base nodes (application/window/scene/theme), update `AI_PATHS.md` first, then reference the change here only if there is a widget-facing implication.

### Theme resolver (November 15, 2025)

Declarative descriptors resolve styles by walking `style/theme` from the widget up through parent widgets, the owning window, and finally `/system/applications/<app>/themes/default`. The resolved name passes through `Config::Theme::SanitizeName`, then loads `config/theme/<name>/value`, and the result is cached per (app, theme) pair. Persist literal `meta/style` blobs only when a widget truly needs bespoke values; otherwise rely on the resolver.

## Common widget nodes

These nodes exist under every declarative widget root (`widgets/<widget-id>/…`).

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state` | dir | req | Widget state payload (struct per widget type). |
| `style/theme` | value | opt | Theme override scoped to the widget subtree. |
| `focus/order` | value | rt | Zero-based depth-first order assigned by the focus controller. |
| `focus/disabled` | flag | opt | When true, the widget is skipped during traversal. |
| `focus/wrap` | flag | opt | Containers may opt out of wrap-around traversal (`false`). |
| `focus/current` | flag | rt | Boolean flag indicating the widget currently holds focus. |
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
| `panels/<panel-id>/target` | value | rt | Path to the mounted child fragment. |
| `events/panel_select/handler` | callable | opt | Panel selection handler. |

Descriptor status: Stack widgets now participate in descriptor synthesis so dirty queues stay healthy even though the current bucket is a placeholder. Follow-up work will hydrate layout metadata and render panel chrome so stack buckets no longer return empty geometry.

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

Descriptor status: paint surfaces expose brush metadata + GPU flags through the descriptor so the runtime can track dirty state without special cases. Rendering still depends on the forthcoming paint-buffer integration; until then the descriptor emits an empty bucket and relies on the remaining runtime work to stream stroke textures.

## Handler bindings & descriptors

- Every `events/<event>/handler` leaf stores a `HandlerBinding` struct (`registry_key`, `kind`). Declarative helpers register the lambda in an in-memory registry keyed by `<widget_path>#<event>#<sequence>` and write the binding record into PathSpace. Removing a widget drops every binding with that prefix so stale handlers never fire.
- `render/synthesize` entries store a `RenderDescriptor` (widget kind enum). The runtime converts descriptors + `state/*` + `style/*` into a `WidgetDescriptor` (`Descriptor.hpp`) and synthesizes `DrawableBucketSnapshot` data via the legacy preview builders. Cached buckets live at `render/bucket`, and `SceneSnapshotBuilder` reads them via `structure/widgets/<widget>/render/bucket`.

## Dirty + lifecycle flow

1. Declarative helpers update widget state, flip `render/dirty`, and push the widget path onto `render/events/dirty`.
2. The scene lifecycle trellis (`runtime/lifecycle/trellis`) drains dirty queues, calls into the descriptor synthesizer, writes updated buckets under `scene/structure/widgets/<widget>/render/bucket`, and aggregates those buckets into scene-wide revisions via `SceneSnapshotBuilder`. Snapshot stats mirror under `runtime/lifecycle/metrics` so tooling can inspect `widgets_registered_total`, `sources_active_total`, `widgets_with_buckets`, `events_processed_total`, `last_revision`, `last_published_widget`, `last_published_ms`, and `pending_publish` when throttling kicks in.
3. Theme/focus invalidations: the focus controller now marks declarative widgets dirty whenever `focus/current` flips, and the theme runtime (`Config::Theme::SetActive`) notifies the scene lifecycle worker via `runtime/lifecycle/control`. The worker walks `/widgets` and `/windows/<id>/widgets`, re-enqueues dirty events, and republishes buckets so descriptor caches pick up style changes.
4. Removal is logical (`state/removed = true`), but lifecycle workers immediately deregister trellis sources, evict cached buckets, and clear `scene/structure/widgets/.../render/bucket` so metrics reflect the shrinking tree.
3. Renderer targets consume the updated bucket set, publish a new snapshot, and presenters pick up the fresh revision (window `render/dirty` or per-view dirty bits).
4. Focus controller mirrors the active widget under both `widgets/<id>/focus/current` (boolean) and `structure/window/<window>/focus/current` (absolute widget path) so input + accessibility bridges stay aligned.
5. *Dispatcher integration (November 15, 2025):* `Widgets::Focus::Move(space, config, Direction)` now reads the published `focus/order` metadata directly. Keyboard/gamepad dispatchers simply call the controller with a direction; the helper walks the stored ordering, updates focus flags, and schedules the `focus-navigation` auto-render events without bespoke focus lists.

## Related paths & queues

- `widgets/<id>/ops/inbox/queue` — `WidgetOp` events produced by bindings.
- `widgets/<id>/ops/actions/inbox/queue` — Reduced `WidgetAction` payloads from reducers.
- `<target>/events/renderRequested/queue` — Auto-render requests emitted when widgets mutate.
- `structure/window/<window>/accessibility/dirty` + `widgets/<id>/accessibility/*` — macOS bridge uses these nodes to synchronize with VoiceOver (see `Plan_WidgetDeclarativeAPI.md` for details).

Keep this reference synchronized with the schema headers whenever you add a widget kind, introduce new metadata, or retire legacy nodes.
