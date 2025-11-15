# Handoff Notice

> **Handoff note (October 19, 2025):** Namespace conventions remain valid, but this file reflects the prior assistant cycle. Cross-check any edits with `docs/AI_Onboarding_Next.md` and record deviations while the new assistant spins up.

# PathSpace — Standard Paths

> **Context update (October 15, 2025):** Path conventions documented here reflect the recently introduced assistant context; translate earlier terminology to the updated vocabulary as needed.

This file gathers the canonical path namespaces and layout conventions referenced across the docs. Paths are grouped by domain and use placeholders in angle brackets. Absolute paths begin with “/”. App-internal paths are app-relative (no leading slash) and must resolve within the app root. Sources: see docs/AI_Architecture.md and docs/Plan_SceneGraph_Renderer.md. Keep these in sync when path conventions change.

Conventions:
- Placeholders: <app>, <user>, <scene-id>, <renderer-id>, <surface-id>, <view-id>, <target-id>, <revision>, <id>, <rendererName>, <kind>, <name>
- App-relative: no leading “/”; resolved against the app root
- Target-id convention: prefer the consumer’s app-local path, e.g. `surfaces/<surface-id>` or `textures/<texture-name>`

## 1) System-wide namespaces (absolute)

- Application roots
  - `/system/applications/<app>`
  - `/users/<user>/system/applications/<app>`

- Device IO
  - Inputs (event queues):
    - `/system/devices/in/pointer/default/events`
    - `/system/devices/in/text/default/events`
    - `/system/devices/in/gamepad/default/events`
  - Discovery (recommended mount):
    - `/system/devices/discovery`
  - Haptics (outputs):
    - `/system/devices/out/gamepad/<id>/rumble`
- System IO (logs)
  - `/system/io/stdout`
  - `/system/io/stderr`
- Declarative runtime (global services)
  - `/system/widgets/runtime/input/state/running` — bool flag toggled by `SP::System::LaunchStandard`/`ShutdownDeclarativeRuntime`.
  - `/system/widgets/runtime/input/metrics/{widgets_processed_total,widgets_with_work_total,actions_published_total,last_pump_ns}`
  - `/system/widgets/runtime/input/log/errors/queue` — string queue capturing reducer/pump failures.

## 2) Application subtree layout (app-relative)

The following subtrees are standardized within each application root (one of the absolute roots above). See docs/Plan_SceneGraph_Renderer.md for detailed semantics and responsibilities.

- Scenes
  - `scenes/<scene-id>/`
    - `src/...` — authoring tree (mutable)
    - `builds/<revision>/...` — immutable snapshots per revision
    - `current_revision` — single-value pointer to latest published build
    - `diagnostics/`
      - `errors/live` — latest `PathSpaceError` for the scene
      - `errors/history/<id>` — optional immutable historical errors
      - `log/ring/<segment>` — structured log segments (JSON/CBOR blobs)
      - `metrics/live` — rolling scene metrics (optional)
      - `overlays/<kind>` — optional debugging overlays (e.g., bounds violations)
      - `frame_profiler/node/<drawable-id>` — drawable-specific annotations
      - `frame_profiler/summary` — scene-level diagnostics counters and timestamps
      - `dirty/state` — latest `Scene::DirtyState` (sequence, pending mask, timestamp)
      - `dirty/queue` — `Scene::DirtyEvent` items enqueued by `Scene::MarkDirty`

