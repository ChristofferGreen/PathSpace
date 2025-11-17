> **Update (October 28, 2025):** Captured the PathRenderer2D module split (`PathRenderer2DDamage/Encode/Color`, shared `PathRenderer2DDetail*`) plus the UI builder translation-unit refactor so this doc mirrors the current code layout and cross-links stay accurate.
> **Handoff note (October 19, 2025):** This document reflects the outgoing assistant cycle and is retained for historical reference. New AI maintainers should begin with `docs/AI_Onboarding_Next.md` for current workflow expectations and update this file only after aligning with the latest plan.

Renderer snapshot builder details have moved out of this architecture document. See docs/Plan_SceneGraph_Renderer.md (“Decision: Snapshot Builder”) for the authoritative policy, rebuild triggers, publish/GC protocol, and performance notes. This file focuses on PathSpace core (paths, trie storage, concurrency/wait/notify, views/alias layers, and OS I/O). New AI assistants should first walk through docs/AI_Onboarding.md for the session bootstrap checklist.

> **Context update (October 15, 2025):** PathSpace documentation now assumes the assistant context launched for this cycle; earlier references to legacy contexts should be interpreted accordingly.
> **Concurrency update (November 14, 2025):** PathSpaceTrellis redesign completed; see `docs/finished/Plan_PathSpaceTrellis_Finished.md` for the final fan-in notes. Historical background remains in `docs/finished/Plan_PathSpace_FanIn_Abandoned.md`.

## UI/Rendering — cross-reference

Present policy (backend-aware) and the software progressive present are documented in docs/Plan_SceneGraph_Renderer.md. This architecture document focuses on PathSpace core; rendering/presenter details live in the plan. Also see “View keys (final)” and “Target keys (final)” in docs/Plan_SceneGraph_Renderer.md for the authoritative schemas, and note that RenderSettings are a single-path atomic whole-object value; ParamUpdateMode::Queue refers to client-side coalescing before one atomic write (no server-side queue in v1).

> **Diagnostics note (October 16, 2025):** Presenter runs now persist the resolved present policy and age/staleness data (`presentMode`, `stalenessBudgetMs`, `frameTimeoutMs`, `maxAgeFrames`, `presentedAgeMs`, `presentedAgeFrames`, `stale`, `autoRenderOnPresent`, `vsyncAlign`) under `targets/<tid>/output/v1/common/*`. Renderer executions publish matching progressive statistics (`progressiveTilesUpdated`, `progressiveBytesCopied`) so tooling can correlate policy decisions with render work.
> **Diagnostics update (October 19, 2025):** `backendKind` and `usedMetalTexture` flag the presenter’s backend choice per frame so telemetry can distinguish Metal uploads from software fallbacks while the GPU encoder comes online.
> **Diagnostics update (October 19, 2025):** Dirty-rect hints are now coalesced at insert time and the full tile/damage/fingerprint metric set (`damage*`, `fingerprint*`, `progressiveTiles*`) is available when `PATHSPACE_UI_DAMAGE_METRICS=1`. Keep the flag unset for perf runs—the encode pass is still single-threaded today, and the next renderer milestone is to shard that work across core-count-sized tile queues.
> **Diagnostics update (October 29, 2025):** `Renderer::SubmitDirtyRects` stopped overwriting previously queued rectangles. The queue now accumulates per-widget hints so focus handoffs (e.g., widgets_example slider → list) clear the outgoing highlight on the very next frame; see `tests/ui/test_Builders.cpp` (“Widget focus slider-to-list transition covers highlight footprint”) for the regression coverage.
> **Diagnostics update (2025-10-17):** Software renderer incremental perf (64 px hints) holds ~140 FPS even at 4K, but full-surface damage and IOSurface reuse remain bottlenecks; see docs/Plan_SceneGraph.md for the profiling/benchmark plan and docs/AI_Onboarding.md for the latest hand-off notes.
> **Presenter update (October 18, 2025):** CAMetalLayer presents remain zero-copy by default. Builders expose a `capture_framebuffer` flag under `windows/<win>/views/<view>/present/params/capture_framebuffer`; only when the flag is true do presenters copy the IOSurface back to RAM for diagnostics/tests.
> **Diagnostics update (October 18, 2025):** Presenter/renderer errors now populate `diagnostics/errors/live` with a structured `PathSpaceError` (code, severity, message, revision, timestamp). `output/v1/common/lastError` remains as a compatibility mirror, but tooling should consume `diagnostics/errors/live` for full context. CAMetalLayer presents rely on a bounded IOSurface reuse pool, eliminating the range-group exhaustion seen in early paint sessions.
> **Telemetry update (October 19, 2025):** Targets now publish residency/cache metrics under `diagnostics/metrics/residency/{cpuBytes,cpuSoftBytes,cpuHardBytes,gpuBytes,gpuSoftBytes,gpuHardBytes}`. Builders thread the configured cache limits from `RenderSettings.cache` so dashboards can compare actual usage against the soft/hard watermarks.
> **Telemetry update (October 20, 2025):** Residency metrics now also publish `cpuSoftBudgetRatio`, `cpuHardBudgetRatio`, `gpuSoftBudgetRatio`, `gpuHardBudgetRatio`, per-budget exceed flags, and `overallStatus` under `diagnostics/metrics/residency/` so dashboards and alerts can consume thresholds without bespoke parsing.
> **Renderer staging (October 19, 2025):** Builders can now provision Metal targets alongside the software path. The rendering pipeline still populates targets through the software raster; enabling true Metal uploads is gated behind the environment variable `PATHSPACE_ENABLE_METAL_UPLOADS=1` while we finish the GPU encoder. When the flag is unset (or PATHSPACE_UI_METAL is disabled at build time) render contexts automatically fall back to `RendererKind::Software2D` so the CPU path remains the default in tests and CI.

### Widget Namespace Summary (October 24, 2025)

- Widget roots live at `<app>/widgets/<widget-id>`. Roots encapsulate mutable state (`state`), canonical metadata (`meta/kind`), style/material data (`meta/style`, `meta/label`, `meta/range`, `meta/items`, `meta/nodes`), and interaction queues (`ops/inbox/queue`, `ops/actions/inbox/queue`). Layout containers add `layout/{style,children,computed}` so layout recomputation stays observable.
- Widget scenes mount under `<app>/scenes/widgets/<widget-id>` with immutable snapshots per state (`states/<state-name>/snapshot`) and `current_revision` pointing at the active revision. Builders stamp `meta/name` and `meta/description` on first publish to keep diagnostics readable.
- Focus routing keeps the active widget name at `<app>/widgets/focus/current`. Focus helpers update the value atomically and optionally enqueue auto-render events when focus shifts.
- Interaction bindings enqueue dirty hints via `Renderer::SubmitDirtyRects` and render requests under `renderers/<rid>/targets/<kind>/<name>/events/renderRequested/queue` when `auto_render` is enabled. Both queues rely on standard PathSpace atomic writes and wait/notify semantics.
- Reducer helpers (`Widgets::Reducers::*`) drain `widgets/<id>/ops/inbox/queue`, mutate widget state (usually by rewriting `state` or metadata), and optionally mirror actions to `ops/actions/inbox/queue` for downstream consumers.
- Builder workflow: bootstrap renderer/surface/window with `Builders::App::Bootstrap`, create widgets via `Builders::Widgets::Create*`, bind interactions with `Builders::Widgets::Bindings::Create*Binding`, pump reducers (`Widgets::Reducers::ReducePending` and `PublishActions`), and use `Builders::App::UpdateSurfaceSize` / `PresentToLocalWindow` for resize + diagnostics loops. Each helper performs whole-object replaces so readers never see torn state.

