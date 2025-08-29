Snapshot builder policy (summary): patch-first incremental snapshots with copy-on-write; fall back to a full rebuild on global parameter changes (e.g., DPI/root constraints/theme/color space/font tables) or when fragmentation/performance thresholds are exceeded. See "Snapshot Builder and Rebuild Policy" below.

## Snapshot Builder and Rebuild Policy

Purpose:
- Provide a renderer-facing, immutable scene snapshot per revision, built incrementally when possible, with a well-defined fallback to full rebuild.

Key concepts:
- Incremental, patch-first: maintain the previous snapshot in memory; apply targeted patches for small edits and only rebuild fully on global changes.
- Copy-on-write: snapshots share unchanged structures; modified subtrees allocate new chunks.
- Dirty flags and epochs: per-node flags (STRUCTURE, LAYOUT, TRANSFORM, VISUAL, TEXT, BATCH) and computed-epoch counters to skip unchanged work.
- Chunked draw lists: draw ops are stored per-subtree; unaffected chunks are reused; merges are k-way by stable sort keys.
- Text shaping cache: shaped runs keyed by font+features+script+dir+text hash; only re-shape dirty runs.

Patch pipeline (incremental pass order):
1) Change ingestion → mark dirty and propagate (up/down) with early cut-offs at independent subtrees.
2) Measure/Text: re-shape dirty text; update intrinsic sizes.
3) Layout: re-layout dirty subtrees; contain ripples to constraint islands.
4) Transform: recompute world transforms and bounds for dirty subtrees (pre-order).
5) Batching/Flatten: rebuild only affected chunks; merge and stabilize paint order.
6) Validate changed regions; assemble a new revision with copy-on-write.

Publish and retention:
- Write to `builds/<revision>.staging/...` then atomically rename to `builds/<revision>`; atomically replace `current_revision`.
- Retain the last K revisions (default 3) and GC older ones after a TTL, deferring deletion if a renderer still references them.

When to trigger a full rebuild:
- Global parameters changed: DPI/root constraints/camera/theme/color space/font tables.
- Structure churn: inserts+removes > 15% of nodes or reparent operations touch > 5%.
- Batching churn: > 30% of draw ops move buckets, or widespread stacking-context changes.
- Fragmentation: tombstones > 20% in node/draw-chunk storage; indices significantly degraded.
- Performance: 3 consecutive frames over budget, or moving-average patch cost ≥ 70% of last full rebuild.
- Consistency: validations detect invariant violations (cycles, bounds, sort instability).

Configuration knobs (defaults are conservative and tunable per app/scene):
- Debounce windows (min interval, max staleness), concurrency caps, cache sizes, revision retention (K, TTL).

Performance notes:
- Common passes are O(N_dirty) and parallelizable; copy-on-write keeps memory locality high.
- Renderers are agnostic to build mode; they consume `builds/<revision>` referenced by `current_revision`.

See also:
- `docs/AI_Plan_SceneGraph_Renderer.md` for the broader rendering plan and target I/O layout. If snapshot semantics change, update both documents in the same PR per `.rules`.


AI autonomy guideline:
- The AI should complete tasks end-to-end without asking the user to run commands or finish steps. Use the provided scripts and tooling to build, test, and validate changes (e.g., `./scripts/compile.sh --clean --test --loop=N`).
- Only defer to the user when blocked by missing credentials, unavailable external services, or ambiguous requirements that cannot be resolved from the repository context.
- Prefer making conservative, reversible changes; keep edits minimal and focused, and ensure all references and paths are valid.



AI pull request workflow (to avoid stale commits and noisy PR history):
- Always branch from the current default branch (usually `master`):
  - git fetch origin
  - git checkout -b feat/<short-topic> origin/master
- Keep your branch up to date during the work:
  - git fetch origin
  - git rebase origin/master
- If you accidentally started from an old topic branch, create a clean branch and cherry-pick only your commits:
  - git checkout -b fix/<topic>-clean origin/master
  - git cherry-pick <commit1> [<commit2> ...]
- Use the helper script to open a PR cleanly:
  - ./scripts/create_pr.sh -b master -t "Short human-friendly title"
  - If the PR shows unrelated older commits, close it and create a fresh branch from `origin/master`, then cherry-pick your changes and re-run the script.
- If pre-push hooks run local builds/tests you can skip them for a quick PR push:
  - SKIP_LOOP_TESTS=1 SKIP_EXAMPLE=1 git push -u origin HEAD

PR authoring guidelines for AI:
- PR titles must not include Conventional Commit prefixes (e.g., "chore(...):", "fix:"). Start with a capital letter and keep them short and human-friendly.
- PR bodies should use the minimal template in `.github/PULL_REQUEST_TEMPLATE.md`: include a concise "Purpose" (1–3 sentences) and an "AI Change Log" (file-by-file summary of edits).
- Validate docs and code references against repo paths per `.rules`.

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
It's possible to send a blocking object to insert/read/extract instructing it to wait a certain amount of time for data to arrive if it is currently empty or non-existent.

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
- Canonical device namespace (aligned with SceneGraph plan):
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
