# PathSpace — AI-facing Architecture Overview

This document is written for an AI (or new engineer) that needs to understand the structure, responsibilities and relationships of components in the `PathSpace` project. It maps the main subsystems to file locations, describes typical data flows (insert / read / take), and summarizes concurrency and extension points.

---

## High-level summary

PathSpace is an in-memory, path-keyed data & task routing system. It exposes a path-based API to insert data or tasks at glob-like paths and to read or extract (pop) values from those paths. The core design separates:
- a `PathSpaceBase` interface (access surface),
- a concrete `PathSpace` implementation that manages nodes and wait-notify semantics,
- a `Leaf`/`NodeData` layer that stores serialized data or tasks,
- a `Task` / `TaskPool` subsystem that schedules and executes deferred computations,
- thin adapter layers (`PathView`, `PathFileSystem`) that present alternative views or persistence-like behaviors.

---

## Top-level layout (important files & directories)

- `PathSpace/src/pathspace/` — main public headers and entrypoints:
  - `PathSpace.hpp` — `PathSpace` class declaration.
  - `PathSpace.cpp` — `PathSpace` implementation (lifecycle, in/out, notify, shutdown).
  - `PathSpaceBase.hpp` — abstract API used by clients (`insert`, `read`, `take` templates).

- `PathSpace/src/pathspace/core/` — core runtime data structures:
  - `Leaf.hpp` / `Leaf.cpp` — manages node traversal and delegates to `NodeData`.
  - `NodeData.hpp` / `NodeData.cpp` — primary per-node storage: serialized data or queued `Task`s; (de)serialization and type bookkeeping.
  - `WaitMap.hpp` / `WaitMap.cpp` — condition-variable map used to block/wake readers waiting for data.
  - `NotificationSink.hpp` — lifetime-safe notification interface; tasks hold a weak_ptr token, and spaces own the shared token.
  - `Error.hpp`, `In.hpp`, `Out.hpp`, `InsertReturn.hpp`, `ElementType.hpp`, etc. — small core types used by the core logic.

- `PathSpace/src/pathspace/task/` — task execution:
  - `Task.hpp` — `Task` abstraction (callable, result storage, states).
  - `TaskPool.hpp` / `TaskPool.cpp` — thread pool for executing `Task`s.
  - `TaskStateAtomic.hpp` — atomic task state machine.

- `PathSpace/src/pathspace/path/` — path utilities:
  - `Iterator.hpp` / `Iterator.cpp` — iterator for path components and helper operations (`isAtFinalComponent`, `startToCurrent`, validation).
  - `utils.hpp` / `validation.hpp` — path matching and validation helpers.
  - `TransparentString.hpp` — hash/equality helpers for string_view keyed maps.

- `PathSpace/src/pathspace/layer/` — adapters and views:
  - `PathView.hpp` / `PathView.cpp` — permissioned view that forwards to a `PathSpaceBase`.
  - `PathFileSystem.hpp` / `PathFileSystem.cpp` — adapter that maps path operations into a filesystem-like root.

- `PathSpace/src/pathspace/log/`
  - `TaggedLogger.hpp` / `.cpp` — centralized logging helpers used across components.

- `PathSpace/src/pathspace/type/` — supporting type utilities:
  - `InputData.hpp`, `InputMetadata.hpp`, `InputMetadataT.hpp` — how runtime/compile-time type metadata and serialization hooks are expressed.
  - `NodeDataHashMap.hpp` — container used by `Leaf` to map path keys to `NodeData`.
  - `SlidingBuffer.hpp` — byte buffer used to store serialized object payloads.

---

## Key classes and relationships

- `PathSpaceBase` (`PathSpace/src/pathspace/PathSpaceBase.hpp`)
  - Pure-virtual methods: `in`, `out`, `shutdown`, `notify`.
  - Provides public template wrappers: `insert`, `read`, `take`. These templates build `InputData` and `InputMetadata` and call the protected virtuals.

