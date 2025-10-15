Renderer snapshot builder details have moved out of this architecture document. See docs/AI_Plan_SceneGraph_Renderer.md (“Decision: Snapshot Builder”) for the authoritative policy, rebuild triggers, publish/GC protocol, and performance notes. This file focuses on PathSpace core (paths, trie storage, concurrency/wait/notify, views/alias layers, and OS I/O). New AI assistants should first walk through docs/AI_Onboarding.md for the session bootstrap checklist.

## UI/Rendering — cross-reference

Present policy (backend-aware) and the software progressive present are documented in docs/AI_Plan_SceneGraph_Renderer.md. This architecture document focuses on PathSpace core; rendering/presenter details live in the plan. Also see “View keys (final)” and “Target keys (final)” in docs/AI_Plan_SceneGraph_Renderer.md for the authoritative schemas, and note that RenderSettings are a single-path atomic whole-object value; ParamUpdateMode::Queue refers to client-side coalescing before one atomic write (no server-side queue in v1).

See also:
- `docs/AI_Plan_SceneGraph_Renderer.md` for the broader rendering plan and target I/O layout. If snapshot semantics change, update both documents in the same PR per `.rules`.
 - `docs/AI_PATHS.md` for the canonical path namespaces and layout conventions; update it alongside changes to path usage and target I/O layout.


## Contributing to PathSpace

For AI-facing workflows (branching, PR creation, troubleshooting, commit guidelines), see AGENTS.md. This document focuses on code architecture and APIs.

PathSpace is a coordination language that enables insertion and extractions from paths in a thread safe datastructure. The data structure supports views of the paths similar to Plan 9. The data attached to the paths are more like a JSON datastructure than files though. The data supported is standard C++ data types and data structures from the standard library, user created structs/classes as well as function pointers, std::function or function objects for storing executions that generate values to be inserted at a path.

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
- Canonical device namespace (aligned with SceneGraph plan; see `docs/AI_PATHS.md`):
  - Inputs:
    - `/system/devices/in/pointer/default/events`
    - `/system/devices/in/text/default/events`
    - `/system/devices/in/gamepad/default/events`
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

## UI & Rendering

The scene graph and renderer pipeline lives entirely on top of PathSpace paths. Applications mount their rendering tree under a single app root (see `docs/AI_Plan_SceneGraph_Renderer.md` and `docs/AI_PATHS.md`) so that tearing down the app root atomically releases windows, surfaces, scenes, and renderer targets in one operation. All references between components are app-relative strings validated through the dedicated helpers in `SP::App` (`SP::App::is_app_relative`, `SP::App::resolve_app_relative`, `SP::App::ensure_within_app`).

- Raw user input that has not yet been validated should flow through `SP::UnvalidatedPathView`; the helper layer makes the transition to `ConcretePath`/`ConcretePathView` explicit before touching core storage.

Build toggles (CMake options, default in parenthesis):
- `PATHSPACE_ENABLE_EXTRA` (ON) — layer/view/IO providers, platform discovery helpers, and their tests. Set to OFF to consume only the core data-space API.
- `PATHSPACE_ENABLE_APP` (ON) — app-level helpers in `SP::App`.
- `PATHSPACE_ENABLE_UI` (OFF) — UI/scene graph helper stubs; see `docs/AI_Plan_SceneGraph_Renderer.md` for downstream feature guards (`PATHSPACE_UI_SOFTWARE`, `PATHSPACE_UI_METAL`).

### Layering and Synchronization
- PathSpace core retains exclusive ownership of concurrency primitives (per-node mutexes, wait queues). Higher layers—including SceneGraph helpers—coordinate via atomic inserts/takes, revision counters, and notifications; they must not introduce external mutexes.
- Typed helper functions (`Scene::Create`, `Renderer::UpdateSettings`, etc.) now delegate to the builder layer in `include/pathspace/ui/Builders.hpp`, which centralizes app-root containment and path derivation before touching `PathSpace` storage. These helpers are restart-friendly: repeated `Create` calls simply return the existing concrete path, and diagnostics reads default to zeroed metrics when outputs are absent. The `SP::UI` façade remains thin wrappers over these builders.
- Tests can substitute `PathSpace` fixtures or helper fakes to validate SceneGraph behaviour independently from core internals, provided they respect the same atomic contracts.

### Component landscape
- `scenes/<scene-id>/` — authoring source (`src/`), immutable builds (`builds/<revision>/`), and `current_revision` pointing at the latest published snapshot.
- `renderers/<renderer-id>/targets/<kind>/<name>/` — renderer targets. Each target binds to a scene, drains whole-frame `RenderSettings`, executes `render`, and publishes the most recent outputs under `output/v1/...`.
- `surfaces/<surface-id>/` — offscreen render entry. Surfaces coordinate with a renderer target, provide per-frame execution (`render`), and surface the output handles/buffers the presenters consume.
- `windows/<window-id>/views/<view-id>/` — presenters that resolve a `surface`, optionally trigger a frame, and blit/draw into the platform window shell.
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
- Dirty rectangles stay tile-aligned. The presenter coalesces tile IDs, copies even-seq tiles immediately, and may present opaque-complete tiles before alpha tiles finish. Metrics record skips (odd `seq`), recopies after `seq` changes, and copy throughput, enabling performance analysis of progressive mode.
- `PathWindowView` encapsulates the presenter logic for software surfaces, exposes `PresentStats` (frame metadata, render/present durations, skip flags), and Builders persist those stats under `targets/<tid>/output/v1/common/*` for diagnostics consumers.

### Snapshot integration
- Snapshot builder publishes immutable scene revisions beneath `scenes/<id>/builds/<revision>/...` and atomically updates `current_revision` when a revision is ready.
- Renderer targets resolve the scene by reading `targets/<kind>/<name>/scene` and load the referenced `current_revision`. They bump snapshot reference counts during adoption and release once the frame completes (see `docs/AI_Plan_SceneGraph_Renderer.md`, “Decision: Snapshot retention”).
- The C++ helper (`SceneSnapshotBuilder`, added October 14, 2025) clamps drawable SoA integrity and now emits structured binary artifacts (`drawables.bin`, `transforms.bin`, `bounds.bin`, `state.bin`, `cmd-buffer.bin`, plus optional index files) under `scenes/<id>/builds/<revision>/bucket/`. A compact binary manifest stored at `drawable_bucket` records counts and layer ids for consumers. After writing the files it atomically publishes via `Builders::Scene::PublishRevision`, maintains a rolling index at `meta/snapshots/index`, and enforces the default retention policy (≥3 revisions or ≥2 minutes) while preserving `current_revision`.
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
- When adding renderer targets or surfaces, document the path contract in `docs/AI_Plan_SceneGraph_Renderer.md` and ensure tests cover atomic settings adoption and output publication (loop=15).
- Update the Mermaid sources in `docs/images/` if data flow or pipeline steps change; keep the inline diagrams in this section synchronized.
- Cross-link new C++ entry points (`PathRenderer2D`, `PathSurface*`, `PathWindowView`, etc.) from this section so contributors can navigate between docs and code.
