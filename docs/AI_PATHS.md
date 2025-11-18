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
  - Push configuration (per-device):
    - `/system/devices/in/<class>/<id>/config/push/enabled` — bool opt-in for Trellis subscribers
    - `/system/devices/in/<class>/<id>/config/push/rate_limit_hz` — uint32 throttling hint
    - `/system/devices/in/<class>/<id>/config/push/max_queue` — uint32 queue cap hint
    - `/system/devices/in/<class>/<id>/config/push/telemetry_enabled` — bool gating device telemetry
    - `/system/devices/in/<class>/<id>/config/push/subscribers/<name>` — bool registration for downstream pumps
    - Defaults: `enabled=false`, `telemetry_enabled=false`, `rate_limit_hz=240`, `max_queue=256`. Providers only push once at least one subscriber path is set to `true`; otherwise pumps should use blocking `take` semantics. These nodes are backed by `DevicePushConfigNodes`, so every provider shares the same validation rules and error messages.
  - Discovery (recommended mount):
    - `/system/devices/discovery`
  - Haptics (outputs):
    - `/system/devices/out/gamepad/<id>/rumble`
- IO Trellis (normalized queues):
  - `/system/io/events/pointer`
  - `/system/io/events/button`
  - `/system/io/events/text`
  - `/system/io/events/pose`
  - Populated by `CreateIOTrellis` (`include/pathspace/io/IoTrellis.hpp`). Each queue stores the normalized structs from `include/pathspace/io/IoEvents.hpp`; downstream pumps must consume through `take` to avoid replaying the same event multiple times.
  - Metrics (on `/_system/telemetry/io/events_enabled = true`): `/system/io/events/metrics/{pointer_events_total,button_events_total,text_events_total}`.
- System IO (logs)
  - `/system/io/stdout`
  - `/system/io/stderr`
- Telemetry + push control (runtime-managed)
  - `/_system/telemetry/start/queue` / `/_system/telemetry/stop/queue` — accept `TelemetryToggleCommand` payloads to flip telemetry+
    device mirrors via `CreateTelemetryControl`.
  - `/_system/telemetry/io/state/running` — bool flag reflecting the telemetry controller lifecycle.
  - `/_system/telemetry/log/errors/queue` — string queue capturing command failures (bad device paths, invalid payloads, etc.).
  - `/_system/io/push/subscriptions/queue` — accepts `DevicePushCommand` payloads; the controller toggles `config/push/enabled`,
    `config/push/subscribers/<name>`, and optional telemetry overrides for the targeted devices.
  - `/_system/io/push/throttle/queue` — accepts `DeviceThrottleCommand` payloads; updates `/config/push/{rate_limit_hz,max_queue}` for each matched device.
- Telemetry toggles
  - `/_system/telemetry/io/events_enabled` — bool gate for IO Trellis metrics/publication, default `false`.