- `PathSpace` (`PathSpace/src/pathspace/PathSpace.hpp` / `.cpp`)
  - Concrete implementation of `PathSpaceBase`.
  - Holds:
    - `Leaf leaf` — manages node-level storage/lookup.
    - `WaitMap waitMap` — used to block readers until data appears.
    - optional `TaskPool* pool` — used by tasks inserted in nodes.
  - Responsibilities:
    - Coordinate insert (`in`) and read/extract (`out`) semantics.
    - Notify root waiters on successful insertions.
    - Manage shutdown and clearing of state.

- `Leaf` (`PathSpace/src/pathspace/core/Leaf.hpp` / `.cpp`)
  - Walks a path (`Iterator`) to the final node.
  - Creates/looks up `NodeData` in `NodeDataHashMap`.
  - Calls into `NodeData::serialize` to store incoming `InputData`, or `NodeData::deserialize` to read/extract.

- `NodeData` (`PathSpace/src/pathspace/core/NodeData.hpp` / `.cpp`)
  - Stores either serialized data (in `SlidingBuffer`) or queued `Task`s.
  - Keeps a `types` deque (`ElementType`) to track type/quantity sequence at that node.
  - Handles serialize/deserialize/pop semantics and validation against requested `InputMetadata`.
  - When a `Task` is inserted and is Immediate, `NodeData` asks `TaskPool` to execute it.

- `Task` / `TaskPool` (`PathSpace/src/pathspace/task/`)
  - `Task` encapsulates a callable, result storage (`std::any`), and a small adaptor to copy the result into user-provided output buffers (`resultCopy_`).
  - `TaskPool` maintains worker threads and processes queued tasks, tracking active workers/tasks and returning `Error` on overload/shutdown.
  - `NotificationSink` (`PathSpace/src/pathspace/core/NotificationSink.hpp`) provides lifetime-safe notifications. `Task` holds a `weak_ptr<NotificationSink>`, and `PathSpaceBase` owns the token and forwards notify calls.

- `WaitMap` (`PathSpace/src/pathspace/core/WaitMap.hpp`)
  - Maintains a mapping from path string -> `std::condition_variable`.
  - Provides `Guard` objects that encapsulate a locked `mutex` + condition variable to wait until notified.
  - Used by `PathSpace::out` to implement blocking reads with deadlines and timeouts.

- `PathView` / `PathFileSystem`
  - Both subclass `PathSpaceBase` and forward to another `PathSpaceBase` while applying permission checks or path prefixing.

---

## Typical data flow (insert)

1. Client calls `insert(path, data, inOptions)` (template on `PathSpaceBase`).
2. `PathSpaceBase::insert`:
   - Builds an `Iterator` and validates the path.
   - Wraps `data` into `InputData` (or creates a `Task` for execution category).
   - Calls the virtual `in(pathIterator, InputData)`.
3. `PathSpace::in`:
   - Detects special cases (e.g., inserting a nested `PathSpace`).
   - Calls `leaf.in(iter, inputData, ret)` to perform node operations.
   - If nodes/values/tasks were added, calls `waitMap.notify(path)` to wake waiting readers.
4. `Leaf::in`:
   - Traverses to final node and calls `NodeData::serialize(inputData)` to append or queue data.

---

## Typical data flow (read / take)

1. Client calls `read<DataType>(path, outOptions)` or `take<DataType>`.
2. `PathSpaceBase` validates path and creates destination object.
3. Calls `out(path, InputMetadataT<DataType>{}, options, &obj)` virtual.
4. `PathSpace::out`:
   - Calls `leaf.out`.
   - If data isn't immediately available and options request blocking, uses `waitMap` to wait until `notify` wakes relevant waiters or until timeout.
5. `Leaf::out`:
   - Finds node's `NodeData` and invokes `deserialize` / `deserializePop`.
6. `NodeData::deserialize*`:
   - Validates type and either:
     - If stored as serialized bytes, runs `InputMetadata::deserialize` (or `deserializePop`) into `obj`.
     - If stored as `Task`, ensures `Task` is scheduled, waits or returns error if not completed, and copies task result via `resultCopy`.

