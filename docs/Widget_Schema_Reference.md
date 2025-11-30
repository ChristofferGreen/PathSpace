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
- When you introduce or retire base nodes (application/window/scene/theme), update `AI_PATHS.md` first, then refresh the tables here so widget authors do not have to hunt across outdated references.

## Base declarative namespaces

| Node | Base path | Key properties | Notes |
| --- | --- | --- | --- |
| **Application** | `/system/applications/<app>` | `state/title`, `windows/<windowId>`, `scenes/<sceneId>`, `themes/<name>`, `events/lifecycle/handler` | `SP::App::Create` mounts the root, seeds renderer/theme defaults, and records lifecycle handlers. |
| **Window** | `<app>/windows/<windowId>` | `state/title`, `state/visible`, `style/theme`, `views/<viewId>/{scene,surface,renderer,present}`, `widgets/<widgetName>`, `events/{close,focus}/handler`, `render/dirty` | Declarative widget roots mount under `widgets/<name>`; `Window::Create` wires renderer + surface bindings and registers device subscriptions. |
| **Scene** | `<app>/scenes/<sceneId>` | `structure/widgets/<widgetPath>`, `structure/window/<windowId>/{focus,current,metrics/dpi}`, `snapshot/<rev>`, `metrics/*`, `events/present/handler`, `views/<viewId>/dirty`, `state/attached`, `render/dirty` | `Scene::Create` spawns the lifecycle worker that drains `render/events/dirty`, rebuilds buckets, and publishes revisions. |
| **Theme** | `<app>/themes/<name>` | `colors/<token>`, `typography/<token>`, `spacing/<token>`, `style/inherits`, compiled value under `config/theme/<name>/value` | Widgets inherit `style/theme` up the parent → window → app chain before falling back to `/system/themes/_active`; `Theme::{Create,SetColor,RebuildValue}` keep editable + compiled blobs in sync. |

This table mirrors the “Canonical Path Schema” snapshot inside `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md`. Whenever the plan gains a row/column, update both this file and `docs/AI_PATHS.md` so downstream contributors always see the live schema.

### Theme resolver (November 18, 2025)

Declarative descriptors resolve styles by walking `style/theme` from the widget up through parent widgets, the owning window, and finally the global `/system/themes/_active` pointer. The resolved name passes through `SP::UI::Declarative::ThemeConfig::SanitizeName`, then the loader walks `config/theme/<name>/style/inherits` (up to 16 ancestors) until it finds the nearest layer with a `value` payload. Cycles raise `Error::InvalidType`, missing payloads after the walk raise `Error::NoSuchPath`, and edits take effect the next time a descriptor is loaded—no persistent cache needs to be flushed. Persist literal `meta/style` blobs only when a widget truly needs bespoke values; otherwise rely on the resolver + inheritance.

### Theme editing API (November 19, 2025)

`SP::UI::Declarative::Theme::Create` seeds `/system/applications/<app>/themes/<name>` with human-editable nodes and mirrors the compiled `WidgetTheme` into `config/theme/<name>/value`. Callers may supply a seed struct, optional inheritance chain (`style/inherits`), and request activation. `Theme::SetColor` validates canonical tokens, stores the RGBA array under `/themes/<name>/colors/<token>`, rewrites the compiled value, and invokes `SceneLifecycle::InvalidateThemes` so scenes repaint automatically. `Theme::RebuildValue` replays stored tokens onto the serialized struct after manual edits.
Regression coverage: `tests/ui/test_DeclarativeTheme.cpp` now checks both inheritance fallbacks (`Theme::SetColor` across parent/child themes) and the manual-edit repair path (`Theme::RebuildValue`).

Supported color tokens:

| Token | Field |
| --- | --- |
| `button/background`, `button/text` | `WidgetTheme::button.{background_color,text_color}` |
| `toggle/track_off`, `toggle/track_on`, `toggle/thumb` | Toggle colors |
| `slider/track`, `slider/fill`, `slider/thumb`, `slider/label` | Slider palette |
| `list/background`, `list/border`, `list/item`, `list/item_hover`, `list/item_selected`, `list/separator`, `list/item_text` | List palette |
| `tree/background`, `tree/border`, `tree/row`, `tree/row_hover`, `tree/row_selected`, `tree/row_disabled`, `tree/connector`, `tree/toggle`, `tree/text` | Tree palette |
| `text_field/background`, `text_field/border`, `text_field/text`, `text_field/placeholder`, `text_field/selection`, `text_field/composition`, `text_field/caret` | Text input palette |
| `text_area/background`, `text_area/border`, `text_area/text`, `text_area/placeholder`, `text_area/selection`, `text_area/composition`, `text_area/caret` | Text area palette |
| `palette/text_on_light`, `palette/text_on_dark` | Palette control label colors (paint example swatches choose whichever contrast fits the swatch background). |
| `heading/color`, `caption/color`, `accent_text/color`, `muted_text/color` | Theme-level typography accents |

Colors are clamped to `[0.0, 1.0]`, components use lowercase/underscore tokens, and invalid components yield `Error::InvalidPath`.

### Style override masks (November 29, 2025)

Declarative widgets now serialize a bitmask alongside every `meta/style` payload so descriptor loaders know which palette/typography fields were explicitly overridden. The mask lives at `widgets/<id>/meta/style/overrides` (see `ButtonStyle::overrides`, etc. in `WidgetSharedTypes.hpp`) and uses widget-specific enums to label the bits. When a bit is **unset**, descriptors treat the stored value as a placeholder and resolve the final color/typography from the active theme before rendering. When a bit is **set**, the serialized value wins even if it differs from the theme.

> **Serialization note (November 29, 2025):** Fragment helpers now zero unused palette fields (`{0,0,0,0}`) and reset typography blocks to an “inherit” sentinel before writing `/meta/style`. Only structural/layout metrics plus explicitly overridden palette/typography values survive serialization; theme colors are injected later by the descriptor/theme merge. This keeps backing storage lean and makes it trivial to detect override intent.

> **Descriptor merge (November 29, 2025):** `DescriptorDetail::Read*` now pulls the resolved `WidgetTheme` alongside the widget path and fills in any palette/typography fields whose override bits are unset (buttons, toggles, sliders, lists, trees, InputField, and TextArea). Renderer buckets therefore receive fully materialized styles even though `/meta/style` only stores overrides.

> **Event routing (November 29, 2025):** WidgetEventTrellis now resolves the same descriptor/theme pair before computing slider/list/tree geometry or hover math, so pointer + keyboard/gamepad interactions stay in sync with the renderer even when `/meta/style` only stores structural overrides.
> **Test coverage (November 30, 2025):** `tests/ui/test_DeclarativeTheme.cpp` adds slider and toggle descriptor cases (theme defaults plus `style_override()` wins) so every widget with palette overrides now has regression tests guarding the new contract.

Current masks (bit indices are defined in the matching `*StyleOverrideField` enums):

| Widget | Bits |
| --- | --- |
| Button | `background_color`, `text_color`, `typography` |
| Toggle | `track_off_color`, `track_on_color`, `thumb_color` |
| Slider | `track_color`, `fill_color`, `thumb_color`, `label_color`, `label_typography` |
| List | `background_color`, `border_color`, `item_color`, `item_hover_color`, `item_selected_color`, `separator_color`, `item_text_color`, `item_typography` |
| Tree | `background_color`, `border_color`, `row_color`, `row_hover_color`, `row_selected_color`, `row_disabled_color`, `connector_color`, `toggle_color`, `text_color`, `label_typography` |
| TextField | `background_color`, `border_color`, `text_color`, `placeholder_color`, `selection_color`, `composition_color`, `caret_color`, `typography` |
| TextArea | Same bits as `TextField` (`background_color` through `typography`) |

Fragments automatically populate the mask when they serialize a style, so downstream code no longer needs to bake the active theme into the payload just to get correct colors.
When you need bespoke palette/typography values, call the widget’s `Args::style_override()` helper (e.g., `Button::Args::style_override().background_color(...)`) so the matching override bit flips while the serialized blob stays lean.

## Common widget nodes