Keep `docs/AI_PATHS.md` synchronized with new widget kinds or metadata keys and ensure tests mirror any path additions before merging.

## UI/Rendering — cross-reference

Present policy (backend-aware) and the software progressive present are documented in docs/AI_Plan_SceneGraph_Renderer.md. This architecture document focuses on PathSpace core; rendering/presenter details live in the plan. Also see “View keys (final)” and “Target keys (final)” in docs/AI_Plan_SceneGraph_Renderer.md for the authoritative schemas, and note that RenderSettings are a single-path atomic whole-object value; ParamUpdateMode::Queue refers to client-side coalescing before one atomic write (no server-side queue in v1).

See also:
- `docs/Plan_SceneGraph_Renderer.md` for the broader rendering plan and target I/O layout. If snapshot semantics change, update both documents in the same PR per `.rules`.
 - `docs/AI_Paths.md` for the canonical path namespaces and layout conventions; update it alongside changes to path usage and target I/O layout.
- HTML render targets now persist adapter output under `renderers/<rid>/targets/html/<name>/output/v1/html/{dom,css,commands,assets,mode,usedCanvasFallback}`; see `Html::Adapter` for the serialization rules and SceneGraph plan Phase 7 for outstanding replay/CI work.


## Contributing to PathSpace

For AI-facing workflows (branching, PR creation, troubleshooting, commit guidelines), see AGENTS.md. This document focuses on code architecture and APIs.

PathSpace is a coordination language that enables insertion and extractions from paths in a thread safe datastructure. The data structure supports views of the paths similar to Plan 9. The data attached to the paths are more like a JSON datastructure than files though. The data supported is standard C++ data types and data structures from the standard library, user created structs/classes as well as function pointers, std::function or function objects for storing executions that generate values to be inserted at a path.

### Declarative Runtime Services (November 14, 2025)
- `SP::System::LaunchStandard` (`include/pathspace/ui/declarative/Runtime.hpp`) now performs end-to-end bootstrap for declarative UI runs:
  - Seeds `/system/themes/<name>` and app-local `config/theme/active`, then mirrors the normalized theme name into each window’s `style/theme`.
  - Ensures every app owns `renderers/widgets_declarative_renderer` plus per-view surfaces named `surfaces/widgets_surface_<window>_<view>`, wiring `windows/<id>/views/<view>/{surface,renderer,scene}` and mirroring those values under `scenes/<scene>/structure/window/<id>/{surface,renderer,present}`.
  - Starts the `/system/widgets/runtime/input` pump unless `LaunchOptions::start_input_runtime` is disabled. The worker drains `widgets/<widget>/ops/inbox/queue`, publishes `WidgetAction`s back to `ops/actions/inbox/queue`, and maintains metrics under `/system/widgets/runtime/input/metrics/*` (`widgets_processed_total`, `widgets_with_work_total`, `actions_published_total`, `last_pump_ns`). Failures enqueue strings to `/system/widgets/runtime/input/log/errors/queue`.
  - Starts the `/system/widgets/runtime/io` pump unless `LaunchOptions::start_io_pump` is disabled. The worker consumes `/system/io/events/{pointer,button,text}` and fans them into `/system/widgets/runtime/events/<window-token>/{pointer,button,text}/queue` based on the device subscriptions registered under `/system/widgets/runtime/windows/<window-token>/subscriptions/{pointer,button,text}/devices`, with `/system/widgets/runtime/events/global/*/queue` as the unmatched fallback.
- `include/pathspace/ui/declarative/Widgets.hpp` exposes the declarative API surface. `WidgetFragment` + `Widgets::Mount` let callers author widgets entirely through fragments; `Button::Create`, `List::Create`, `Slider::Create`, etc., normalize styles/state, register handler bindings (`events/<event>/handler = HandlerBinding { registry_key, kind }`), and flip `render/dirty` automatically. `render/synthesize` now stores a `RenderDescriptor` (widget kind enum) instead of raw callables so renderers synthesize buckets directly from stored state/style.
- Call `SP::System::ShutdownDeclarativeRuntime(space)` in short-lived tests/examples to stop the pump; production apps can leave it running for the PathSpace lifetime.
- The runtime remains idempotent: repeated `LaunchStandard` runs reuse the existing scaffolding and report `LaunchResult::input_runtime_started = false` / `LaunchResult::io_pump_started = false` when those workers already exist.

##### IO runtime launch & environment knobs (November 17, 2025)

- `LaunchOptions::start_input_runtime` defaults to `true`. Disable it when embedding PathSpace into processes that already own an input loop (e.g., scripted tests or custom render hosts) so `LaunchStandard` skips `CreateInputTask`.
- `LaunchOptions::input_task_options` forwards directly to `InputTaskOptions`:
  - `poll_interval` (default 4 ms) controls how often the worker scans `/system/applications/<app>/widgets/*`. A value ≤ 0 falls back to 1 ms to guarantee progress.
  - `max_actions_per_widget` (default 64) caps how many pending reducer actions drain per widget during a single pass, providing predictable latency for large trees.
- `CreateInputTask` seeds `/system/widgets/runtime/input`:
  - `/system/widgets/runtime/input/state/running` reflects worker lifecycle.
  - `/system/widgets/runtime/input/metrics/{widgets_processed_total,widgets_with_work_total,actions_published_total,last_pump_ns}` accumulate per-pass stats so tooling can spot stalls.
  - `/system/widgets/runtime/input/log/errors/queue` collects stringified reducer failures. Global logging env toggles (`PATHSPACE_LOG`, `PATHSPACE_LOG_ENABLE_TAGS`, `PATHSPACE_LOG_SKIP_TAGS`) still govern whether host-side logs emit additional diagnostics, but the authoritative record for pump failures is this queue.
- `LaunchOptions::start_io_pump` defaults to `true`. Disable it when embedding PathSpace into a host that performs its own event routing. `LaunchOptions::io_pump_options` expose the pump-level timeouts (block, idle sleep, subscription refresh cadence), metrics root, events root, windows root, and an optional shared `stop_flag` for deterministic shutdown.
- `CreateIOPump` seeds `/system/widgets/runtime/io/state/running`, keeps `/system/widgets/runtime/input/metrics/{pointer_events_total,button_events_total,text_events_total,events_dropped_total,last_pump_ns}` up to date, and republishes normalized IO Trellis events into per-window queues:
  - Windows register under `/system/widgets/runtime/windows/<token>` with `window = <absolute window path>` and `subscriptions/{pointer,button,text}/devices = ["/system/devices/in/<class>/<id>", ...]`. Tokens default to `MakeRuntimeWindowToken(window_path)` (path string with non-alphanumerics replaced by `_`).
  - Routed events land at `/system/widgets/runtime/events/<token>/{pointer,button,text}/queue`. When no subscription claims a device path the event falls through to `/system/widgets/runtime/events/global/{pointer,button,text}/queue` so diagnostics and legacy tooling can continue consuming the stream.
  - Errors enqueued while routing are mirrored into `/system/widgets/runtime/input/log/errors/queue` alongside reducer failures for a single debugging surface.