---

## Concurrency & thread-safety notes

- `WaitMap` internally synchronizes access to the condition variable map via `mutex`.
- `TaskPool` uses worker threads and `std::condition_variable` to schedule tasks.
- Tasks notify via a `weak_ptr<NotificationSink>`; `PathSpaceBase` resets its `shared_ptr` during shutdown so late notifications are dropped safely.
- `NodeData` may contain `Task` objects (shared pointers) and a `SlidingBuffer`. Serialization/deserialization must be used carefully if adding new methods—`NodeData` methods assume proper locking at call sites (the `Leaf`/`NodeDataHashMap` manage access).
- `PathSpace::out` implements a loop with `waitMap.wait` and `wait_until` to implement blocking with deadlines. The pattern minimizes lock scope.

---

## Extending the system

- Add new serializable types:
  - Provide an `InputMetadataT<T>` instantiation (`InputMetadataT.hpp`) that sets `serialize`, `deserialize`, `deserializePop` function pointers and `typeInfo`.
  - Ensure `InputMetadata` carries `typeInfo` and `dataCategory` as expected.
- Add new view/adapter:
  - Subclass `PathSpaceBase` and implement `in`, `out`, `shutdown`, `notify`.
  - Use `PathView` as a reference for permissioned forwarding.
- Add new task behavior:
  - Modify `Task` to support additional execution categories and use `TaskPool::addTask` to schedule.

---

## Where to look for specific behavior

- Blocking read semantics & timeouts: `PathSpace::out` (`PathSpace/src/pathspace/PathSpace.cpp`).
- Node storage & type bookkeeping: `NodeData` (`PathSpace/src/pathspace/core/NodeData.*`).
- Path parsing and validation: `Iterator` and `validation.hpp` (`PathSpace/src/pathspace/path/`).
- Task lifecycle and thread pool: `Task.hpp` and `TaskPool.hpp` (`PathSpace/src/pathspace/task/`).
- Notification sink and lifetime: `NotificationSink.hpp` (`PathSpace/src/pathspace/core/NotificationSink.hpp`).
- Wait-notify primitives: `WaitMap.hpp` (`PathSpace/src/pathspace/core/WaitMap.hpp`).
- Logging utilities: `TaggedLogger.hpp` (`PathSpace/src/pathspace/log/TaggedLogger.hpp`).

---

## Useful grep patterns for programmatic discovery

- Find data-flow entrypoints: search for `PathSpace::in` / `PathSpace::out` or `PathSpaceBase::insert`.
- Find node-level code: grep for `NodeData::` and for `Leaf::`.
- Find task flow: grep for `Task::Create`, `TaskPool::addTask`, `NotificationSink`, and `Task::resultCopy`.

---

## Tests & build

- Tests live under `PathSpace/tests`.
- Build is configured by `CMakeLists.txt` at project root and `PathSpace/src/pathspace/CMakeLists.txt`.
- Helper script: `scripts/compile.sh`
  - Default: incremental build into `./build`
  - Full rebuild: `--clean` removes the build directory and reconfigures
  - Build type: `--debug` (default) or `--release` or `--build-type TYPE`
  - Jobs: `-j N` or `--jobs N` for parallel builds
  - Generator: `-G "Ninja"` or `--generator NAME`
  - Target: `-t NAME` or `--target NAME` to build a specific target (e.g., `PathSpaceTests`)
  - Sanitizers: `--asan`, `--tsan`, `--usan` map to this repo’s CMake options
  - Verbose: `-v` or `--verbose` prints underlying commands
  - Test run: `--test` builds and runs tests (executes `build/tests/PathSpaceTests`)
  - Examples:
    - `./scripts/compile.sh`
    - `./scripts/compile.sh --clean -j 8 --release`
    - `./scripts/compile.sh -G "Ninja" --target PathSpaceTests`
    - `./scripts/compile.sh --test`

---

## Notes for an AI editor