- Declarative runtime (global services)
  - `/system/widgets/runtime/input/state/running` — bool flag toggled by `SP::System::LaunchStandard`/`ShutdownDeclarativeRuntime`.
  - `/system/widgets/runtime/input/metrics/{widgets_processed_total,widgets_with_work_total,actions_published_total,last_pump_ns}`
  - `/system/widgets/runtime/input/metrics/{handlers_invoked_total,handler_failures_total,handler_missing_total,last_handler_ns}` — handler-dispatch telemetry produced by `CreateInputTask`.
  - `/system/widgets/runtime/input/metrics/{events_enqueued_total,events_dropped_total}` — mirrors how many widget events were mirrored into the canonical queues vs. dropped due to storage errors.
  - `/system/widgets/runtime/input/log/errors/queue` — string queue capturing reducer/pump failures.
  - Launch controls:
    - `LaunchOptions::start_input_runtime` (default `true`) decides whether `CreateInputTask` launches automatically when `LaunchStandard` runs.
    - `InputTaskOptions::poll_interval` (default 4 ms, minimum 1 ms) controls how often the runtime scans every `widgets/` subtree.
    - `InputTaskOptions::max_actions_per_widget` (default 64) bounds reducer work per widget each pass so watchdogs can predict latency.
    - `LaunchOptions::start_io_pump` (default `true`) controls whether `CreateIOPump` starts automatically; `LaunchOptions::io_pump_options` selects the windows root (`/system/widgets/runtime/windows`), per-window event root, metrics root, and worker timeouts.
  - `/system/widgets/runtime/io/state/running` — bool flag set by `CreateIOPump`.
  - `/system/widgets/runtime/input/metrics/{pointer_events_total,button_events_total,text_events_total,events_dropped_total,last_pump_ns}` — IO pump telemetry mirroring the InputTask counters.
  - `/system/widgets/runtime/windows/<token>` — runtime registration for each window; `window` stores the absolute window path and `subscriptions/{pointer,button,text}/devices` contains the device paths claimed by that window (strings such as `/system/devices/in/pointer/default`). Tokens default to `MakeRuntimeWindowToken(window_path)` (path string with `/` → `_`).
  - `/system/widgets/runtime/events/<token>/{pointer,button,text}/queue` — per-window event queues generated by the IO pump.
  - `/system/widgets/runtime/events/global/{pointer,button,text}/queue` — unmatched events (no window subscription) land here for diagnostics/tooling.
  - `/system/widgets/runtime/events/state/running` — bool flag toggled by `CreateWidgetEventTrellis` / `ShutdownWidgetEventTrellis`.
  - `/system/widgets/runtime/events/metrics/{pointer_events_total,button_events_total,widget_ops_total,hit_test_failures_total,last_dispatch_ns}` — routing telemetry for the WidgetEventTrellis worker.
  - `CreateWidgetEventTrellis` emits `WidgetOp`s for buttons, toggles, sliders, lists, trees, text inputs, stack panels (`StackSelect`), and paint surfaces (`PaintStrokeBegin/Update/Commit`).
  - **New (Nov 18, 2025):** Before each enqueue the trellis mutates `widgets/<id>/state/*` (hover, pressed, value, selection, expansion) and flips `widgets/<id>/render/dirty`, guaranteeing that declarative widgets visually react to input even when user handlers are no-ops.
  - `/system/widgets/runtime/events/log/errors/queue` — string queue capturing hit-test and routing failures.

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
- Widgets (details in `docs/Widget_Schema_Reference.md`)
  - `widgets/<widget-id>/` — canonical widget roots; the appendix tracks every `state/*`, `meta/*`, `layout/*`, `events/*`, and `render/*` leaf so this file can stay focused on high-level namespace placement.
  - `widgets/<widget-id>/events/inbox/queue` — runtime-populated stream of `WidgetAction` payloads emitted whenever the trellis reduces ops for that widget; consumers can also watch `events/<event>/queue` for filtered views (press, toggle, change, child_event, node_event, submit).
  - Stack previews expose `stack/panel/<panel-id>` targets; `StackSelect` ops flow through the widget inbox and bubble to `events/panel_select/queue` before invoking the optional handler.
  - Paint surfaces emit `PaintStrokeBegin/Update/Commit` ops with pointer-local coordinates; each op lands in the widget inbox plus `events/draw/queue` so reducers/handlers can append strokes under `state/history/*` and refresh `render/buffer`.
  - `widgets/focus/current` and `widgets/<id>/focus/{current,order}` — app-level and per-widget focus mirrors maintained by the focus controller.
  - `scenes/widgets/<widget-id>/` — widget snapshot subtrees (`states/*`, `current_revision`, `meta/*`) consumed by renderer targets.

### Declarative widget API summary

For schema tables, handler metadata, theme resolution rules, and per-widget specifics refer to `docs/Widget_Schema_Reference.md` (source of truth for `include/pathspace/ui/declarative/Schema.hpp`). This document only records the system-level plumbing so the namespace map stays concise:

- Runtime bootstrap — `SP::System::LaunchStandard`, `SP::App::Create`, `SP::Window::Create`, and `SP::Scene::Create` seed the application, window, and scene nodes listed above (visibility flags, view bindings, `structure/window/<window>/*`) before any widgets mount.
- Lifecycle worker — every declarative scene owns `runtime/lifecycle/trellis`, a `PathSpaceTrellis` that drains `widgets/.../render/events/dirty`, rebuilds buckets, and reports metrics under `runtime/lifecycle/metrics/*`. Control messages live under `runtime/lifecycle/control` so `SP::Scene::Shutdown` can stop the worker and the theme runtime can broadcast invalidations (`:invalidate_theme`). Metrics now include `widgets_registered_total`, `sources_active_total`, `events_processed_total`, `widgets_with_buckets`, `last_revision`, `last_published_widget`, `last_published_ms`, `pending_publish`, and `last_error` for snapshot failures.
- Snapshot feeds — updated widget buckets flow into `scene/structure/widgets/<widget>/render/bucket` and then into the normal `scenes/<scene>/builds/<revision>` publish/adopt flow described later in this document.

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
