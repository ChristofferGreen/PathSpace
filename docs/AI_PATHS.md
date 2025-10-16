# PathSpace — Standard Paths

> **Context update (October 15, 2025):** Path conventions documented here assume the newly launched “Atlas” AI context; translate prior context names to the closest Atlas equivalents.

This file gathers the canonical path namespaces and layout conventions referenced across the docs. Paths are grouped by domain and use placeholders in angle brackets. Absolute paths begin with “/”. App-internal paths are app-relative (no leading slash) and must resolve within the app root. Sources: see docs/AI_ARCHITECTURE.md and docs/AI_Plan_SceneGraph_Renderer.md. Keep these in sync when path conventions change.

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

## 2) Application subtree layout (app-relative)

The following subtrees are standardized within each application root (one of the absolute roots above). See docs/AI_Plan_SceneGraph_Renderer.md for detailed semantics and responsibilities.

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
      - `surface` — app-relative reference to a surface
      - `present` — execution to present the surface to the window
    - `diagnostics/`
      - `errors/live`
      - `errors/history/<id>`
      - `log/ring/<segment>`
      - `metrics/live` — optional presentation metrics (latency, dropped frames)

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
