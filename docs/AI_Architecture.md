# PathSpace — Architecture

Core architecture reference: the node trie, paths, wait/notify, execution model, layers, and the undo history layer. For workflow and conventions see `AGENTS.md`; for canonical path layout see `docs/AI_Paths.md`.

## Contributing to PathSpace

For AI-facing workflows (branching, PR creation, troubleshooting, commit guidelines), see AGENTS.md. This document focuses on code architecture and APIs.

PathSpace is a coordination language that enables insertion and extractions from paths in a thread safe datastructure. The data structure supports views of the paths similar to Plan 9. The data attached to the paths are more like a JSON datastructure than files though. The data supported is standard C++ data types and data structures from the standard library, user created structs/classes as well as function pointers, std::function or function objects for storing executions that generate values to be inserted at a path.

### Undo History Layer Guidance
- Each history-enabled subtree owns exactly one undo/redo stack. We do not coordinate history across multiple roots; a logical command must mutate a single root so one stack captures the full change.
- If a feature naturally spans several areas, route it through a command-log subtree or reorganize the data so related metadata lives alongside the primary payload before enabling history. `enableHistory` now rejects configurations that expect shared stacks—setting the same `HistoryOptions::sharedStackKey` on multiple roots triggers an error so callers regroup under a single undo root instead.
- Undo entries flush to disk by default for crash safety. Advanced callers can set `HistoryOptions::manual_garbage_collect` or insert `_history/set_manual_garbage_collect = true` when they want to defer durability and retention to explicit checkpoints.
- `history::UndoableSpace` journals mutations: the log records inserts and takes together with inverse payloads, so undo/redo replays them without walking the trie. Retention budgets apply (auto-trim plus manual garbage collect); `_history/stats/*` and `_history/lastOperation/*` are derived from `UndoJournalState::Stats`, and `_history/unsupported/*` records nodes that cannot be serialized (executions, nested spaces) because their payloads are skipped when before/after state is captured.
- Writing `_history/set_tag = "<label>"` (a `std::string`) stamps the label onto each following journal entry and mirrors it to `_history/lastOperation/tag`, so tooling can show command names next to undo/redo telemetry.
- Persistence metadata uses a versioned little-endian binary header shared by recovery, telemetry and tooling; `_history/stats/*` is the authoritative introspection surface.
- `docs/History_Journal_Wiring.md` describes how `UndoableSpace` sits between callers and the inner `PathSpace`, how the `_history/*` control paths are intercepted, why `notify()` still reaches existing waiters, and which guardrails (`allowNestedUndo`, `HistoryOptions::sharedStackKey`) apply before a root is registered.

##### Stack plumbing & notifications
- `JournalTransactionGuard` (and the public `HistoryTransaction` around it) owns one level of a root's open transaction. Moving transfers that ownership, and destroying a guard that was not committed commits it: the recorded mutations already reached the inner space, so they are journaled rather than dropped, and an early return or exception can never leave the root locked for other threads. Undo/redo move the journal cursor only after the step was applied to the tree, so a failed step leaves both in agreement.
- Values that were inserted into the inner space before it was wrapped may sit on a node's POD lane. Reads through the wrapper (which always ask for the generic representation) upgrade such a lane to generic storage when the type matches, and the journal's before/after capture includes the lane's elements.
- `UndoableSpace` resolves every mutation through `findJournalRootByPath`, so inserts/takes under a history-enabled subtree always allocate a `JournalTransactionGuard`, capture "before" payloads, and write journal entries before committing the inner operation. If the path falls outside a registered root, the wrapper short-circuits and forwards the call unchanged.
- `_history/undo`, `_history/redo`, `_history/garbage_collect`, and `_history/set_manual_garbage_collect` are implemented as regular inserts whose payloads are interpreted by `handleJournalControlInsert`. This keeps tooling simple: scripting and inspector clients can keep issuing inserts without linking against C++ helpers.
- `notify()` and `visit()` both delegate to the wrapped `PathSpace`. Waiters that previously subscribed to `<root>/**` (including `_history/stats/*` telemetry nodes) continue to fire because journal mutations ultimately write into the same canonical nodes inside the inner space.
- Persistence setup happens lazily when `HistoryOptions::persistHistory` is set. The wrapper encodes `<root>` into a per-space UUID directory, ensures the directory exists, calls `loadJournalPersistence()`, and updates `_history/stats/manualGcEnabled` / disk telemetry whenever compaction runs.