- Renderers
  - `renderers/<renderer-id>/`
    - `caps`
    - `targets/`
      - `<target-id>/`
        - `scene` — app-relative path to the scene to render
        - `desc` — target descriptor (size/format/etc.)
        - `desc/active` — optional mirror written by renderer
        - `status/*` — e.g. `reconfiguring`, `device_lost`, `message`
        - `settings` — single `RenderSettingsV1` value (atomic whole-object replace)
        - `render` — execution: render one frame for this target
        - `output/`
          - `v1/`
            - `common/`
              - `frameIndex`
              - `revision`
              - `renderMs`
              - `lastError`
            - `software/framebuffer` — pixels + stride
            - `metal/texture` — opaque GPU handle + metadata
            - `vulkan/image` — opaque GPU handle + metadata
            - `html/dom`
            - `html/commands`
            - `html/assets/*`
    - `diagnostics/`
      - `errors/live` — latest `PathSpaceError` for the renderer
      - `errors/history/<id>` — optional immutable error records
      - `log/ring/<segment>` — structured log segments
      - `metrics/live` — rolling averages and counters for recent frames
      - `overlays/<kind>` — debug overlay payloads (e.g., errors, overdraw)
      - `frame_profiler/live` — latest frame sample (struct payload)
      - `frame_profiler/ring` — immutable ring buffer chunks for timeline data
      - `frame_profiler/capture/`
        - `inbox` — capture requests inserted by external profilers
        - `active` — renderer acknowledgement + status metadata
        - `dump/*` — optional blobs captured for that request (framebuffers, GPU counters)
- Widgets
  - `widgets/<widget-id>/`
    - `state` — current widget state payload (button/toggle/slider/list/tree structs)
    - `meta/*` — style, label, range/list/tree metadata authored by widget builders
    - `meta/nodes` — tree view node metadata (`id`, `parent_id`, `label`, `enabled`, `expandable`, `loaded`)
    - `layout/style` — layout container style (axis, spacing, alignment)
    - `layout/children` — layout child specs (widget path + weight/constraints)
    - `layout/computed` — latest measured bounds per child (layout containers)
    - `authoring/<component>` — canonical authoring nodes written by widget builders and consumed by hit testing (`Widgets::ResolveHitTarget`)
    - `ops/actions/inbox/queue` — reduced widget actions emitted by `Widgets::Reducers`
    - `ops/inbox/queue` — `WidgetOp` FIFO written by `Widgets::Bindings` helpers (hover/press/toggle/slider events)
  - `widgets/focus/current` — current focused widget name written by focus helpers
  - `scenes/widgets/<widget-id>/`
    - `states/<state-name>/snapshot` — immutable per-state display list
    - `current_revision` — revision pointer adopted by renderer targets
    - `meta/{name,description}` — human-readable labels stamped on first publish

### Declarative widget API (November 14, 2025 — schema appendix refreshed November 15, 2025)

See `docs/Widget_Schema_Reference.md` for the authoritative per-node tables (mirrors `include/pathspace/ui/declarative/Schema.hpp` + `Descriptor.hpp`). The summary below highlights the most frequently accessed nodes so path authors stay oriented without flipping between documents.

Canonical schema definitions for the declarative workflow live in `include/pathspace/ui/declarative/Schema.hpp` and surface through `SP::UI::Declarative::widget_schemas()`. The namespace overlays are implemented in `src/pathspace/ui/declarative/Schema.cpp`.

- Namespaces
  - `application` — `state/title`, `windows/<window-id>`, `scenes/<scene-id>`, `themes/default`, `events/lifecycle/handler`
- `window` — `state/title`, `state/visible`, `style/theme`, `widgets/<widget-name>`, `events/{close,focus}/handler`, `render/dirty`
- Runtime bootstrap (November 14, 2025): `SP::System::LaunchStandard` + `SP::Window::Create` now seed `state/visible`, `render/dirty`, and `views/<view>/scene|surface|htmlTarget` so declarative scenes can rely on these leaves before wiring presenters.
- `scene` — `structure/widgets/<widget-path>`, `structure/window/<window-id>/{focus/current,metrics/dpi,surface,renderer,present}`, `views/<view-id>/dirty`, `snapshot/<revision>`, `state/attached`, `render/dirty`
- Runtime bootstrap (November 14, 2025): `SP::Scene::Create` populates `structure/window/<window-id>/*`, `state/attached`, and marks the bound window view so focus/metrics/rendering scaffolding exists prior to widget mounts.
  - Lifecycle worker (November 15, 2025): each scene mounts `runtime/lifecycle/trellis` (a `PathSpaceTrellis`) that fans in widget dirty queues (`widgets/.../render/events/dirty`). Worker state/metrics live under `runtime/lifecycle/state/*` and `runtime/lifecycle/metrics/*`; control events are published to `runtime/lifecycle/control` so `SP::Scene::Shutdown` can stop the worker without leaking threads.
  - `theme` — `colors/<token>`, `typography/<token>`, `spacing/<token>`, `style/inherits`