These nodes exist under every declarative widget root (`widgets/<widget-id>/…`).

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state` | dir | req | Widget state payload (struct per widget type). |
| `state/removed` | flag | opt | Logical removal marker. Runtime helpers leave the subtree intact but lifecycle workers immediately deregister and evict render buckets when this flips true. |
| `style/theme` | value | opt | Theme override scoped to the widget subtree. |
| `focus/order` | value | rt | Zero-based depth-first order assigned by the focus controller. |
| `focus/disabled` | flag | opt | When true, the widget is skipped during traversal. |
| `focus/wrap` | flag | opt | Containers may opt out of wrap-around traversal (`false`). |
| `focus/current` | flag | rt | Boolean flag indicating the widget currently holds focus. |
| `layout/{orientation,spacing,computed/*}` | value | opt/rt | Layout hints plus computed metrics. |
| `children/<child-name>` | dir | opt | Nested widget fragments. |
| `ops/inbox/queue` | queue | rt | Reducer input queue used by the declarative runtime when it needs to mutate widget-local state ahead of handler dispatch. |
| `ops/actions/inbox/queue` | queue | rt | Reducer output queue; InputTask mirrors every action into the canonical event queues before resolving handlers. |
| `events/<event>/handler` | callable | opt | Stores `HandlerBinding { registry_key, kind }`; runtime resolves registry ids to lambdas. Fragment specs carry handler metadata so `Widgets::Mount` re-registers them automatically, and scenes can override a binding with `Widgets::Handlers::{Replace,Wrap,Restore}` without touching the raw path. |
| `events/inbox/queue` | queue | rt | Canonical `WidgetAction` stream populated by the declarative runtime. |
| `events/<event>/queue` | queue | opt | Optional filtered queue (press/toggle/change/etc.) for tooling that only needs a single event family. |
| `metrics/handlers/{invoked_total,failures_total,missing_total}` | value | rt | Per-widget handler telemetry recorded by the input runtime. |
| `metrics/focus/{acquired_total,lost_total}` | value | rt | Counters tracking how often the widget gains or loses focus; useful for spotting flapping focus targets. |
| `metrics/history_binding/*` | value | opt/rt | Present when `HistoryBinding` wraps the widget; records readiness, button enablement, undo/redo counters, and the serialized telemetry card. |
| `render/synthesize` | value | req | `RenderDescriptor` describing the widget kind. |
| `render/bucket` | value | rt | Cached `DrawableBucketSnapshot` rebuilt when `render/dirty` flips. |
| `render/dirty` | flag | rt | Raised by helpers whenever state/style changes. |
| `render/events/dirty` | queue | rt | Widget-path FIFO consumed by the scene lifecycle trellis. |
| `render/buffer/pendingDirty` | value | rt | Coalesced `DirtyRectHint` list flushed into `targets/<tid>/hints/dirtyRects` once SceneLifecycle stores the refreshed bucket. |
| `log/events` | queue | rt | Per-widget diagnostic log; the InputTask mirrors handler failures, reducer errors, enqueue drops, and slow-handler warnings (> configurable threshold) into this queue so tooling can audit misbehaving widgets without scraping global logs. |

All widgets also inherit the global queues documented elsewhere (e.g., `widgets/<id>/ops/inbox/queue`, `ops/actions/inbox/queue`) plus the auto-render queue under the window target.

### Composition rules & fragments

- Treat every `WidgetFragment` as a fully-instantiated subspace. Mounting a fragment copies its subtree under `children/<name>`, rewrites handler bindings for the destination path, and re-registers callable nodes inside the handler registry so callbacks keep working.
- Parent overrides win: when both the parent and the fragment define `events/<event>/handler`, `Widgets::Mount` preserves the parent handler unless the caller explicitly replaces it via the arg struct.
- Containers that accept child fragments (`List`, `Tree`, `Stack`, paint controls, etc.) enumerate the fragment’s `children/*` and mount each child in order. Mount operations mark the parent `render/dirty`, enqueue `render/events/dirty`, and queue a synthetic auto-render event so new widgets appear immediately.
- Undoable widgets should colocate all history-affecting nodes under the same root before enabling `HistoryBinding`. The helper sets up `UndoableSpace` mirrors, seeds telemetry, and toggles undo/redo buttons via `SetHistoryBindingButtonsEnabled`.

### Dirty propagation & lifecycle

1. Declarative helpers rewrite widget state and flip `render/dirty`; they also push the widget path into `render/events/dirty` so the lifecycle trellis notices the change.
2. `SceneLifecycle::PumpSceneOnce` (manual) or the background lifecycle worker drains dirty queues, synthesizes `WidgetDescriptor` objects, rebuilds buckets, and records metrics under `runtime/lifecycle/metrics/*`.
3. Updated buckets publish under `scene/structure/widgets/<widget>/render/bucket` and flow into `SceneSnapshotBuilder` revisions. Snapshot metadata mirrors to `scenes/<scene>/builds/<revision>/bucket/*` plus the summary files consumed by presenter targets.
4. Renderer targets get notified via the normal dirty hint queue; presenters read `scene/runtime/focus/*`, `structure/window/<window>/focus/current`, and `renderers/<rid>/targets/<name>/output/*` to stay in sync. Focus updates also flip `widgets/<id>/focus/current` (boolean) and `structure/window/<window>/focus/current` (path string) so accessibility and inputs share the same data.

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
| `panels/<panel-id>/order` | value | rt | Stable ordering written during mount so layout recomputes deterministically. |
| `panels/<panel-id>/state` | dir | rt | Panel metadata (title, enabled, loaded). |
| `panels/<panel-id>/target` | value | rt | Path to the mounted child fragment. |
| `panels/<panel-id>/visible` | value | rt | Boolean flag managed by `Stack::SetActivePanel`; mirrors whether the panel is currently shown. |
| `events/panel_select/handler` | callable | opt | Panel selection handler. |
| `layout/style` | value | rt | Serialized `Widgets::StackLayoutStyle` (axis, spacing, padding). |
| `layout/children` | value | rt | `std::vector<StackChildSpec>` describing child widget paths + constraints. |
| `layout/computed` | value | rt | Cached `StackLayoutState` produced by the declarative runtime so descriptors/previews can reuse measured bounds. |

Descriptor status: Stack descriptors now emit full `StackLayoutStyle` + `StackLayoutState` metadata and reuse the builder preview to render background + panel placeholders. The active panel receives a highlighted overlay derived from the stored layout bounds, so stack buckets remain meaningful even before child widgets render.

Runtime hit tests now emit `StackSelect` widget ops when a user activates a rendered preview (`stack/panel/<panel-id>`). Those ops flow through `/widgets/<id>/ops/inbox/queue`, mirror into `events/panel_select/queue`, and invoke the optional handler with `panel_id` populated so containers can react without bespoke routing tables.

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

Runtime note (Nov 19, 2025): Focused input fields now receive synthesized `TextDelete`, `TextMoveCursor`, and `TextSubmit` `WidgetOp`s directly from WidgetEventTrellis whenever keyboards or gamepads dispatch delete/arrow/submit events. The runtime mutates `state/text`, updates `cursor`/selection fields, flips `render/dirty`, and mirrors the ops into both `events/inbox/queue` and the typed queues (`events/change/queue`, `events/submit/queue`) before resolving the registered handlers, so declarative apps no longer need bespoke glue for cursor/edit parity.

### Paint Surface

| Path | Kind | Req | Notes |
| --- | --- | --- | --- |
| `state/brush/{size,color}` | value | opt | Brush metadata for new strokes. |
| `state/stroke_mode` | value | opt | Draw, erase, or flood mode. |
| `state/history/<stroke-id>` | dir | rt | Ordered stroke history (UndoableSpace-backed). |
| `state/history/<stroke-id>/meta` | value | rt | Stroke metadata (brush size, color, commit flag). |
| `state/history/<stroke-id>/points` | value | rt | Widget-local points captured for the stroke. |
| `state/history/<stroke-id>/version` | value | rt | Monotonic counter incremented every time meta/points are rewritten to guarantee consistent descriptor reads. |
| `state/history/last_stroke_id` | value | rt | Last stroke identifier processed by the runtime. |
| `render/buffer` | value | rt | CPU-readable paint buffer. |
| `render/buffer/metrics/{width,height,dpi}` | value | rt | Buffer metrics derived from layout × DPI. |
| `render/buffer/viewport` | value | rt | Visible rect when buffer > layout. |
| `render/buffer/revision` | value | rt | Monotonic counter incremented whenever stroke data mutates. |
| `render/gpu/{enabled,state,dirtyRects,fence/start,fence/end,log/events,stats}` | mixed | opt/rt | GPU staging controls + diagnostics. `state` cycles through `Idle`, `DirtyPartial`, `DirtyFull`, `Uploading`, `Ready`, `Error`; `dirtyRects` queues `DirtyRectHint`s for the uploader, `fence/*` capture the last upload timestamps, and `stats` mirrors upload counters/bytes/revisions. |
| `assets/texture` | value | rt | Serialized `PaintTexturePayload` (width, height, stride, revision, RGBA8 pixels) written by the paint GPU uploader. |
| `events/draw/handler` | callable | opt | Draw handler translating pointer events into strokes. |

Descriptor status: paint surfaces expose brush metadata, buffer metrics, viewport and revision info, recorded stroke paths, and GPU staging metadata through the descriptor so the runtime can synthesize background quads plus `DrawCommandKind::Stroke` buckets. When `render/gpu/state == Ready`, the uploader also writes `assets/texture` for future image-command use.

`PaintStrokeBegin`, `PaintStrokeUpdate`, and `PaintStrokeCommit` ops now populate the paint surface action queue with pointer-local coordinates. The runtime forwards each op to `events/draw/queue`, appends points under `state/history/<id>/{meta,points}` (bumping `state/history/<id>/version` after every write), increments `render/buffer/revision`, records stroke footprints under `render/buffer/pendingDirty` + `/render/gpu/dirtyRects`, flips `render/gpu/state` to `DirtyPartial`, and invokes the optional `draw` handler so reducers can append custom behavior or enqueue uploads manually if desired.

Runtime note (Nov 25, 2025): `SP::System::LaunchStandard` now starts the paint GPU uploader by default. The worker scans `/system/applications/*/{windows,widgets}` for GPU-enabled paint surfaces, drains `/render/gpu/dirtyRects`, replays stroke history via `PaintRuntime::LoadStrokeRecords`, and writes `assets/texture` (serialized `PaintTexturePayload` with width/height/stride/revision). Upload telemetry lives per widget under `render/gpu/stats` (uploads, partial/full counters, last bytes/duration/revision) and globally under `/system/widgets/runtime/paint_gpu/metrics/{uploads_total,partial_uploads_total,full_uploads_total,failures_total,widgets_pending,last_upload_ns}`. Failures are appended to `/system/widgets/runtime/paint_gpu/log/errors/queue` as well as `render/gpu/log/events`. Presenters bind the staged texture whenever `render/gpu/state == Ready`; otherwise they continue sampling the CPU buffer. Use `examples/paint_example --gpu-smoke` or `tests/ui/test_DeclarativePaintSurface.cpp` to validate the staging path.

Descriptors, SceneLifecycle, and tooling read paint history through the versioned API (`PaintRuntime::ReadStrokePointsConsistent`), which re-reads `state/history/<id>/version` before/after grabbing `points`. If the version changes mid-read the helper retries, guaranteeing that cached buckets only reference stroke points that actually exist.

**Startup guard (Nov 24, 2025 — updated Nov 26):** Declarative demos must wait for lifecycle metrics, scene structure, and at least one published revision before presenting or running screenshot loops. Use `PathSpaceExamples::ensure_declarative_scene_ready` (from `examples/declarative_example_shared.hpp`) right after mounting widgets; it counts `/windows/<id>/views/<view>/widgets/*`, polls `/runtime/lifecycle/metrics/widgets_with_buckets`, waits for `/scenes/<scene>/structure/widgets/windows/<window>/views/<view>/widgets`, and optionally blocks on `/scenes/<scene>/current_revision` + `/builds/<rev>/bucket/drawables.bin`. The helper now accepts `scene_window_component_override`, `scene_view_override`, and `ensure_scene_window_mirror` so doctests with non-default naming can point it at the right structure branch before waiting. When `force_scene_publish = true` + `pump_scene_before_force_publish` (default), it issues a manual `SceneLifecycle::PumpSceneOnce` before each forced publish so initial buckets exist even on fresh builds, then calls `SceneLifecycle::ForcePublish` with the caller’s `min_revision`/timeout to return a deterministic revision. All samples now call this helper before presenting so empty-window flashes stay gone and future demos inherit the same contract. UITests that need deterministic progress while the global worker is busy can call `SP::UI::Declarative::PumpWindowWidgetsOnce` and watch `/system/widgets/runtime/input/windows/<token>/metrics/*` / `/system/widgets/runtime/input/apps/<app>/metrics/*` to prove their widgets were serviced.

## Handler bindings & descriptors

- Every `events/<event>/handler` leaf stores a `HandlerBinding` struct (`registry_key`, `kind`). Declarative helpers register the lambda in an in-memory registry keyed by `<widget_path>#<event>#<sequence>` and write the binding record into PathSpace. Removing a widget drops every binding with that prefix so stale handlers never fire. Fragments can bundle handler specs and `Widgets::Mount` rewrites them for the destination path, while instrumentation layers can call `Widgets::Handlers::Read/Replace/Wrap/Restore` to observe, override, or restore bindings without hand-editing the schema.
- `render/synthesize` entries store a `RenderDescriptor` (widget kind enum). The runtime converts descriptors + `state/*` + `style/*` into a `WidgetDescriptor` (`Descriptor.hpp`) and synthesizes `DrawableBucketSnapshot` data via the legacy preview builders. Cached buckets live at `render/bucket`, and `SceneSnapshotBuilder` reads them via `structure/widgets/<widget>/render/bucket`.

## Dirty + lifecycle flow

1. Declarative helpers update widget state, flip `render/dirty`, and push the widget path onto `render/events/dirty`.
2. The scene lifecycle trellis (`runtime/lifecycle/trellis`) drains dirty queues, calls into the descriptor synthesizer, writes updated buckets under `scene/structure/widgets/<widget>/render/bucket`, and aggregates those buckets into scene-wide revisions via `SceneSnapshotBuilder`. Snapshot stats mirror under `runtime/lifecycle/metrics` so tooling can inspect `widgets_registered_total`, `sources_active_total`, `widgets_with_buckets`, `events_processed_total`, `last_revision`, `last_published_widget`, `last_published_ms`, and `pending_publish` when throttling kicks in.
3. Theme/focus invalidations: the focus controller now marks declarative widgets dirty whenever `focus/current` flips, and the theme runtime (`SP::UI::Declarative::ThemeConfig::SetActive`) notifies the scene lifecycle worker via `runtime/lifecycle/control`. The worker walks `/widgets` and `/windows/<id>/widgets`, re-enqueues dirty events, and republishes buckets so descriptor caches pick up style changes.
4. Removal is logical (`state/removed = true`), but lifecycle workers immediately deregister trellis sources, evict cached buckets, and clear `scene/structure/widgets/.../render/bucket` so metrics reflect the shrinking tree.
3. Renderer targets consume the updated bucket set, publish a new snapshot, and presenters pick up the fresh revision (window `render/dirty` or per-view dirty bits).
4. Focus controller mirrors the active widget under both `widgets/<id>/focus/current` (boolean) and `structure/window/<window>/focus/current` (absolute widget path) so input + accessibility bridges stay aligned.
5. *Dispatcher integration (November 15, 2025):* `Widgets::Focus::Move(space, config, Direction)` now reads the published `focus/order` metadata directly. Keyboard/gamepad dispatchers simply call the controller with a direction; the helper walks the stored ordering, updates focus flags, and schedules the `focus-navigation` auto-render events without bespoke focus lists.

## Related paths & queues

- `widgets/<id>/ops/inbox/queue` — `WidgetOp` events produced by bindings.
- `widgets/<id>/ops/actions/inbox/queue` — Reduced `WidgetAction` payloads from reducers.
- `widgets/<id>/events/inbox/queue` — Canonical event queue emitted by the declarative runtime (same payload as `WidgetAction`).
- `widgets/<id>/events/<event>/queue` — Filtered queues scoped to a handler name (press, toggle, change, child_event, node_event, submit, draw, etc.).
- `<target>/events/renderRequested/queue` — Auto-render requests emitted when widgets mutate.
- `structure/window/<window>/accessibility/dirty` + `widgets/<id>/accessibility/*` — macOS bridge uses these nodes to synchronize with VoiceOver (see `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md` for details).

Keep this reference synchronized with the schema headers whenever you add a widget kind, introduce new metadata, or retire legacy nodes.