- Prefer using `grep` over guessing file paths. The codebase uses consistent naming and directory structure under `PathSpace/src/pathspace/`.
- When making changes that affect serialization API, update `InputMetadataT` specializations and ensure `deserializePop` exists for `take`-style operations.
- Make small, localized edits when possible; adding extensive concurrency changes requires careful reasoning about `WaitMap`, `Leaf`, and `NodeData` interactions.

---

## Detailed call sequences

Below are explicit call sequences (ordered function calls and decisions) including file references to help an AI or engineer trace behavior through the code.

A. Insert (simple serialized data)
1. `Client` calls:
   - `PathSpaceBase::insert(path, data, options)` — `PathSpace/src/pathspace/PathSpaceBase.hpp`
2. `insert` does:
   - `Iterator const path{pathIn}` — `PathSpace/src/pathspace/path/Iterator.hpp/.cpp`
   - path validation (`path.validate(...)`) — `PathSpace/src/pathspace/path/validation.hpp`
   - `InputData inputData{std::forward<DataType>(data)}`
   - calls `this->in(path, inputData)` (virtual)
3. Virtual dispatch to `PathSpace::in` — `PathSpace/src/pathspace/PathSpace.cpp`
   - detects special-case (e.g., nested `PathSpace`) if the inserted type is `unique_ptr<PathSpace>`
   - calls `leaf.in(path, inputData, ret)` — `PathSpace/src/pathspace/core/Leaf.hpp/.cpp`
4. `Leaf::in`:
   - traverses components using `Iterator` to locate/create a node entry in `NodeDataHashMap` — `PathSpace/src/pathspace/type/NodeDataHashMap.hpp`
   - calls `NodeData::serialize(inputData)` — `PathSpace/src/pathspace/core/NodeData.cpp`
5. `NodeData::serialize`:
   - if `inputData.task` is present, pushes the `Task` into `tasks` and possibly schedules immediate execution via `TaskPool::Instance().addTask(...)` — `PathSpace/src/pathspace/task/TaskPool.hpp/.cpp`
   - otherwise, calls `inputData.metadata.serialize(inputData.obj, data)` to append bytes to `SlidingBuffer` — `PathSpace/src/pathspace/type/SlidingBuffer.hpp/.cpp`
   - updates `types` deque via `pushType(...)`
6. Return values bubble up (`InsertReturn`) and `PathSpace::in` calls `waitMap.notify(path.toStringView())` if anything new was inserted — `PathSpace/src/pathspace/core/WaitMap.hpp`

B. Insert (task-based; lazy vs immediate)
1. `PathSpaceBase::insert` builds `InputData` that includes a `Task` when `InputMetadataT<DataType>::dataCategory == DataCategory::Execution`.
2. `PathSpace::in` -> `Leaf::in` -> `NodeData::serialize`:
   - if task `category == ExecutionCategory::Immediate`, `NodeData::serialize` attempts to schedule it immediately via `TaskPool::Instance().addTask(...)`
   - if `Lazy`, it is stored; when a consumer later calls `read`/`take`, the `NodeData::deserializeExecution` logic will call `TaskPool::Instance().addTask(...)` before trying to read the result.

C. Read (non-blocking)
1. `PathSpaceBase::read<DataType>(path, options)` — `PathSpace/src/pathspace/PathSpaceBase.hpp`
2. Validates path, constructs `DataType obj`, calls `out(path, InputMetadataT<DataType>{}, options, &obj)` (virtual)
3. `PathSpace::out`:
   - calls `leaf.out(path, inputMetadata, &obj, options.doPop)` — `PathSpace/src/pathspace/core/Leaf.hpp/.cpp`
   - `Leaf::out` locates `NodeData` and calls `NodeData::deserialize(obj, inputMetadata)` or `deserializePop` if `doPop==true`
   - `NodeData::deserialize*` validates types and either calls provided `deserialize` functions or handles tasks (waits for completion or returns error)