**Declarative theme resolution (November 15, 2025).** Descriptor synthesis now resolves themes without persisting `meta/style` copies:
- Start at the widget root and look for `style/theme`. Walk up through parent widgets (`.../children/...`), then the owning window (`/windows/<window>/style/theme`), and finally `/system/applications/<app>/themes/default`.
- The resolved name passes through `Config::Theme::SanitizeName` and loads `config/theme/<name>/value`. Results are cached per (app root, theme) pair so repeated descriptor loads avoid redundant PathSpace reads.
- Empty/missing values fall back to the literal `"default"`, ensuring every widget can hydrate styles even before the theme editing API ships. Future work will layer per-theme `style/inherits` on top of this resolver.

- Common widget nodes (applies to every declarative widget)
  - `state` — widget state payload exposed to applications
  - `style/theme` — theme override bound to the subtree
  - `focus/order`, `focus/disabled`, `focus/wrap`, `focus/current` — focus traversal metadata
  - `layout/orientation`, `layout/spacing`, `layout/computed/*` — layout hints and computed metrics
  - `children/<child-name>` — child widget mounts
  - `events/<event>/handler` — `HandlerBinding { registry_key, kind }` referencing the declarative callback registry
  - `render/synthesize`, `render/bucket`, `render/dirty` — render cache contract (descriptor + cached bucket + dirty flag)
  - `render/events/dirty` — queue of widget-path strings published whenever declarative helpers mark the widget dirty; consumed by the scene lifecycle trellis worker.
  - `log/events` — runtime diagnostics queue

| Widget | Authored nodes | Runtime-managed nodes |
|---|---|---|
| button | `state/label`, `state/enabled`, `events/press/handler` | `render/bucket`, `render/dirty` |
| toggle | `state/checked`, `events/toggle/handler` | `render/bucket`, `render/dirty` |
| slider | `state/value`, `state/range/{min,max}`, `events/change/handler` | `state/dragging`, `render/bucket`, `render/dirty` |
| list | `layout/orientation`, `layout/spacing`, `events/child_event/handler` | `state/scroll_offset` |
| tree | `events/node_event/handler` | `nodes/<node-id>/state`, `nodes/<node-id>/children` |
| stack | `state/active_panel`, `events/panel_select/handler` | `panels/<panel-id>/state` |
| label | `state/text`, `events/activate/handler` | `render/bucket`, `render/dirty` |
| input_field | `state/text`, `state/placeholder`, `events/{change,submit}/handler` | `state/focused`, `render/dirty` |
| paint_surface | `state/brush/*`, `events/draw/handler` | `state/history/<stroke-id>`, `render/buffer/*`, `render/gpu/*`, `assets/texture` |

**Handler bindings.** Each `events/<event>/handler` leaf stores a `HandlerBinding { registry_key, kind }`. Declarative helpers register lambdas in an in-memory registry keyed by `<widget_path>#<event>#<sequence>` and write the binding record into PathSpace. Removing a widget removes every binding sharing the prefix so stale handlers never fire.

**Widget relocation (November 15, 2025).** `Widgets::Move` now re-homes an existing widget subtree by moving the trie nodes in-place, re-registering handler bindings for the new path, and marking both the widget and its parents dirty so the lifecycle trellis rebuilds buckets on the next pass. No schema translation or fragment re-mounting is required when containers shuffle children at runtime.

**Render descriptors.** `render/synthesize` holds a `RenderDescriptor` (widget kind enum). The runtime converts descriptors + `state/*` + `style/*` into a `WidgetDescriptor` and synthesizes `DrawableBucketSnapshot` data via the legacy preview builders. Cached buckets live at `render/bucket`, and `SceneSnapshotBuilder` reads them via `structure/widgets/<widget>/render/bucket`.