- `LaunchOptions::start_io_telemetry_control` defaults to `true`. `CreateTelemetryControl` watches `/_system/telemetry/start/queue`, `/_system/telemetry/stop/queue`, `/_system/io/push/subscriptions/queue`, and `/_system/io/push/throttle/queue`, mirrors the requested toggles across `/system/devices/in/<class>/<id>/config/push/*`, publishes its lifecycle at `/_system/telemetry/io/state/running`, and logs failures to `/_system/telemetry/log/errors/queue`.
  - Telemetry toggles: push `TelemetryToggleCommand{.enable = true}` to the start queue (or `.enable = false` to the stop queue) to flip `/_system/telemetry/io/events_enabled` and mirror the setting to every device’s `config/push/telemetry_enabled` leaf.
  - Device push control: queue `DevicePushCommand` entries under `/_system/io/push/subscriptions/queue`. `device` accepts an absolute path, `/system/devices/in/<class>/*` prefix, or `"*"`; the worker updates `config/push/enabled` (when `touch_push_enabled` is true) and `config/push/subscribers/<name>` on every match, with optional telemetry overrides per device.
  - Throttling: send `DeviceThrottleCommand` entries through `/_system/io/push/throttle/queue` to update `config/push/rate_limit_hz` / `config/push/max_queue` for the specified devices. Leave `device="*"` to broadcast to every device class registered under `/system/devices/in`.
- Provider-facing environment/configuration is exposed through `DevicePushConfigNodes` (see `src/pathspace/layer/io/DevicePushConfigNodes.*`):
  - `/system/devices/in/<class>/<id>/config/push/enabled` (bool, default `false`) is flipped by pumps/consumers that want events pushed instead of polling.
  - `.../config/push/rate_limit_hz` (uint32, default 240) and `.../config/push/max_queue` (uint32, default 256) bound provider throughput.
  - `.../config/push/telemetry_enabled` gates per-device counters; keep it `false` in latency-sensitive scenarios unless telemetry has been requested via `/_system/telemetry/start`.
  - `.../config/push/subscribers/<name>` (bool) lets each pump register interest so devices only publish when at least one subscriber is active. The IO Trellis + IO Pump will subscribe under stable names once their phases land.
- `CreateIOTrellis(PathSpace&, IoTrellisOptions const&)` (see `include/pathspace/io/IoTrellis.hpp`) mounts the IO Trellis worker. It discovers `/system/devices/in/{pointer,text,keyboard,gamepad}/*`, ensures `config/push/enabled=true`, registers itself under `config/push/subscribers/<subscriber_name>`, drains the provider queues, and emits normalized structs (`PointerEvent`, `ButtonEvent`, `TextEvent`) into `/system/io/events/{pointer,button,text}`. Options cover:
  - `subscriber_name` (default `io_trellis`) so multiple consumers can coexist without clobbering one another.
  - Event wait/idle sleep/discovery intervals (milliseconds) to balance latency vs. CPU usage.
  - Telemetry roots: `telemetry_toggle_path` defaults to `/_system/telemetry/io/events_enabled` (false by default) and `metrics_root` defaults to `/system/io/events/metrics`.
  - Device-class enable flags so callers can temporarily disable keyboard/gamepad listeners in test harnesses.
- When `/ _system/telemetry/io/events_enabled` flips true, `CreateIOTrellis` publishes rolling totals under `/system/io/events/metrics/{pointer_events_total,button_events_total,text_events_total}`. Leave the flag unset for production runs where the extra writes would contend with input latency.
- Tests that need declarative scaffolding without the worker threads should set `launch_options.start_input_runtime = false` and `launch_options.start_io_pump = false`, then manually call `CreateInputTask`, `CreateIOTrellis`, or `CreateIOPump` as needed.

### Undo History Layer Guidance
- Each history-enabled subtree owns exactly one undo/redo stack. We do not coordinate history across multiple roots; a logical command must mutate a single root so one stack captures the full change.
- If a feature naturally spans several areas, route it through a command-log subtree or reorganize the data so related metadata lives alongside the primary payload before enabling history. `enableHistory` now rejects configurations that expect shared stacks—setting the same `HistoryOptions::sharedStackKey` on multiple roots triggers an error so callers regroup under a single undo root instead.
- Undo entries flush to disk by default for crash safety. Advanced callers can set `HistoryOptions::manual_garbage_collect` or insert `_history/set_manual_garbage_collect = true` when they want to defer durability and retention to explicit checkpoints.
- November 10, 2025: `history::UndoableSpace` now journals mutations instead of taking copy-on-write snapshots. The mutation log records inserts/takes plus inverse payloads so undo/redo replay quickly without walking the trie. Retention budgets still apply (auto-trim plus manual garbage collect), `_history/stats/*` and `_history/lastOperation/*` are derived from `UndoJournalState::Stats`, and `_history/unsupported/*` continues to log nodes we cannot serialize (executions, nested spaces) because those payloads are skipped when we capture before/after state.
- November 7, 2025: Persistence metadata now serializes through a versioned little-endian binary header shared across recovery, telemetry, and tooling. Alpaca JSON output was retired; `_history/stats/*` remains the authoritative introspection surface for inspectors/CLIs.
- Migration: Legacy snapshot persistence requires a bridge export. Check out the November 9, 2025 bridge commit (`git log --before '2025-11-10' -1` to locate the hash), export with `pathspace_history_savefile` into a `history.journal.v1` bundle, then import on the current build. Step-by-step commands and validation checks live in `docs/AI_Debugging_Playbook.md` (“Undo journal migration runbook”).

##### Mutation guardrails (November 10, 2025)
- `UndoableSpace` overrides `PathSpaceBase::in`/`out` and wraps every mutation in `beginJournalTransactionInternal`, capturing before/after payload snapshots and replay metadata via `recordJournalMutation`. All live history writes therefore flow through the journal regardless of the public API the caller invokes (`insert`, `take`, history control commands).
- When adding a new mutator (e.g., a bulk update helper or custom transaction primitive), route the implementation through `UndoableSpace` by reusing the existing transaction helpers and emitting a journal mutation for each logical change. Bypassing these helpers will desynchronise undo/redo state and persistence—treat it as a correctness bug.
- External contributors introducing new mutation paths must update this guardrail section and add regression coverage mirroring the patterns in `tests/unit/history/test_UndoableSpace.cpp` (`journal telemetry matches snapshot telemetry outputs`, fuzz/concurrency tests). The maintainer team will run the same `rg 'snapshot' tests/unit/history` audit to ensure snapshot-era helpers do not reappear.

#### Journal Persistence Format (November 10, 2025)
- Journal files live at `<persistence-root>/journal.log` and start with a 12-byte header: `0x50534A46` (`'PSJF'` little endian) magic, `uint16` format version (currently `1`), and a reserved `uint32` set to zero. The header is fsynced when the writer is asked to persist eagerly.
- Root directories mirror the history namespace: the persistence namespace becomes the top-level folder, and each undo root is encoded as `"__root"` for `/` or the concrete path with `/` replaced by `_` (e.g. `/doc/article` -> `_doc_article`).
- Each appended record is length-prefixed with a `uint32` (little endian) so tools can stream entries without parsing the payload. Compaction rewrites the header plus the surviving records in the same framing.
- Record payloads follow the `'PSJL'` (`0x50534A4C`) entry schema:
  - `uint16` entry version (`1`), `uint8` operation kind (`0` insert, `1` take), `uint8` flag byte (`0x01` means barrier), and a reserved `uint16`.
  - Three `uint64` fields: wall-clock timestamp in milliseconds since epoch, monotonic timestamp in nanoseconds, and the monotonic sequence number assigned by `UndoJournalState`.
  - Path bytes are stored as `<uint32 length><utf-8 path>` with no null terminator.
  - Two serialized payload blocks encode the post-mutation value (`value`) and the inverse payload (`inverseValue`). Each block is `<uint8 present flag><uint32 byte length><raw bytes>`. When the flag is zero the block carries zero bytes; otherwise it contains the `NodeData::serializeSnapshot` output so tooling can reuse the existing decoding helpers.