D. Read (blocking with timeout)
1. `PathSpace::out` first attempts `leaf.out(...)` once without blocking.
2. If no data and `options.doBlock==true`, compute `deadline = now + options.timeout`.
3. Loop:
   - create guard via `auto guard = waitMap.wait(path.toStringView())` — `PathSpace/src/pathspace/core/WaitMap.hpp`
   - call `guard.wait_until(deadline, predicate)` where predicate re-invokes `leaf.out(...)` to check for availability.
   - if predicate returns true (data available), return result; otherwise, if deadline reached return `Error::Timeout`.
4. `waitMap.notify(...)` (called from `PathSpace::in`) wakes waiting threads for the notified path.

E. Take (pop semantics)
1. `PathSpaceBase::take` calls `out` with `options & Pop{}` so `doPop==true`.
2. `PathSpace::out` and `Leaf::out` route to `NodeData::deserializePop` which:
   - for serialized data calls `inputMetadata.deserializePop(obj, data)` (which must be provided) and then `popType()` to decrement counts and adjust `types` deque.
   - for tasks, if `doPop` and task completed, `tasks.pop_front()` is performed and `popType()` is invoked.

---

## Component diagrams (ASCII)

High-level component relationship and data flow (simple view):

Client
  |
  v
PathSpaceBase (templates: insert/read/take)  --> PathView / PathFileSystem (subclasses)
  |
  v
PathSpace (root implementation)
  |-- leaf : Leaf
  |       |-- nodeDataMap : NodeDataHashMap
  |       |       \-- NodeData
  |       |            |-- SlidingBuffer (serialized bytes)
  |       |            \-- tasks (deque<shared_ptr<Task>>)
  |       \-- Iterator (path parsing/traversal)
  |
  |-- waitMap : WaitMap (condition variables, Guard)
  |
  \-- pool : TaskPool (threads, task queue)
          \-- Task (callable, result std::any, state)

Textual flow example for a blocking read:
Client -> PathSpaceBase::read -> PathSpace::out -> Leaf::out -> NodeData::deserialize
If missing -> PathSpace::out -> waitMap.wait (Guard) -> Wait for waitMap.notify -> retry Leaf::out

---

## Example sequence diagrams (compact)

Insert (data)
Client -> PathSpaceBase::insert(path, data)
PathSpaceBase::insert -> PathSpace::in
PathSpace::in -> Leaf::in
Leaf::in -> NodeData::serialize
NodeData::serialize -> SlidingBuffer.append OR TaskPool.addTask
PathSpace::in -> WaitMap.notify(path)

Read (blocking)
Client -> PathSpaceBase::read(path)
PathSpaceBase::read -> PathSpace::out
PathSpace::out -> Leaf::out
Leaf::out -> NodeData::deserialize (returns no-data)
PathSpace::out -> WaitMap.wait(path) [Guard]
(another thread) PathSpace::in -> WaitMap.notify(path)
Guard wakes -> PathSpace::out retries Leaf::out -> NodeData::deserialize (success) -> return obj to client

---

## Practical tips for editing & automated modifications

- If you add or change serialization hooks:
  - Update `InputMetadataT<T>` instantiations and ensure both `deserialize` and `deserializePop` are present for types expected to be taken.
  - Update tests under `PathSpace/tests` that rely on `take` semantics.
- If you change `NodeData` internal layout (e.g., change `types` bookkeeping), audit `popType`/`pushType` and any code that relies on `NodeData::empty`.
- If you change wait/notify semantics:
  - Update `WaitMap`'s `getCv`/`notify` logic and ensure `PathSpace::out` still minimizes lock scope in its blocking loop.
- For adding new views, follow `PathView`/`PathFileSystem` patterns: forward to an inner `PathSpaceBase` while mapping the path or applying permission checks.

---

If you want, I can:
- Produce sequence diagrams annotated with exact line ranges for each function (e.g. `PathSpace/src/pathspace/PathSpace.cpp#L1-200`) to make automatic tracing easier.
- Convert the ASCII diagrams into a mermaid diagram and add a small rendering hint.
- Generate a quick grep/ctags command list to jump to the functions mentioned.

Which of those would you like next?