**Dirty + lifecycle flow.**
1. Declarative helpers update widget state, flip `render/dirty`, and push the widget path onto `render/events/dirty`.
2. The scene lifecycle trellis (`runtime/lifecycle/trellis`) drains dirty queues, calls into the descriptor synthesizer, and writes updated buckets under `scene/structure/widgets/<widget>/render/bucket`.
3. Renderer targets consume the updated bucket set, publish a new snapshot, and presenters pick up the fresh revision (window `render/dirty` or per-view dirty bits).
4. Focus controller mirrors the active widget under both `widgets/<id>/focus/current` and `structure/window/<window>/focus/current` so input + accessibility bridges stay aligned.

> **Handler bindings:** the declarative helper registers each handler in an in-memory registry and stores `HandlerBinding { registry_key, kind }` at `events/<event>/handler`. Removal drops every key rooted at the widget path so stale lambdas never fire.
>
> **Render descriptors:** `render/synthesize` now holds a `RenderDescriptor` (widget kind enum). The renderer reads that descriptor plus the widget’s `state/*` and `meta/*` payloads to rebuild buckets when `render/dirty = true`; `render/bucket` remains as the cached snapshot for compatibility while the descriptor path rolls out. `include/pathspace/ui/declarative/Descriptor.hpp` exposes the shared `WidgetDescriptor` loader + bucket synthesizer so runtime tasks can normalize button/toggle/slider/list/tree/label payloads without duplicating builder logic.

- Surfaces (offscreen targets)
  - `surfaces/<surface-id>/`
    - `renderer` — app-relative, e.g. `renderers/2d`
    - `scene` — app-relative, e.g. `scenes/main`
    - `render` — execution that coordinates with the renderer target
    - `diagnostics/`
      - `errors/live`
      - `errors/history/<id>`
      - `log/ring/<segment>`
      - `metrics/live` — optional surface-level counters (e.g., presents, latency)

- Windows (presenters)
  - `windows/<window-id>/`
    - `window` — platform-native window shell
    - `views/<view-id>/`
      - `scene` — app-relative pointer to the bound scene (`scenes/...`)
      - `surface` — app-relative reference to a surface
      - `renderer` — app-relative renderer target path (e.g., `renderers/.../targets/surfaces/...`)
      - `htmlTarget` — app-relative reference to an HTML renderer target
      - `present` — execution to present the surface to the window
      - `present/policy` — optional string selector (`AlwaysFresh`, `PreferLatestCompleteWithBudget`, `AlwaysLatestComplete`)
      - `present/params/` — optional tuning overrides
        - `staleness_budget_ms` — float, default 8.0
        - `frame_timeout_ms` — float, default 20.0
        - `max_age_frames` — uint, default 1
        - `auto_render_on_present` — bool, default true
        - `vsync_align` — bool, default true
    - `diagnostics/`
      - `errors/live`
      - `errors/history/<id>`
      - `log/ring/<segment>`
      - `metrics/live` — optional presentation metrics (latency, dropped frames)

- Resources
- `resources/fonts/<family>/<style>/`
    - `meta/family` — canonical family identifier registered for this font
    - `meta/style` — style identifier (e.g., Regular, SemiBold)
    - `meta/weight` — CSS-like weight string (e.g., "400", "600")
    - `meta/fallbacks` — string array of fallback families, ordered by preference
    - `meta/active_revision` — `uint64_t` of the adopted atlas revision
    - `meta/atlas/softBytes` — soft residency budget in bytes for persisted glyph atlases
    - `meta/atlas/hardBytes` — hard residency budget in bytes for persisted glyph atlases
    - `meta/atlas/shapedRunApproxBytes` — approx bytes consumed per shaped run to scale cache capacity
    - `builds/<revision>/atlas.bin` — persisted atlas payload for the revision
    - `builds/<revision>/meta/*` — optional per-revision metadata (features, language coverage, etc.)
    - `inbox` — staging area for loader jobs and background ingestion