##### Mutation guardrails
- `UndoableSpace` overrides `PathSpaceBase::in`/`out` and wraps every mutation in `beginJournalTransactionInternal`, capturing before/after payload snapshots and replay metadata via `recordJournalMutation`. All live history writes therefore flow through the journal regardless of the public API the caller invokes (`insert`, `take`, history control commands).
- When adding a new mutator (e.g., a bulk update helper or custom transaction primitive), route the implementation through `UndoableSpace` by reusing the existing transaction helpers and emitting a journal mutation for each logical change. Bypassing these helpers will desynchronise undo/redo state and persistence—treat it as a correctness bug.
- External contributors introducing new mutation paths must update this guardrail section and add regression coverage mirroring the patterns in `tests/unit/history/test_UndoableSpace.cpp` (`journal telemetry matches snapshot telemetry outputs`, fuzz/concurrency tests). The maintainer team will run the same `rg 'snapshot' tests/unit/history` audit to ensure snapshot-era helpers do not reappear.

#### Journal Persistence Format
- Journal files live at `<persistence-root>/journal.log` and start with a 10-byte header: `0x50534A46` (`'PSJF'` little endian) magic, `uint16` format version (currently `1`), and a reserved `uint32` set to zero. The header is fsynced when the writer is asked to persist eagerly.
- Root directories mirror the history namespace: the persistence namespace becomes the top-level folder, and each undo root is encoded as `"__root"` for `/` or the concrete path with `/` replaced by `_` (e.g. `/doc/article` -> `_doc_article`).
- Each appended record is length-prefixed with a `uint32` (little endian) so tools can stream entries without parsing the payload. Compaction rewrites the header plus the surviving records in the same framing.
- Compaction (`compactJournalPersistence`, which calls `compactJournal`) runs whenever the file holds records the in-memory journal no longer has: when retention trims entries, when a new edit after an undo discards the redo tail (that commit rewrites the file instead of appending), and after loading when retention trimmed during the load. The journal writer is closed before the file is replaced and reopens on the next commit; `importHistorySavefile` follows the same rule.
- The undo position is not stored in `journal.log`. An undo that is not followed by a new edit is therefore not durable: after a restart every record is replayed and the redo stack is empty. This is a known gap that needs a format decision (see `docs/State_Machine_Audit.md`, H4).
- Record payloads follow the `'PSJL'` (`0x50534A4C`) entry schema:
  - `uint16` entry version (`1`), `uint8` operation kind (`0` insert, `1` take), `uint8` flag byte (`0x01` means barrier), and a reserved `uint16`.
  - Three `uint64` fields: wall-clock timestamp in milliseconds since epoch, monotonic timestamp in nanoseconds, and the monotonic sequence number assigned by `UndoJournalState`.
  - Path bytes are stored as `<uint32 length><utf-8 path>` with no null terminator.
  - Two serialized payload blocks encode the post-mutation value (`value`) and the inverse payload (`inverseValue`). Each block is `<uint8 present flag><uint32 byte length><raw bytes>`. When the flag is zero the block carries zero bytes; otherwise it contains the `NodeData::serializeSnapshot` output so tooling can reuse the existing decoding helpers.