- The writer and replayer operate in host byte order today (little endian on all supported targets). Tooling that needs to run on big-endian machines should transcode explicitly before emitting or consuming records.
- `compactJournal` preserves record order and re-emits the same schema; new versions must bump both the file header and entry version so older tooling can detect incompatibilities before reading arbitrary bytes.

## Path System

### Path Types

#### Concrete Paths
Concrete paths represent exact locations in the PathSpace hierarchy. They follow these rules:
- Must start with a forward slash (/)
- Components are separated by forward slashes
- Cannot contain relative path components (. or ..)
- Names cannot start with dots
- Components cannot be empty (// is normalized to /)

Example:
```cpp
"/data/sensors/temperature"
"/system/status/current"
```

#### Glob Paths
Glob paths extend concrete paths with pattern matching capabilities. They support:
- `*` - Matches any sequence of characters within a component
- `**` - Matches zero or more components (super-matcher)
- `?` - Matches any single character
- `[abc]` - Matches any character in the set
- `[a-z]` - Matches any character in the range
- `\` - Escapes special characters

Example patterns:
```cpp
"/data/*/temperature"    // Matches any sensor's temperature
"/data/**/status"       // Matches status at any depth
"/sensor-[0-9]/*"       // Matches numbered sensors
"/data/temp-?"          // Matches single character variations
```

### Path Implementation

#### Core Classes
```cpp
// Base path template
template <typename T>
struct Path {
    T path;  // string or string_view storage
    bool isValid() const;
    T const& getPath() const;
};

// Concrete path types
using ConcretePathString = ConcretePath<std::string>;
using ConcretePathStringView = ConcretePath<std::string_view>;

// Glob path types
using GlobPathString = GlobPath<std::string>;
using GlobPathStringView = GlobPath<std::string_view>;
```

#### Path Component Iteration
The system provides iterators to traverse path components:
```cpp
template <typename T>
struct ConcreteIterator {
    ConcreteNameStringView operator*() const;  // Current component
    bool isAtStart() const;                    // Check if at first component
    std::string_view fullPath() const;         // Full path string
};

template <typename T>
struct GlobIterator {
    GlobName operator*() const;  // Current component with pattern matching
    bool isAtStart() const;      // Check if at first component
    std::string_view fullPath() const;  // Full path string
};
```

#### Pattern Matching
Pattern matching can handle various glob patterns:

```cpp
struct GlobName {
    // Returns match status
    auto match(const std::string_view& str) const
        -> bool /*match*/;

    bool isConcrete() const;  // Check if contains patterns
    bool isGlob() const;      // Check for glob characters
};
```

Pattern matching behaviors:
- `*` matches within component boundaries
- Character classes support ranges and sets
- Escaped characters are handled literally
- Empty components are normalized

### Path Usage Examples

#### Basic Path Operations
```cpp
PathSpace space;

// Insert at concrete path
space.insert("/sensors/temp1/value", 23.5);

// Insert using pattern
space.insert("/sensors/*/value", 0.0);  // Reset all sensor values

// Read with exact path
auto value = space.read<float>("/sensors/temp1/value");

// Read with pattern
auto values = space.read<float>("/sensors/*/value");
```

#### Pattern Matching Examples
```cpp
// Match any sensor
"/sensors/*"

// Match sensors with numeric IDs
"/sensors/[0-9]*"

// Match any depth under sensors
"/sensors/**/value"

// Match specific character variations
"/sensor-?"

// Match range of sensors
"/sensors/[1-5]"
```

### Path Performance Considerations

#### Path Storage
- Uses `string_view` for read-only operations to avoid copies
- Maintains string ownership where needed for modifications
- Caches path components for efficient iteration

#### Pattern Matching Optimization
- Early termination for non-matching patterns
- Efficient character class evaluation
- Optimized super-matcher (`**`) handling
- Component-wise matching to minimize string operations

#### Memory Efficiency
- Shared path component storage
- Lazy evaluation of pattern matches
- Efficient string views for immutable paths

### Path Thread Safety
The path system provides thread-safe operations through:
- Immutable path objects
- Synchronized pattern matching
- Thread-safe path resolution
- Atomic path operations

Testing guidance:
- Concurrency and scheduling changes (WaitMap, Node trie, Task/Executor) can expose intermittent races only under repeated runs.
- Use the build helper to loop tests: `./scripts/compile.sh --loop[=N]` (default N=15) to increase the likelihood of catching rare timing issues.

Wait/notify:
- Waiters are stored in a trie-backed registry keyed by concrete paths, with a separate registry for glob-pattern waiters.
- Concrete notifications traverse the trie along the path (O(depth)), while glob notifications filter and match registered patterns.
- All operations are synchronized with internal mutexes; waiter CVs are attached to nodes to minimize contention.

Additional note:
- Task completion notifications are lifetime-safe via a NotificationSink token. Each `PathSpaceBase` owns a `shared_ptr<NotificationSink>` that forwards to its `notify(path)`; `Task` objects capture a `weak_ptr<NotificationSink>`. During shutdown, the `PathSpace` resets the `shared_ptr` so late notifications are dropped cleanly without dereferencing stale pointers.

### Path Error Handling
Comprehensive error handling for path operations:
- Path validation errors
- Pattern syntax errors
- Resolution failures
- Type mismatches
- Timeout conditions

## Internal Data
PathSpace stores data in a unified Node trie. Each Node contains:
- children: a concurrent hash map keyed by the next path component
- data: an optional NodeData payload that holds serialized bytes and/or queued Task objects
- nested: an optional nested PathSpaceBase anchored at that node

Inserting more data at a path appends to the node’s NodeData sequence. Reading returns a copy of the front element; extracting pops the front element. Nodes that become empty after extraction are erased from their parent when safe.

Concurrency notes:
- children uses a sharded concurrent map for scalability
- payload accesses (data/nested) are guarded by a per-node mutex to ensure safe updates under contention

Additionally, nodes may hold `Task` objects representing deferred computations alongside serialized bytes. `NodeData` stores tasks (`std::shared_ptr<Task>`) and, when tasks complete, notifications are delivered via a `NotificationSink` interface: tasks hold a `std::weak_ptr<NotificationSink>` and the owning `PathSpaceBase` provides the `shared_ptr` token and forwards to `notify(path)`. This removes the need for a global registry and prevents use-after-free races during teardown (the space resets its token during shutdown, causing late notifications to be dropped safely).

## Syntax Example
```CPP
PathSpace space;
space.insert("/collection/numbers", 5);
space.insert("/collection/numbers", 3.5f);
assert(space.take<int>("/collection/numbers").value() == 5);
assert(space.read<float>("/collection/numbers").value() == 3.5);

space.insert("/collection/executions", [](){return 7;});
assert(space.take<int>("/collection/executions", Block{}).value() == 7);
```

## Polymorphism
The internal spaces inside a path are implemented witha  Leaf class, it's possible to inherit from that class and insert that child class in order to change the
behaviour of parts of the path structure of a PathSpace instance. This can be used to have different behaviour for different sub spaces within a PathSpace. By default PathSpace
will create a Leaf of the same type as the parent when creating new ones (which happens during insert of data).

## Path Globbing
Paths given to insert can be a glob expression, if the expression matches the names of subspaces then the data will be inserted to all the matching subspaces.
```cpp
PathSpace space;
space.insert("/collection/numbers", 5);
space.insert("/collection/numbers_more", 4);
space.insert("/collection/other_things", 3);

// Globbed insert updates only paths matching the pattern "numbers*"
space.insert("/collection/numbers*", 2);

// Unaffected: different prefix
assert(space.read<int>("/collection/other_things").value() == 3);

// Affected: matches "numbers*"
assert(space.take<int>("/collection/numbers_more").value() == 4);
assert(space.take<int>("/collection/numbers_more").value() == 2);
```

## Blocking
Blocking is opt-in and expressed through the helpers in `src/pathspace/core/Out.hpp`.

- Passing `Block{}` to `read`/`take` (or composing `Out{} & Block{}`) turns the call into a waiter when the target node is empty. The waiter is registered in the concrete or glob registry described in the wait/notify section.
- `Block{}` defaults to a very large timeout (100 years); override it with `Block{std::chrono::milliseconds{budget}}` to enforce a real deadline. When the timeout elapses, the operation returns an `Error::Timeout` in the `std::expected` result.
- Successful reads/takes automatically deregister their waiter before returning. If the waiter is cancelled because the path is deleted or the space is shutting down, the operation reports an error so callers can retry or exit.
- Inserts themselves do not block, but they wake both concrete and glob waiters on completion. Custom layers can tap into the same behaviour through `BlockOptions` when forwarding or composing higher-level queues.

## Operations
The operations in the base language are insert/read/extract, they are implemented as member functions of the PathSpace class.
* **Insert**:
	* Insert data or a Leaf to one or more paths. If the path does not exist it will be created.
	* The given path can be a concrete path in which case at most one object will be inserted or a glob expression path which could potentially insert multiple values.
	* Supports batch operations by inserting an initialiser list
	* Takes an optional In which has the following properties:
		* Optional Execution object that describes how to execute the data (if the data is a lambda or function):
			* Execute immediately or when the user requests the data via read/extract.
			* If the data should be cached and updated every n milliseconds.
				* How many times the function should be executed.
			* If the value to be stored is executed in a lazy fashion or right away.
		* Optional Block object specifying what to do if the value does not (yet) exist, enables waiting forever or a set amount of time.
	* The data inserted can be:
		* Executions with signature T() or T(ConcretePath const &path, PathSpace &space) :
			* Lambda
			* Function pointer
			* std::function
			* Preregistered executions for serialisation/deserialisation over the network.
		* Data
			* Fundamental types
			* Standard library containers if serialisable
			* User created structures/classes as long as they are serialisable
	* Returns an InsertReturn structure with the following information:
		* How many items/Tasks were inserted.
		* What errors occurred during insertion.
	* Syntax:
		* InsertReturn PathSpace::put<T>(GlobPath const &path, T const &value, optional<In> const &options={})
* **Read**:
	* Returns a copy of the front value at the supplied path or Error if it could not be found, if for example the path did not exist or the front value had the wrong type.
	* Takes an optional ReadOptions which has the following properties:
		* Optional Block object specifying what to do if the data does not exist or paths to the data do not exist:
			* Wait forever if data/space does not exist
			* Wait a specified amount of milliseconds if data/space does not exist
			* Return an error
	* Takes a ConcretePath, does not support GlobPaths. Perhaps will implement a readMultiple later that returns a vector<T>
	* Syntax:
		* std::expected<T, Error> PathSpace::read<T>(ConcretePath const &path, optional<ReadOptions> const &options={})
* **Extract**:
	* Same as read but pops the front data instead of just returning a copy.
	* Syntax:
		* std::expected<T, Error> PathSpace::take<T>(ConcretePath, Block, optional<Out> const &options={})

## Data Storage
A normal PathSpace will store data by serialising it to a std::vector<std::byte>. That vector can contain data of different types and a separate vector storing std::type_id pointers
together with how many objects or that type are in a row will be used to determine what parts of the data vector has what type. std::function objects will be stored in their own vector
as well since they can not be serialised. Insert will append serialised data to this vector. Extract will not necessarily erase from the front of the vector since this would be too costly,
a pointer to the front element will instead be stored and its position changed forward when a extract is issued. At first the serialisation will be done via the alpaca library but when a
compiler supporting the C++26 serialisation functionality it will be rewritten to use that instead.

## Unit Testing
Unit testing is by using the C++ doctest library.

## Exception Handling
PathSpaces will not throw exceptions, all errors will be handled via the return type of the operations.

## Views
PathSpace supports read-only projections and permission gating through `src/pathspace/layer/PathView.hpp` (view with a permission callback) and path aliasing/forwarding via `src/pathspace/layer/PathAlias.hpp`.

- PathView: wraps an underlying `PathSpaceBase` and enforces a `Permission(Iterator)` policy for `in`/`out`. It can also optionally prepend a root mount prefix when forwarding paths.
- PathAlias: a lightweight alias layer that forwards `in`/`out`/`notify` to an upstream space after path rewriting with a configurable `targetPrefix`. It uses the iterator tail (`currentToEnd()`) so nested mounts resolve correctly. It forwards `notify(...)` by mapping the notification path through the current target prefix before forwarding upstream. On retargeting, it emits a notification on its mount prefix to wake waiters and prompt re-resolution.

Concurrency and notifications:
- Both layers are mount-agnostic; they adopt the parent `PathSpaceContext` when inserted so that `notify`/wait semantics flow through naturally.
- `PathAlias` forwards `notify(...)` by mapping the notification path through its current `targetPrefix` and then forwarding upstream, preserving end-to-end wait/notify semantics across the alias boundary.
- On retargeting, `PathAlias` emits a notification on its mount prefix to wake waiters and prompt re-resolution.
- Provider/forwarder patterns:
  - When relaying events between providers (e.g., mouse -> mixer), consume with `take` (pop) on the upstream queue so each event is forwarded exactly once; use `read` (peek) only for passive observation.
  - Providers with blocking reads should notify waiters upon enqueue (`notify(path)` or `notifyAll()`).
  - Use bounded waits and cooperative shutdown: long-running loops should check a stop flag and exit promptly; `PathSpace::shutdown()` marks shutting down and wakes waiters.

### Shutdown and Test Hooks

- `PathSpace::shutdown()` is the public API to cooperatively wake waiters and clear paths during teardown.
- `PathSpace::shutdownPublic()` and `PathSpace::notifyAll()` are protected test utilities. When tests need to call them, expose via a small test-only subclass, for example:
  - `struct TestablePathSpace : SP::PathSpace { using SP::PathSpace::PathSpace; using SP::PathSpace::shutdownPublic; };`
- `PathSpace::peekFuture(...)` and `PathSpace::setOwnedPool(...)` are protected implementation details. Prefer the unified `read<FutureAny>(path)` and constructor injection of the executor/pool.
- Nested spaces adopt shared context and a mount prefix internally via the protected `adoptContextAndPrefix(...)`; external callers should not invoke this directly.

## Operating System
Device IO is provided by path-agnostic layers that can be mounted anywhere in a parent `PathSpace`, with platform backends feeding events into them:

- Keep `PathIO` base and current providers (mouse, keyboard, pointer mixer, stdout, discovery, gamepad).
- Event providers deliver typed events via `out()`/`take()`; blocking semantics are controlled by `Out{doBlock, timeout}` and pop-vs-peek by `Out.doPop`.
- Canonical device namespace (aligned with SceneGraph plan; see `docs/AI_Paths.md`):
  - Inputs:
    - `/system/devices/in/pointer/default/events`
    - `/system/devices/in/text/default/events`
    - `/system/devices/in/gamepad/default/events`
  - Push config nodes (`/system/devices/in/<class>/<id>/config/push/*`), surfaced directly by each provider:
    - `enabled` (bool) gates provider-side push to Trellis subscribers
    - `rate_limit_hz` / `max_queue` (uint32) express throttling hints and queue caps
    - `telemetry_enabled` (bool) toggles per-device metrics emission
    - `subscribers/<name>` (bool) lets pumps register/unregister downstream consumers
  - Normalized IO Trellis queues live under `/system/io/events/{pointer,button,text,pose}` with canonical structs defined in `include/pathspace/io/IoEvents.hpp`; future pump stages drain those queues instead of per-device types.
  - Discovery mount (recommended): `/system/devices/discovery`
  - Haptics (outputs): `/system/devices/out/gamepad/<id>/rumble`
- Notifications: providers perform targeted `notify(mountPrefix)` and `notify(mountPrefix + "/events")` on enqueue; use `notifyAll()` only for broad updates (e.g., retargeting or clear).

### Backpressure and queue limits
- Scope: Event providers (mouse/keyboard/pointer mixer/gamepad) maintain per-mount in-memory deques for pending events.
- Complexity: enqueue/dequeue O(1); targeted notify is O(depth) along the path trie; memory is O(N) per queue.
- Current behavior: queues are unbounded deques; no drops are performed.
- Planned: bound queues to N events (target default ≈1024) with a configurable drop policy:
  - Oldest-drop: drop front entries to minimize end-to-end latency on live streams.
  - Newest-drop: drop incoming events if preserving history is preferred.
- Blocking semantics:
  - Non-blocking read returns `NoObjectFound` when empty.
  - Blocking read wakes on arrival or timeout; wakeups use targeted `notify(mountPrefix)` and `notify(mountPrefix + "/events")`.
- Mitigations:
  - Prefer pop (`take`) to keep up; minimize work in the read loop; batch processing where possible.
  - Use mixers/aggregation to reduce per-device rates; downsample or coalesce deltas when acceptable.
  - Consider shorter time slices for provider loops once configurable wait-slice is introduced.
- Observability: track counters per provider (enqueued, dropped_oldest, dropped_newest); expose via a side path such as `.../stats` in a later change.

- `src/pathspace/layer/io/PathIOMouse.hpp`, `src/pathspace/layer/io/PathIOKeyboard.hpp`, and `src/pathspace/layer/io/PathIOGamepad.hpp` expose typed event queues (MouseEvent/KeyboardEvent/GamepadEvent) with blocking `out()`/`take()` (peek vs pop via `Out.doPop`). When mounted with a shared context, `simulateEvent()` wakes blocking readers.
- `src/pathspace/layer/io/PathIODeviceDiscovery.hpp` provides a simulation-backed discovery surface (classes, device IDs, per-device `meta` and `capabilities`), using iterator tail mapping for correct nested mounts; recommended mount prefix: `/system/devices/discovery`.

Platform backends (unified, via compile-time macros):
- `src/pathspace/layer/io/PathIOMouse.hpp` and `src/pathspace/layer/io/PathIOKeyboard.hpp` expose start()/stop() hooks and select OS paths internally (e.g., `PATHIO_BACKEND_MACOS`) to feed events via `simulateEvent(...)`.
- On macOS, enable with `-DENABLE_PATHIO_MACOS=ON` to define `PATHIO_BACKEND_MACOS` (CI uses simulation/no-op by default).
- Deprecated: `src/pathspace/layer/macos/PathIO_macos.hpp` is a compatibility shim only and no longer defines `PathIOMouseMacOS` or `PathIOKeyboardMacOS`. Include the unified headers instead.

Note: First-class links (symlinks) are planned; in the interim, `PathAlias` offers robust forwarding/retargeting semantics without changing core trie invariants.

### Hit testing and auto-render scheduling (October 16, 2025)
- `PathSpace::UI::Builders::Scene::HitTest` reads the current snapshot (`DrawableBucketSnapshot`) for a scene, walks draw order (opaque + alpha) back-to-front, respects per-drawable clip stacks, and returns the topmost hit along with authoring focus metadata. Results now include scene-space coordinates plus local-space offsets derived from the drawable’s bounds so input handlers can translate pointer positions without re-reading scene state.
- `HitTestRequest.schedule_render` enqueues `AutoRenderRequestEvent` items under `renderers/<rid>/targets/<kind>/<name>/events/renderRequested/queue`; the auto-render loop can watch this queue to trigger responsive redraws after pointer interactions.
- Utility helpers in `src/pathspace/ui/DrawableUtils.hpp` centralize drawable bounds tests, focus-chain construction, and coordinate conversions so raster and hit-test paths stay consistent.
- `Scene::MarkDirty` updates `scenes/<scene-id>/diagnostics/dirty/state` with a monotonically increasing sequence, merged dirty mask, and timestamp, and pushes matching `Scene::DirtyEvent` entries onto `diagnostics/dirty/queue`. `Scene::TakeDirtyEvent` (blocking) and `Scene::ReadDirtyState` give renderers a wait/notify friendly surface for scheduling layout/rebuild work without polling. Loop-harness coverage (`tests/ui/test_Builders.cpp`) waits on `Scene::TakeDirtyEvent` before issuing `Scene::MarkDirty`, confirming wake latency stays under 200 ms once a dirty event is published and that FIFO ordering is preserved.

## UI & Rendering

The scene graph and renderer pipeline lives entirely on top of PathSpace paths. Applications mount their rendering tree under a single app root (see `docs/Plan_SceneGraph_Renderer.md` and `docs/AI_Paths.md`) so that tearing down the app root atomically releases windows, surfaces, scenes, and renderer targets in one operation. All references between components are app-relative strings validated through the dedicated helpers in `SP::App` (`SP::App::is_app_relative`, `SP::App::resolve_app_relative`, `SP::App::ensure_within_app`).

- Raw user input that has not yet been validated should flow through `SP::UnvalidatedPathView`; the helper layer makes the transition to `ConcretePath`/`ConcretePathView` explicit before touching core storage.

Build toggles (CMake options, default in parenthesis):
- `PATHSPACE_ENABLE_EXTRA` (ON) — layer/view/IO providers, platform discovery helpers, and their tests. Set to OFF to consume only the core data-space API.
- `PATHSPACE_ENABLE_APP` (ON) — app-level helpers in `SP::App`.
- `PATHSPACE_ENABLE_UI` (OFF) — UI/scene graph helper stubs; see `docs/Plan_SceneGraph_Renderer.md` for downstream feature guards (`PATHSPACE_UI_SOFTWARE`, `PATHSPACE_UI_METAL`).

### Layering and Synchronization
- PathSpace core retains exclusive ownership of concurrency primitives (per-node mutexes, wait queues). Higher layers—including SceneGraph helpers—coordinate via atomic inserts/takes, revision counters, and notifications; they must not introduce external mutexes.
- Typed helper functions (`Scene::Create`, `Renderer::UpdateSettings`, etc.) now delegate to the builder layer in `include/pathspace/ui/Builders.hpp`, which centralizes app-root containment and path derivation before touching `PathSpace` storage. These helpers are restart-friendly: repeated `Create` calls simply return the existing concrete path, and diagnostics reads default to zeroed metrics when outputs are absent. The `SP::UI` façade remains thin wrappers over these builders.
- Tests can substitute `PathSpace` fixtures or helper fakes to validate SceneGraph behaviour independently from core internals, provided they respect the same atomic contracts.

### Component landscape
- `scenes/<scene-id>/` — authoring source (`src/`), immutable builds (`builds/<revision>/`), and `current_revision` pointing at the latest published snapshot.
- `renderers/<renderer-id>/targets/<kind>/<name>/` — renderer targets. Each target binds to a scene, drains whole-frame `RenderSettings`, executes `render`, and publishes the most recent outputs under `output/v1/...`.
- `surfaces/<surface-id>/` — offscreen render entry. Surfaces coordinate with a renderer target, provide per-frame execution (`render`), and surface the output handles/buffers the presenters consume.
- `windows/<window-id>/views/<view-id>/` — presenters that resolve a `surface`, `windowTarget`, or `htmlTarget`; surface/window bindings drive native presents, while HTML bindings expose the most recent DOM/CSS/Canvas payload for web presenters and tooling.
- Upcoming C++ stubs ship under `src/pathspace/ui/` (`PathRenderer2D`, `PathSurfaceSoftware`, `PathWindowView`) and will wire directly into these paths. Keep this section updated as the implementations land.

### Data flow
The following Mermaid diagram documents how scene snapshots propagate to render targets, surfaces, and window presenters. The source lives at `docs/images/ui_rendering_flow.mmd`.

```mermaid
%% Source: docs/images/ui_rendering_flow.mmd
flowchart TD
    subgraph Scenes
        A[scenes/<scene-id>/src]
        B[SnapshotBuilder]
        C[scenes/<scene-id>/current_revision]
        A --> B
        B --> C
    end

    subgraph Renderer Target
        D[renderers/<renderer-id>/targets/<kind>/<name>/scene]
        E[renderers/<renderer-id>/targets/<kind>/<name>/render]
        F[renderers/<renderer-id>/targets/<kind>/<name>/output/v1/...]
        C --> D
        E --> F
    end

    subgraph Surface
        G[surfaces/<surface-id>/render]
        H[surfaces/<surface-id>/output]
        D --> G
        G --> E
        F --> H
    end

    subgraph Window View
        I[windows/<window-id>/views/<view-id>/present]
        J[windows/<window-id>/window]
        H --> I
        I --> J
    end
```

### Atomic pipeline
Renderer targets adopt settings atomically and publish immutable outputs. The sequence diagram (stored in `docs/images/render_atomic_pipeline.mmd`) captures the contract between producers, renderers, surfaces, and presenters.

```mermaid
%% Source: docs/images/render_atomic_pipeline.mmd
sequenceDiagram
    autonumber
    participant Producer as Producer (Surface/Presenter)
    participant Target as Renderer Target Paths
    participant Renderer as PathRenderer
    participant Surface as PathSurface
    participant Presenter as Window View

    Producer->>Target: insert RenderSettings (settings/inbox)
    Renderer->>Target: take() latest settings
    Renderer->>Target: update settings/active
    Surface->>Renderer: invoke render
    Renderer->>Target: publish output/v1/common & buffers
    Presenter->>Surface: trigger present (optional frame)
    Presenter->>Target: read output/v1/*
```

Atomicity rules:
- Writers insert whole `RenderSettingsV1` objects into `settings/inbox`; partial updates are not supported. The renderer drains the inbox with `take()` and only adopts the newest payload.
- Targets may mirror the adopted settings under `settings/active` for introspection. Mirrors must be written atomically so consumers never observe a mixed version.
- Frame outputs live under `output/v1/...` and are single-value registers (software framebuffers, GPU handles, timing metadata). Replace-in-place updates keep consumers lockstep with renderer commits.
- Surfaces coordinate per-target renders, double-buffer software pixels, and ensure GPU handles remain valid until presenters finish presenting the active frame.

### Progressive software present
- The software renderer’s progressive mode shares a single CPU framebuffer (`RGBA8Unorm_sRGB`, premultiplied alpha). Rendering occurs in linear light; the renderer encodes to sRGB on store, and presenters copy the resulting bytes without further conversion, preventing double-encode artifacts.
- Framebuffer tiles maintain a small seqlock metadata block: `seq` (`std::atomic<uint32_t>`, even=stable, odd=writer active), `pass` (`std::atomic<uint32_t>` with states {None, OpaqueInProgress, OpaqueDone, AlphaInProgress, AlphaDone}), and `epoch` (`std::atomic<uint64_t>` monotonic per AlphaDone). All stores use release semantics; readers pair them with acquire loads.
- Writers flip `seq` to odd (`fetch_add(1, memory_order_acq_rel)`), optionally mark the in-progress pass, write pixels, issue a release fence, update `pass` (and `epoch` when reaching AlphaDone), then flip `seq` back to even. Presenters read `seq`/`pass`/`epoch` with acquire semantics; if `seq` is odd or differs before/after a copy, they discard the tile to avoid tearing.
- Dirty rectangles stay tile-aligned. The presenter coalesces tile IDs, copies even-seq tiles immediately, and may present opaque-complete tiles before alpha tiles finish. Metrics recorded under `output/v1/common` – `progressiveTilesCopied`, `progressiveRectsCoalesced`, `progressiveSkipOddSeq`, `progressiveRecopyAfterSeqChange` – capture copy throughput and retry behaviour, enabling performance analysis of progressive mode.
- `PathWindowView` encapsulates the presenter logic for software surfaces, exposes `PresentStats` (frame metadata, render/present durations, skip flags), and Builders persist those stats under `targets/<tid>/output/v1/common/*` for diagnostics consumers.
- On macOS the shared UI bridge (`src/pathspace/ui/LocalWindowBridge.mm`) now owns a `CAMetalLayer`, grabs `CAMetalDrawable` instances, copies the most recent software framebuffer directly into the drawable’s `IOSurface`, and presents via an empty Metal command buffer. Examples call the bridge so presenter logic lives alongside `PathWindowView`, eliminating platform-specific presenter code in sample apps.
- PathSurfaceSoftware now exposes an IOSurface-backed framebuffer, allowing PathWindowView and macOS presenters to bind the same IOSurface without memcpy; the legacy copy path remains only as a diagnostics fallback.
- PathRenderer2D now reuses per-target caches of drawable bounds plus the fingerprints emitted in `fingerprints.bin` so unchanged drawables (including id renames) skip damage; only tiles touched by modified geometry or settings are repainted, with full-surface fallbacks reserved for resize/clear color changes (see docs/Plan_SceneGraph.md “Incremental software renderer” for remaining diagnostics/hint follow-ups).
- Renderer targets accept optional dirty-rectangle hints under `targets/<tid>/hints/dirtyRects`; PathRenderer2D consumes and unions those rectangles with fingerprint-derived damage so producers can force or narrow incremental updates without triggering a full repaint.
- When `capture_framebuffer=true` in the present policy, `Builders::Window::Present` persists the software framebuffer under `output/v1/software/framebuffer`; the helpers (`Builders::Diagnostics::ReadSoftwareFramebuffer`) provide typed access (width/height/stride metadata included) so tools and examples (see `examples/paint_example.cpp`) can display the rendered bytes without re-rendering. With capture disabled, the path stays empty to avoid per-frame serialization overhead.
- `SurfaceDesc` now captures Metal-specific options (`metal.storage_mode`, `metal.texture_usage`, `metal.iosurface_backing`) so renderers can configure texture allocation deterministically. `RenderSettings::surface.metal` mirrors those options per frame, and `RenderSettings::renderer` records the backend actually used plus whether Metal uploads were enabled, giving diagnostics downstream context when targets fall back to software.
- When Metal uploads are enabled, `PathWindowView` now presents the steady-state Metal texture via the shared CAMetalLayer, recording `gpuEncodeMs` (command encoding time) and `gpuPresentMs` (time until the drawable is scheduled) alongside the existing `presentMs` entry in `output/v1/common`. Software presents keep those fields at `0.0` so tooling can tell which path executed.

#### Renderer module layout (October 28, 2025)

- `src/pathspace/ui/PathRenderer2D.cpp` now focuses on orchestration (settings adoption, damage selection, material bookkeeping, stats publication) while delegating specialised work to companion translation units.
- `PathRenderer2DDamage.cpp` owns damage region math and tile utilities; `PathRenderer2DEncode.cpp` handles linear-buffer clears plus encode job fan-out; `PathRenderer2DColor.cpp` centralises color conversion helpers so both software and Metal paths share the same math.
- `PathRenderer2DDetail{Common,Draw,Encode}.cpp` and `PathRenderer2DDetail.hpp` expose reusable draw-command primitives consumed by both the software renderer and the Metal encoder. Shared state such as `DamageRect` and encode worker helpers now live in `PathRenderer2DInternal.hpp`.
- UI builder entry points that previously lived in a monolithic `ui/Builders.cpp` are split across focused units: `SceneBuilders.cpp`, `RendererBuilders.cpp`, `SurfaceBuilders.cpp`, `WindowBuilders.cpp`, `AppBuilders.cpp`, plus widgets-specific files (`WidgetBuildersCore.cpp`, `WidgetBindings.cpp`, `WidgetFocus.cpp`, `WidgetReducers.cpp`, `WidgetTrace.cpp`). Diagnostics helpers live beside them in `DiagnosticsBuilders.cpp`.
- When updating renderer or builder behaviour, touch the matching translation unit rather than reintroducing a grab bag file; keep the doc cross-references in this section aligned so contributors can navigate directly to the relevant implementation.

### Snapshot integration
- Snapshot builder publishes immutable scene revisions beneath `scenes/<id>/builds/<revision>/...` and atomically updates `current_revision` when a revision is ready.
- Renderer targets resolve the scene by reading `targets/<kind>/<name>/scene` and load the referenced `current_revision`. They bump snapshot reference counts during adoption and release once the frame completes (see `docs/Plan_SceneGraph_Renderer.md`, “Decision: Snapshot retention”).
- The C++ helper (`SceneSnapshotBuilder`, added October 14, 2025) clamps drawable SoA integrity and now emits structured binary artifacts (`drawables.bin`, `transforms.bin`, `bounds.bin`, `state.bin`, `cmd-buffer.bin`, plus optional index files) under `scenes/<id>/builds/<revision>/bucket/`. A compact binary manifest stored at `drawable_bucket` records counts and layer ids for consumers. After writing the files it atomically publishes via `Builders::Scene::PublishRevision`, maintains a rolling index at `meta/snapshots/index`, and enforces the default retention policy (≥3 revisions or ≥2 minutes) while preserving `current_revision`.
- PathRenderer2D (October 16, 2025) composites Rect, RoundedRect, Image, TextGlyphs, Path, and Mesh commands in linear light, honors opaque/alpha partitioning from `pipelineFlags`, and publishes drawable/command/unsupported counts alongside the usual timing metrics.
- Builders’ `Surface::RenderOnce` and `Window::Present` (October 15, 2025) synchronously invoke `PathRenderer2D`, returning a ready `FutureAny` handle while the renderer updates `output/v1/common/*`; asynchronous scheduling remains TODO.
- Metrics for snapshot GC (`retained`, `evicted`, `last_revision`, `total_fingerprint_count`) live under `scenes/<id>/metrics/snapshots` so render loops can surface health in UI tooling.

### HTML adapter modes
- Renderer targets select an adapter via `renderers/<rid>/targets/html/<name>/adapter_mode` (enum: `canvas_replay`, `webgl`, `dom_bridge`). Adapters own capability detection: they probe the runtime, negotiate fallbacks, and surface the final choice under `output/v1/html/metadata/active_mode`.
- All modes share the same command stream contract: `output/v1/html/commands` stores a compact JSON array mirroring the `DrawableBucket` ordering (opaque before alpha) with resource fingerprints for textures, glyph atlases, and shader bundles. The adapter runtime hydrates resources from `output/v1/html/assets/*` using those fingerprints.
- Canvas replay ships as the baseline runtime. The manifest (`renderers/<rid>/targets/html/<name>/manifest.json`) declares the JS module that replays commands on a `<canvas>`, supported features, and the minimum schema version. Presenters load the module referenced by `manifest.runtime.module` and stream frames by diffing fingerprints.
- WebGL mode adds optional GPU acceleration. The manifest lists shader binaries or pipelines under `assets/shaders/*` and flags optional capabilities (e.g., `EXT_disjoint_timer_query`, MSAA sample counts). If the presenter or browser does not meet the requirements, the adapter downgrades to Canvas replay without changing the command stream.
- DOM bridge layers semantic HTML on top of the Canvas runtime for accessibility. The manifest’s `dom_overlays` section maps drawable categories (text, rects) to DOM templates and attributes. When enabled, presenters render Canvas first, then materialize DOM overlays based on `output/v1/html/overlays` produced by the adapter.
- Versioning: `manifest.json` carries `{ "schema_version": 1 }`. Breaking changes increment the schema and adapters must retain compatibility with the previous version for at least one release. Resource fingerprints always include the schema version so stale presenters fall back safely.

### Contributor checklist
- Mount new UI components under the application root and use the typed path helpers to validate app-relative references before storing them.
- Widget toolkit: prefer the shared builder APIs (`Builders::Widgets::CreateButton`, `CreateToggle`, `CreateStack`, etc.) when adding UI primitives. Keep state paths (`/widgets/<id>/state`, `/meta/style`, `/layout/*`, etc.), canonical authoring nodes (`/widgets/<id>/authoring/...`), and update helpers (`UpdateButtonState`, `UpdateToggleState`, `UpdateStackLayout`) in sync with any new widgets.
- Stack layout metadata lives under `/widgets/<id>/layout/{style,children,computed}`. `style` records axis/alignment/spacing, `children` lists widget roots + per-child constraints, and `computed` caches the latest layout frames. Bindings reuse `layout/computed` to derive dirty hints and auto-render events when layouts change.
- When adding renderer targets or surfaces, document the path contract in `docs/Plan_SceneGraph_Renderer.md` and ensure tests cover atomic settings adoption and output publication (loop=15).
- Update the Mermaid sources in `docs/images/` if data flow or pipeline steps change; keep the inline diagrams in this section synchronized.
- Cross-link new C++ entry points (`PathRenderer2D`, `PathSurface*`, `PathWindowView`, etc.) from this section so contributors can navigate between docs and code.