- `config/theme/`
    - `active` — string canonical name of the active widget theme
    - `theme/<name>/value` — stored `WidgetTheme` struct describing colors, typography, and widget styles
- `config/renderer/default` — app-relative renderer root (e.g., `renderers/widgets_declarative_renderer`), written by `SP::App::Create`.

- IO logging (app-local)
  - `io/log/error` — authoritative error log (UTF-8, newline-delimited)
  - `io/log/warn` — authoritative warnings log
  - `io/log/info` — authoritative info log
  - `io/log/debug` — authoritative debug log (may not tee globally)
  - `io/stdout` — read-only mirror of info and debug (system tee)
  - `io/stderr` — read-only mirror of warn and error (system tee)

- IO logging (app-local)
  - `io/log/error` — authoritative error log (UTF-8, newline-delimited)
  - `io/log/warn` — authoritative warnings log
  - `io/log/info` — authoritative info log
  - `io/log/debug` — authoritative debug log (may not tee globally)
  - `io/stdout` — read-only mirror of info and debug (system tee)
  - `io/stderr` — read-only mirror of warn and error (system tee)

## 3) Renderer target keys (final)

Target base (app-relative; fully-qualified form prefixes with the app root, e.g. `<app-root>/renderers/...` where `<app-root>` is `/system/applications/<app>` or `/users/<user>/system/applications/<app>`):
- `renderers/<rendererName>/targets/<kind>/<name>`
- `kind ∈ { surfaces, textures, windows, html }`

Keys under a target:
- `scene`
- `desc`
- `desc/active`
- `status/*`
- `settings`
- `render`
- `output/v1/...` (see section 2)
- `events/renderRequested/queue` — `AutoRenderRequestEvent` items enqueued by hit tests or dirty-marker scheduling
- `hints/dirtyRects` — optional array of dirty rectangles (float min/max) that renderers merge into their damage regions

## 4) Scene build and publish protocol

- Staging and publish:
  - `scenes/<scene-id>/builds/<revision>.staging/...` — write here first
  - Atomically rename to `scenes/<scene-id>/builds/<revision>`
  - Atomically replace `scenes/<scene-id>/current_revision` with `<revision>`

- Renderer adoption (per frame):
  - Read `scenes/<scene-id>/current_revision`
  - Traverse `scenes/<scene-id>/builds/<revision>/...`
  - Write outputs under the target’s `output/v1/...`

## 5) Output versioning

- Target outputs are versioned at the path level:
  - `output/v1/...` (current)
  - Future incompatible changes should add `output/v2/...` and keep `output/v1` during a deprecation window.

## 6) Fully-qualified examples

- `/system/applications/notepad/scenes/main/current_revision`
- `/system/applications/notepad/scenes/main/builds/42/...`
- `/system/applications/notepad/renderers/2d/targets/surfaces/editor/settings`
- `/system/applications/notepad/renderers/2d/targets/surfaces/editor/output/v1/common/frameIndex`
- `/system/applications/notepad/io/log/error`
- `/system/applications/notepad/io/log/warn`
- `/system/applications/notepad/io/log/info`
- `/system/applications/notepad/io/log/debug`
- `/system/applications/notepad/io/stdout`
- `/system/applications/notepad/io/stderr`
- `/system/io/stdout`
- `/system/io/stderr`
- `/system/devices/in/pointer/default/events`
- `/system/devices/out/gamepad/1/rumble`

## 7) Glossary

- App root: `/system/applications/<app>` or `/users/<user>/system/applications/<app>`
- App-relative path: a path string without leading slash, resolved within the app root
- Renderer target: a per-consumer subtree under `renderers/<id>/targets/<kind>/<name>`
- Snapshot (scene): immutable render-ready representation under `scenes/<scene-id>/builds/<revision>`
- Revision: monotonically increasing identifier referenced by `current_revision`