- The writer and replayer operate in host byte order today (little endian on all supported targets). Tooling that needs to run on big-endian machines should transcode explicitly before emitting or consuming records.
- Crash recovery: a writer that dies in the middle of an append leaves a partial last record. Loading a persisted history replays the complete records, drops the partial one, and truncates the file to the last complete record so later appends stay well-framed (`replayJournal` with `JournalReplayOptions::tolerateTornTail`; strict replay still reports it as `MalformedInput`). A record length larger than the rest of the file is rejected before any memory is allocated for it. The records carry no checksum, so a corrupt length in the middle of the file is indistinguishable from a torn tail and ends the replay at that point.
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
- `[a-z]` - Matches any character in the range; `[!...]` negates the set. A `-` that cannot form a range (first, last, or right after a range, as in `[a-c-e]`) is a literal `-`
- `\` - Escapes special characters

`match_names` (used by `Leaf` and `WaitMap`) and `GlobName::match` share one component matcher, `matchGlobComponent` in `path/utils`, so they cannot disagree.

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

### Traversal & JSON Export
- `PathSpaceBase::visit(PathVisitor const&, VisitOptions const&)` is the supported way to walk a subtree without exposing `Node*`. `VisitOptions` controls the canonical root, maximum depth, and child limits; `includeValues=false` keeps the walk structural, while `includeNestedSpaces=false` fences nested PathSpaces.
- `VisitOptions::UnlimitedDepth`/`UnlimitedChildren` document the sentinel values, `childLimitEnabled()` reports whether the cap is active, and the exporter/CLI surface `"max_children": "unlimited"` whenever the child limit is disabled (still bounded by `maxDepth`).
- Visitors receive a lightweight `PathEntry` (path string, child count estimate, data category hints) plus a `ValueHandle`. Call `ValueHandle::read<T>()` for typed samples or `snapshot()` when you only need queue metadata; reads never pop values, so instrumentation remains non-destructive.
- `PathSpaceJsonExporter` builds on the visitor API to serialize a hierarchical tree: the payload contains the root-path object with nested `children`, `values`, and truncation flags (`children_truncated`/`depth_truncated`); `_meta` (schema/limits/flags/stats) is optional via `PathSpaceJsonOptions::includeMetadata` and enabled automatically in debug mode. `PathSpaceBase::toJSON` wraps the exporter so tooling can dump subtrees without wiring converters manually; set `visit.includeNestedSpaces=true` when you explicitly need nested PathSpaces. Presets keep behaviour consistent: `Mode::Minimal` (default) disables diagnostics/opaque placeholders/metadata and leaves traversal unbounded by default (`max_depth=unlimited`, `max_children=unlimited`, `max_queue_entries=unlimited`, `include_nested_spaces=false`); `Mode::Debug` re-enables diagnostics, structure fields, opaque placeholders, and metadata for full inspection. The exporter no longer applies path filters—the tree is expected to be lean by construction.

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

### Visitor API and Traversal Helpers

- `PathSpaceBase::visit(PathVisitor const&, VisitOptions const&)` walks the trie without exposing raw `Node*`. Use it in new tooling instead of manually chaining `listChildren` and `read`.
- `VisitOptions` controls the starting path (`root`), depth and child limits, nested-space inclusion, and whether `ValueHandle::read<T>()` is permitted. Depth is relative to the requested root, so `maxDepth = 0` limits the walk to that node.
- Each callback receives a `PathEntry` (path, child counts, nested/value flags) plus a `ValueHandle`. Handles can read the front queue value when `includeValues` is enabled or return snapshots describing the queue layout (type list, queue depth, execution flags, raw-buffer bytes).
- The walker visits nodes depth-first with lexicographically ordered children and automatically steps into mounted spaces when `includeNestedSpaces = true`. Layered views (`PathView`, `PathAlias`, `PathSpaceTrellis`, `History::UndoableSpace`) delegate `visit` so callers always see their logical path space.

#### JSON Exporter

- `PathSpaceBase::toJSON(PathSpaceJsonOptions const&)` wraps `PathSpaceJsonExporter` (`src/pathspace/tools/PathSpaceJsonExporter.{hpp,cpp}`) to dump any subtree as a hierarchical tree: payload = the root-path object with nested `children`, `values`, and truncation flags (`children_truncated`/`depth_truncated`), plus optional `_meta` (schema/limits/flags/stats) when `includeMetadata=true` or debug mode is selected. The exporter walks depth-first with lexicographic children; set `visit.includeNestedSpaces=true` when you explicitly need mounted/nested PathSpaces in the tree.
- `PathSpaceJsonOptions` extends `VisitOptions` with `maxQueueEntries`, `includeOpaquePlaceholders`, `includeDiagnostics`, and `dumpIndent` (nested spaces default to `false` for exports to avoid trellis/runtime mirrors). Queue sampling respects `maxQueueEntries`; truncated exports set `values_truncated=true` so tooling can decide whether to follow up with a deeper crawl. Set `visit.maxChildren = VisitOptions::UnlimitedChildren` when you need the exporter to traverse every child regardless of branch factor (helpers expose `VisitOptions::childLimitEnabled()` when you need to inspect a caller-provided struct).
- Value conversion flows through the registry in `src/pathspace/tools/PathSpaceJsonConverters.hpp`. Bool/integer/float/string types are registered by default (with canonical type names). Use `PathSpaceJsonRegisterConverter<T>(lambda)`, `PathSpaceJsonRegisterConverterAs<T>("FriendlyType", lambda)`, or the `PATHSPACE_REGISTER_JSON_CONVERTER` macro to opt additional structs into JSON export without touching the exporter implementation; the alias you provide shows up in `values[*].type` as well as any placeholder entries, including opaque fallbacks.
- When no converter exists (or value sampling is disabled), the exporter emits placeholders with type/category metadata. Execution queues always produce `{ "placeholder": "execution", "state": "pending" }` entries so debuggers know a callable occupies the node even though its result cannot be inlined.

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
- Use the build helper to loop tests: `./scripts/compile.sh --loop[=N]` (default N=5) to increase the likelihood of catching rare timing issues.

Wait/notify:
- Waiters are stored in a trie-backed registry keyed by concrete paths, with a separate registry for glob-pattern waiters.
- Concrete notifications traverse the trie along the path (O(depth)), while glob notifications filter and match registered patterns.
- Registry structure is guarded by a timed mutex so notify can log if it blocks; each waiter CV now owns its own mutex to reduce contention between notification scans and threads that are waiting.
- `PATHSPACE_DEBUG_WAITMAP=1` logs wait/notify events with the path, thread id, registry lock wait, and wait duration; a watchdog message is emitted if notify needs more than 100ms to acquire the registry lock.

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

A task whose callable throws ends in `TaskState::Failed`. Waiting ends on either terminal state: `Future::wait` returns, timed waits and `copy_result_to` return `false`, and the typed future of a `TaskT` is released as well (`SharedState<T>` is `Pending`, `Ready` or `Failed`; `ready()` stays `false`, `get`/`copy_to` return `false`). What `read`/`take` report for a path whose front execution failed is not settled: today the failed task stays at the front of the node and reads report "Task is not completed" (see `docs/State_Machine_Audit.md`, T3). `NodeData` keeps its task, `Future` and `FutureAny` queues index-aligned, so `read<FutureAny>(path)` always refers to the execution at the front.

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

## Snapshot Cache Layer
- Read-optimized snapshot caching lives in a layer: `SnapshotCachedPathSpace`.
  - A failed rebuild leaves the cache dirty and is retried no sooner than `max(rebuildDebounce, 50 ms)` later (worker and rebuild-on-read alike). `setSnapshotOptions` never resets the in-flight rebuild or a pending worker stop; a rebuild that predates the new options leaves the cache dirty, and starting the worker waits for one that is still winding down.
- The legacy snapshot cache embedded in `PathSpace` has been removed; wrap a space with the layer instead.
- Freshness contract: reads may be stale during active mutations, but after a mutation completes (through the layer) the cache must not return stale data.
- Rebuilds are worker-driven by default; synchronous rebuilds are opt-in via `allowSynchronousRebuild`.

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

- Passing `Block{}` to `read`/`take` (or composing `Out{} & Block{}`) turns the call into a waiter when the target node is empty.
- `Block{}` defaults to a very large timeout (100 years); override it with `Block{std::chrono::milliseconds{budget}}` to enforce a real deadline. When the timeout elapses the call returns `Error::Timeout`. A shutdown or `clear()` also ends the wait with an error.
- `clear()` and `shutdown()` wait for the `in()`/`out()` calls in flight before they wipe the tree: an operation registers itself first and checks the clearing flag second, the clearing side raises the flag first and waits for the count second. A read that arrives during a clear returns `Error::Timeout` ("PathSpace clearing in progress"); an insert that arrives during a clear waits for it to finish and then lands in the cleared space (an insert issued from inside another operation of the same thread on the same space, such as a span callback, is admitted without waiting). Pack inserts, span packs, `visit` and `listChildren` do not take part in this handshake yet.
- Inserts do not block; on completion they notify the path they changed, which wakes the waiters described below. Custom layers can reuse the same behaviour through `BlockOptions` when forwarding or composing higher-level queues.

### Wait/notify protocol
Waiters and notifications meet in the context's `WaitMap` (`src/pathspace/core/WaitMap.*`), shared by a space and its nested spaces through `PathSpaceContext`.

- **Register, then attempt, then wait.** A blocking call first tries the operation once. If that fails it loops: create a `WaitMap::Guard` (which captures the path's notification version), attempt the operation again, and only then wait on the guard. A notification that lands between the attempt and the wait changes the version, so the wait returns immediately; no wakeup is lost. The wait is a plain condition-variable wait on the version, bounded by the deadline.
- **Notifiers bump the version under the waiter's mutex** (`signalEntry` in `WaitMap.cpp`), so a waiter either sees the new version or is already blocked when `notify_all` runs. Lock order is registry mutex, then entry mutex. A `Guard` releases the entry mutex after each wait, so a thread holding a guard can still notify.
- **Keys.** Waiters and notifiers are keyed by the node path without index suffixes (`/a/b[2]/c` is the node `/a/b/c`); `PathSpaceContext::wait`/`notify` normalize this, so nested spaces mounted at `/a/b[2]` notify the same key readers wait on. Glob character classes such as `[a-z]` are kept.
- **Descendants.** `notify(path)` also wakes waiters registered below `path`, because a change at a node (for example a nested space inserted there) can make everything under it readable. Glob waiters are woken when the notified concrete path matches their pattern.
- **Tasks.** A task records an absolute notification path (its space's mount prefix included) and the sink to notify. The sink delivers that path to the context unchanged (`PathSpaceBase::notifyAbsolute`); only direct `notify()` calls add the space's prefix. Re-homing a space (`adoptContextAndPrefix`) rewrites the target of its queued tasks, and a worker re-reads the target when a task finishes, so a re-home during execution takes effect. A re-home also wakes waiters of the previous context so they re-evaluate against the new binding.
- **Safety net.** Each wait is additionally capped at 50 ms (`PATHSPACE_WAIT_RECHECK_MS` overrides it) after which the operation is retried. Normal operation never depends on it; it only bounds the damage if a writer or a custom sink fails to notify. Tests run once with the cap set very high (60000) to prove that no wakeup is being missed.
- **Waker and observer.** A `PathSpaceContext` owns a waker, the sink every task and space notifies (`PathSpaceBase::getNotificationSink` returns it). It delivers into the context, so waiters wake, and then reports to the optional observer sink (`PathSpaceContext::setSink`). Installing an observer never changes how waiters are woken. The waker is detached when the context is destroyed, so a late task completion is dropped safely.
- **Observer re-entrancy.** An observer that notifies its own context from inside its callback is not called recursively. The guard is per thread: a notification sent by another thread while a delivery is in progress is delivered.
- **Clearing the registry.** `WaitMap::clear()` wakes the waiters, waits for them to leave and then destroys the entries. Waiters are counted under the registry lock, and the destruction is skipped when a waiter registered after the drain; its entry stays valid and a later `clear()` cleans up.
- **Context ownership.** A space that adopted another space's context (a nested space, also after it was taken out again) borrows it. Its `clear()`, `shutdown()` and destructor tear down its own tree only; shutting the context down, removing the observer and clearing the wait registry are reserved for the owner. Nested spaces are destroyed together with the node that holds them.

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

### POD fast path and span access
Trivially copyable types (`podPreferred`) are stored in a lock-free single-lane queue (`PodPayload`) instead of serialized `NodeData`. Concurrency contract:
- Inserts (including pack inserts) may run concurrently with every other operation; they only write slots beyond the published tail.
- `take`/`read`/const spans may run concurrently with each other.
- A mutable span (`take(path, [](std::span<T>) {...})` and the spans handed out by `spanPackMut`) has exclusive write access to the elements it covers. Do not run `take`, `read`, a const span or another mutable span on the same lane while such an edit runs; the element values would be a data race. Queue structure and element counts stay consistent either way.
- The payload cannot enforce this itself: pack operations hand raw pointers to the caller after the payload has returned, so exclusion has to come from the caller. Debug builds (NDEBUG undefined, which includes the sanitizer builds) assert on the single-lane violations they can observe.
- Mutable pack spans (`spanPackMut`) are not covered by the assertion; the same rule applies to them.

## Backpressure & Queue Limits
PathSpace has no global flow control: producers are never blocked by `insert`, and the core imposes no size limit. What happens under producer/consumer skew depends on where the queue lives.

- **Node queues are unbounded.** Each node is a FIFO of values and executions (`NodeData`, or the lock-free `PodPayload` lane for POD types). `insert` appends and returns immediately ("Inserts do not block" in "Blocking"); a slow consumer just makes the queue longer. `take` pops from the front; `read` leaves it. Nothing caps the number of pending entries.
- **Consumers wait, producers do not.** A blocking `read`/`take` on an empty node waits on the `WaitMap` until a notification or its timeout (`Timeout`). There is no symmetric wait for "queue has room".
- **Task queue.** Executions are scheduled on a `TaskPool` (`src/pathspace/task/TaskPool.*`): an unbounded `std::queue` of weak task pointers served by a fixed number of workers (`std::thread::hardware_concurrency()` for the default pool, at least one). A burst of `Immediate` executions queues behind busy workers; submission only fails once the pool is shutting down. `Lazy` executions are queued only when a reader asks for the result. Pass your own executor (or a pool of a different size) to a `PathSpace` to isolate heavy producers; a task that blocks inside a worker on a read occupies that worker until it returns.
- **Opt-in bound: `BoundedPathSpace`** (`src/pathspace/layer/BoundedPathSpace.hpp`) wraps a backing space and caps the queue length per path (indexed spellings such as `/q[1]` share the count of `/q`). It never blocks the producer and never fails because the queue is full; instead it drops the oldest entry (drop-oldest, latest wins):
  - When the count has reached `maxItems` (minimum 1) an insert pops the oldest value of the same type and then appends the new one. Only types with both a serializer and a deserializer can be evicted; others are rejected with `NotSupported` once the queue is full.
  - If the front entry has another type the insert is dropped and the failure is reported in `InsertReturn::errors` (the old data stays).
  - If eviction finds the queue empty (someone drained it through the backing space) the count resets and the insert proceeds.
  - Pops through the wrapper (`take`) decrement the count; plain reads do not.
  - Counts live in the wrapper, not in the backing space, so writers that bypass the wrapper make the count stale until it resynchronises as described above.
  Choose it for telemetry/event streams where the newest data matters more than completeness; keep unbounded queues (and apply your own back pressure, for example by waiting on a result path) when every item must be processed.
- **History retention** is bounded separately (`HistoryOptions` / `JournalState::RetentionPolicy`, `maxEntries`/`maxBytes`, `0` means unlimited). It trims undo entries, not live queues.
- **Mitigations in practice:** wrap hot queues in `BoundedPathSpace`; give heavy work its own `TaskPool`; consume with `take` rather than `read` so entries are released; `Error::Code::CapacityExceeded` is reserved for future hard limits and is not returned anywhere today.
- Tests: `tests/unit/layer/test_BoundedPathSpace.cpp`, `tests/unit/layer/test_BoundedPathSpaceCounts.cpp`; task pool behaviour in `tests/unit/task/test_TaskPool.cpp`.

## Unit Testing
Unit testing is by using the C++ doctest library.

## Exception Handling
PathSpace code should not throw C++ exceptions. All failures must travel through explicit status channels (`std::expected`, `Error`, variant/enum results, or structured logs) so callers can react deterministically. Exceptions are only acceptable when wrapping third-party libraries (the Alpaca calls in `type/serialization.hpp` are guarded by try/catch and converted to `Error`) or for container-style programmer errors in `SlidingBuffer` (`at`, `resize` mirror `std::vector`).

- Serialization hooks in `InputMetadata` (`deserialize`, `deserializePop`) return `std::optional<Error>`: `std::nullopt` means the destination was fully written; truncated or corrupt bytes yield `MalformedInput`/`UnserializableType` and leave the destination untouched.

## Fault Tolerance & Errors
Every failure reaches the caller as an `Error` (`src/pathspace/core/Error.hpp`) or an entry in `InsertReturn::errors`; nothing is retried automatically. Callers that want a retry loop around `Block{}` reads or lazy executions have to write it.

### Task exceptions
- A callable inserted as an execution runs on a `TaskPool` worker (`TaskPool::workerFunction`). The worker wraps the call in `try/catch (...)`: an exception marks the task `Failed` (`Task::markFailed`), logs "Exception in running Task" under the `Error`/`Exception` tags (visible only with logging enabled, see "Logging" below), and the worker carries on with the next task. The exception does not propagate and the message is not preserved.
- Typed tasks (`TaskT<T>`) additionally fail their `SharedState<T>` when the callable throws, so waiters on `FutureT`/`FutureAny` wake up. `ready()` stays `false` and `get`/`copy_to` return `false`; there is no way to tell a failure from "not ready yet" except that `wait()` returns. Likewise `Future::wait`, `wait_for` and `copy_result_to` return as soon as the legacy task is terminal (`Completed` or `Failed`) instead of waiting for a result that will never come.
- The notification for the task's path is still delivered after a failure, so blocked readers re-check the node.
- **Known gap (audit item T3 in `docs/State_Machine_Audit.md`).** A failed execution stays at the front of its node. `read`/`take` report the same error as for a task that is still running (`UnknownError`, "Task is not completed"); with `Block{}` they keep retrying until the timeout and return `Timeout`. Values queued behind the failed task are unreachable until the node is cleared. The pending test `a failed execution does not block its node forever` in `tests/unit/task/test_TaskFailureState.cpp` documents the minimum contract any fix has to meet and is skipped until the error contract is chosen.
- Tests: `tests/unit/task/test_TaskFailureState.cpp` (`task.failure.statemachine`), `tests/unit/task/test_FutureCoverage.cpp`, `tests/unit/task/test_IFutureAny.cpp`.

### Submission failures
- Inserting an `Immediate` execution without an executor returns `UnknownError` ("No executor available for immediate task submission"); an executor that refuses the task (for example a `TaskPool` that is shutting down: "Executor shutting down") surfaces its error from the insert. A lazy execution reports the same conditions from the first `read`/`take` that tries to start it ("No executor available for lazy task submission").
- Covered in `tests/unit/core/test_NodeDataCoverage.cpp` (executor refusal, missing executor) and `tests/unit/task/test_TaskPool.cpp` (`addTask` refuses submissions during/after shutdown).

### Read/take error codes
`read`/`take` return `Expected<T>`; the codes below are the ones produced by the core (`Leaf`, `NodeData`, `PathSpace::out`):
- `InvalidPath`: validation failed (`Out::validationLevel`) or an indexed component is malformed.
- `NoSuchPath`: a component of the path does not exist.
- `NoObjectFound`: the node exists but holds no value (also returned for `read<FutureAny>` when no execution future is present).
- `InvalidType`: the front value has a different type than requested; the value is left in place.
- `MalformedInput` / `UnserializableType` / `SerializationFunctionMissing`: payload bytes are truncated or the type has no (de)serializer; the destination is untouched.
- `NotSupported`: for example spans on non-POD nodes, or reading a nested PathSpace instead of taking it.
- `UnknownError`: an execution is not finished yet or failed ("Task is not completed"), or the task expired before its result was copied.
- `Timeout`: a blocking call ran out of time. It is also used for "Shutting down while waiting for data" and "PathSpace clearing in progress", so check the message when the distinction matters.
- `CapacityExceeded` exists in the enum but nothing in `src/` produces it yet; see "Backpressure & Queue Limits".
Non-blocking calls return the first error; blocking calls retry on every notification and return `Timeout` at the deadline, discarding the last underlying error. Tests: `tests/unit/pathspace/test_PathSpaceRead.cpp`, `tests/unit/core/test_NodeDataDeserializeEdges.cpp`.

### Shutdown behaviour
- `PathSpace::shutdown()` (also run by the destructor) invalidates the notification sink, marks the context as shutting down, wakes all waiters, waits for in-flight `in()`/`out()` calls, clears the tree and purges the wait registry. Blocked reads return `Timeout` ("Shutting down while waiting for data..."). A space that borrows another space's context only clears its own tree. See "Blocking" and "Wait/notify protocol".
- Because the tree is cleared first, queued tasks whose node is gone no longer lock and are skipped by the workers. `TaskPool::shutdown()` sets the shutting-down flag; workers keep taking tasks until the queue is empty (a task that is already running always finishes), then exit and are joined (calls are serialised). Completions that finish during shutdown skip their notification. `addTask`/`submit` after shutdown return an `Error`.
- Tests: `tests/unit/pathspace/test_PathSpaceRead.cpp` ("Shutdown waits for a blocking read to exit"), `tests/unit/pathspace/test_PathSpaceClearHandshake.cpp`, `tests/unit/pathspace/test_PathSpaceContextOwnership.cpp`, `tests/unit/task/test_TaskPoolShutdownState.cpp`.

### Logging
`sp_log` is compiled in only when `SP_LOG_DEBUG` is defined and is off at run time unless `PATHSPACE_LOG=1` (or `PATHSPACE_LOG_ENABLED=1`). Output is tag-filtered: a built-in skip list silences the noisy internal tags (for example `INFO`, `TaskPool`, `NodeData`; the `Error`/`Exception` tags used for task exceptions are not in it), `PATHSPACE_LOG_ENABLE_TAGS=a,b` restricts output to messages whose tags are all in the allowlist, and `PATHSPACE_LOG_SKIP_TAGS` / `PATHSPACE_LOG_CLEAR_DEFAULT_SKIPS` adjust the skip list. Tags can also be toggled at run time (`TaggedLogger::enableTag`/`disableTag`/`skipTag`, or `SP::enable_log_tag`). Tests: `tests/unit/log/test_TaggedLogger.cpp`.

## Const-correctness notes
A few `const_cast`s are deliberate; each has one documented home:
- `read`/`peek` style calls are `const` but go through the virtual `out`, which may start a lazy task or register a waiter. They are logically const (they never change the visible data), which is why `PathSpaceBase::read` casts `this` to call `out`.
- `spanPackConst`/`spanPackMut` are `const` in the `PathSpaceBase` interface; `Leaf::mutableRoot()` is the single place that casts the root away because the lookup helpers take a non-const `Node&`.
- `InputData` stores the caller's object as `void*` (type erasure for the shared insert/read signature). Insert paths must treat it as read-only. `BoundedPathSpace::in` is the one exception: while evicting old entries it pops into the caller's object and restores it from a serialized copy, so it only evicts for types that have both `serialize` and `deserialize` hooks and otherwise rejects the insert with `NotSupported` (covered by a test).
- `BoundedPathSpace` keeps one count per queue (`/q` and `/q[1]` share it). When an eviction finds the queue empty the queue was drained through the backing space: the count is reset and the insert proceeds. When an eviction fails for another reason (the queue is full of a different type) the insert is dropped and the failure is reported in `InsertReturn::errors`.
- `PathSpaceBase::getNotificationSink` is non-const: it lazily creates the sink that refers back to the owning space.

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

### View API, invariants and examples
**`PathView`** (`src/pathspace/layer/PathView.hpp`)
- Constructed as `PathView(backingSpace, permission, root = "")`; `permission` is `std::function<Permission(Iterator const&)>` returning `{read, write, execute}`.
- Invariants: the callback sees the path as the caller spelled it (before `root` is prepended). `in` needs `write`, `out` (read/take/peek) needs `read`; a denied call returns `InvalidPermissions` (inserts report it in `InsertReturn::errors`). `execute` is part of the struct but the view does not check it. A non-empty `root` is prepended to every forwarded path. `visit` maps the view's root to the backing root, drops entries outside `root`, and skips subtrees the callback denies for reading.
- Example (`tests/unit/layer/test_PathView.cpp`, suite `layer.path_view`):
  ```cpp
  auto backing = std::make_shared<PathSpace>();
  PathView view(backing, [](Iterator const& p) {
      bool ok = p.toStringView().find("/deny") == std::string_view::npos;
      return Permission{.read = ok, .write = ok, .execute = true};
  }, "/root");
  view.insert("/allow/value", 42);   // stored at /root/allow/value in `backing`
  view.insert("/deny/value", 7);     // InvalidPermissions
  ```
- Further coverage: `tests/unit/layer/test_PathViewCoverage.cpp` (missing backing space, root filtering, visit filtering, join/strip normalization).

**`PathAlias`** (`src/pathspace/layer/PathAlias.hpp`)
- Constructed as `PathAlias(upstream, targetPrefix)`; `setTargetPrefix` normalizes the prefix (leading `/`, no trailing `/`) and retargets atomically, then notifies the mount prefix (or `notifyAll` when the mount is unknown) so blocked readers re-check.
- Invariants: `in`/`out` accept concrete paths only (glob paths, also ones introduced by the prefix, are rejected); with no upstream the alias returns errors and `notify` is a no-op; the alias does not mirror upstream notifications by itself.
- Example (`tests/unit/layer/test_PathAlias.cpp`, suite `layer.path_alias`):
  ```cpp
  auto upstream = std::make_shared<PathSpace>();
  PathAlias alias{upstream, "/dev"};
  alias.insert<"/mouse/0/name", std::string>("mouse0");   // upstream: /dev/mouse/0/name
  alias.setTargetPrefix("/dev2");                          // later operations go to /dev2/...
  ```
  Mounting the alias inside a parent (`parent.insert<"/alias">(std::move(aliasPtr))`) makes `/alias/a` resolve to `<targetPrefix>/a` upstream.
- Further coverage: `tests/unit/layer/test_PathAliasCoverage.cpp`.

### Default Paths
There is no implicit default root or built-in node tree in the core: a fresh `PathSpace` is empty and every path is created on first insert. The reserved names that layers define themselves (`_history/*` under history-enabled roots, `/_system/*` on a trellis, the `/diagnostics/history/...` mirror) are listed in `docs/AI_Paths.md` and defined in `src/pathspace/history/UndoHistoryUtils.hpp` and `src/pathspace/layer/PathSpaceTrellis.cpp`; names starting with `_` are reserved for such control and telemetry nodes. Anything else (for example `/system/applications/<app>`) is a caller convention. Defaults that do exist are in the options structs: `Out{}` is a non-blocking read/pop-free call with default validation, `Block{}` waits (effectively) forever until you give it a timeout, and `In{}` executes lambdas immediately unless `executionCategory = Lazy`. Mounting a view or alias under a parent space (`parent.insert<"/alias">(std::move(layer))`) adopts the parent's context and prefix, so notifications and waits keep working across the mount (`tests/unit/layer/test_PathAlias.cpp`, "Nested mounting under a parent PathSpace"; `tests/unit/layer/test_BoundedPathSpace.cpp`, "works mounted under /devices/test").

### Shutdown and Test Hooks

- `PathSpace::shutdown()` is the public API to cooperatively wake waiters and clear paths during teardown.
- `TaskPool::shutdown()` may be called concurrently; calls are serialised and the workers are joined once.
- Test hooks: `testing::SetPathSpaceAdmissionHook` runs right after an `in()`/`out()` was admitted past the clear gate; `testing::SetWaitMapClearDrainedHook` runs inside `WaitMap::clear()` between the drain and the destruction of the registry.
- `PathSpace::shutdownPublic()` and `PathSpace::notifyAll()` are protected test utilities. When tests need to call them, expose via a small test-only subclass, for example:
  - `struct TestablePathSpace : SP::PathSpace { using SP::PathSpace::PathSpace; using SP::PathSpace::shutdownPublic; };`
- `PathSpace::peekFuture(...)` and `PathSpace::setOwnedPool(...)` are protected implementation details. Prefer the unified `read<FutureAny>(path)` and constructor injection of the executor/pool.
- Nested spaces adopt shared context and a mount prefix internally via the protected `adoptContextAndPrefix(...)`; external callers should not invoke this directly.

Note: First-class links (symlinks) are planned; in the interim, `PathAlias` offers robust forwarding/retargeting semantics without changing core trie invariants.