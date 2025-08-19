# PathSpace — AI-facing Architecture Overview

## Pre-push checks (local + CI)

This repository uses both a local pre-push hook (optional) and a CI status check to ensure stability:
- Local: block pushes that don’t pass a clean recompile and 15 looped test runs.
- CI: GitHub Actions workflow runs the same loop=15 test job; protect master so pushes/merges require this check to pass.

### Local pre-push hook (optional)

Install a pre-push hook in your clone to gate pushes:

1) Create the hook file:
```/dev/null/pre-push.sh#L1-100
#!/usr/bin/env bash
set -euo pipefail

# Allow explicit bypass if needed:
if [[ "${SKIP_LOOP_TESTS:-}" == "1" ]]; then
  echo "[pre-push] SKIP_LOOP_TESTS=1 set; bypassing loop tests."
  exit 0
fi

# Ensure compile script is executable
chmod +x ./scripts/compile.sh

# Clean rebuild and looped tests (15 iterations, release)
echo "[pre-push] Building and running tests (loop=15)..."
./scripts/compile.sh --clean --test --loop=15 --release --jobs "$(command -v nproc >/dev/null 2>&1 && nproc || sysctl -n hw.ncpu)" --verbose

echo "[pre-push] OK"
```

2) Install it into your local repo:
- Save the script above to .git/hooks/pre-push
- Make it executable: chmod +x .git/hooks/pre-push

3) Usage:
- Push as normal: the hook will block the push if the loop tests fail.
- To bypass for an emergency: SKIP_LOOP_TESTS=1 git push

### CI status check and branch protection

- Workflow: .github/workflows/ci.yml defines a job named “Build and Test (loop=15)” (job id: loop-tests).
- Enable branch protection for master:
  - Settings → Branches → Add rule → Require status checks to pass
  - Select: CI / loop-tests
  - Optionally require pull request before merging to master.

This document is written for an AI (or new engineer) that needs to understand the structure, responsibilities and relationships of components in the `PathSpace` project. It maps the main subsystems to file locations, describes typical data flows (insert / read / take), and summarizes concurrency and extension points.

New layers and IO building blocks:
- `src/pathspace/layer/PathAlias.hpp` — path aliasing/forwarding layer
  - Forwards `in`/`out`/`notify` to an upstream `PathSpaceBase` with a configurable `targetPrefix`.
  - Supports atomic retargeting and plays well with nested mounts by using the iterator tail (current->end).
- `src/pathspace/layer/PathIOMouse.hpp` and `src/pathspace/layer/PathIOKeyboard.hpp` — device IO providers (path-agnostic)
  - Provide thread-safe simulated event queues and typed `out()`/`take()` for `MouseEvent`/`KeyboardEvent`.
  - Respect `Out.doPop` (peek vs pop) and `Out.doBlock` timeouts.
  - When mounted with a shared context, `simulateEvent()` wakes blocking readers.
- `src/pathspace/layer/PathIOStdOut.hpp` — simple sink that prints inserted `std::string` to stdout (optional prefix/newline).
- `src/pathspace/layer/PathIODeviceDiscovery.hpp` — simulation-backed `/dev`-like discovery
  - Lists classes, device IDs, per-device `meta` and `capabilities`.
  - Mount anywhere (e.g., `/dev`) and it works via iterator tail mapping.

Platform backends (unified providers):
- `src/pathspace/layer/PathIOMouse.hpp` and `src/pathspace/layer/PathIOKeyboard.hpp` include start()/stop() hooks and select platform code paths via compile-time macros (e.g., `PATHIO_BACKEND_MACOS`). When enabled, a worker thread sources OS events and feeds their `simulateEvent(...)` APIs.
- Build flag:
  - Add `-DENABLE_PATHIO_MACOS=ON` to CMake on macOS to define `PATHIO_BACKEND_MACOS` and enable macOS backend paths in the unified providers (CI defaults to simulation/no-op).
- Deprecated:
  - `src/pathspace/layer/macos/PathIO_macos.hpp` is a compatibility shim only and no longer exposes `PathIOMiceMacOS` or `PathIOKeyboardMacOS`. Include the unified headers instead.

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
  - `Node.hpp` — unified trie node (children, payload `NodeData`, optional nested space).
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
  - Navigates the Node trie (`PathSpace/src/pathspace/core/Node.hpp`) and looks up or creates `Node::data` for payload at the final component.
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

- `WatchRegistry` backs wait/notify for concrete (non-glob) paths using a trie of path components (O(path depth)) and a single mutex; `notifyAll()` wakes all waiters. Glob subscriptions are not handled by `WatchRegistry` in this version; glob matching is performed at higher layers (e.g., leaf/path matching), while notifications are issued for concrete paths.
- `TaskPool` uses worker threads and `std::condition_variable` to schedule tasks.
- Tasks notify via a `weak_ptr<NotificationSink>`; `PathSpaceBase` resets its `shared_ptr` during shutdown so late notifications are dropped safely.
- `NodeData` may contain `Task` objects (shared pointers) and a `SlidingBuffer`. Serialization/deserialization must be used carefully if adding new methods—`NodeData` methods assume proper locking at call sites (the Node trie’s concurrent children map and per-node payload mutexes manage access).
- `PathSpace::out` implements a deadline-driven wait loop using `PathSpaceContext::wait(...)` (backed by `WatchRegistry`) and `wait_until`. The pattern minimizes lock scope around the registry and honors the timeout specified in `Out` options.

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
  - Loop: `--loop[=N]` runs the selected tests N times (default: 15). Implies `--test`.
    - Recommended for changes that touch concurrency (WaitMap, Node trie, Task/Executor), since intermittent races may only appear across repeated runs.
  - Compilation database (`compile_commands.json`):
    - After each successful configure/build, CMake target `copy-compile-commands` mirrors `build/compile_commands.json` to `./compile_commands.json` at the repo root.
    - If you add/rename/remove source files, re-run configure or the compile script to refresh it; many editors/LSPs depend on it for include paths and diagnostics.
  Examples:
    - `./scripts/compile.sh`
    - `./scripts/compile.sh --clean -j 8 --release`
    - `./scripts/compile.sh -G "Ninja" --target PathSpaceTests`
    - `./scripts/update_compile_commands.sh --no-configure`

  Test env toggles:
    - `PATHSPACE_LOG=1` — enable verbose test logging when compiled with `SP_LOG_DEBUG` (set to `0` to force off)
    - `PATHSPACE_FAST_TESTS=1` — speed up heavy tests (reduced durations/iterations) without changing their utility
  Timeout policy:
    - Tests must never be run without a timeout wrapper.
    - Use `scripts/run_testcase.sh` (and `scripts/compile.sh --test` / `--loop`) which enforce a 60s timeout with a manual fallback when `timeout` is unavailable.
    - Do not bypass these wrappers in CI or local loops. If you invoke the binary directly, wrap it with `timeout 60s ./build/tests/PathSpaceTests` (or the platform equivalent).
    - Keep timeouts strict: do not raise or disable them to hide hangs. Typical runs complete in 1–2 seconds; significantly longer runs likely indicate a hang.

### Example application (optional)
- A small example app demonstrates user-level composition (mounting providers under arbitrary paths) and prints device activity:
  - File: `examples/devices_example.cpp`
  - It is not hardwired to any path conventions; it simply mounts providers at the paths you choose in the example.
- Build (disabled by default):
  - Enable with CMake option: `-DBUILD_PATHSPACE_EXAMPLES=ON`
  - macOS backend paths (optional, via unified providers): `-DENABLE_PATHIO_MACOS=ON`
  - Commands:
    - `cmake -S . -B build -DBUILD_PATHSPACE_EXAMPLES=ON`
    - `cmake --build build -j`
- Run:
  - `./build/devices_example`
  - Press Ctrl-C to exit.
- Behavior:
  - No simulation: the example listens for real input events from mounted providers.
  - On macOS with `-DENABLE_PATHIO_MACOS=ON`, the unified providers start platform backend threads and can source OS events via hooks when implemented; otherwise they remain simulation/no-op.
  - It logs mouse moves/clicks/wheel from a `PathIOMouse` provider and keyboard keydown/keyup/text from a `PathIOKeyboard` provider.
  - Plug/unplug messages are inferred using an inactivity heuristic (first event marks "plug-in"; prolonged idle marks "unplug") since there is no OS-level hotplug API wired yet.

### Local pre-push hook (recommended)
Note: Ask before committing and pushing changes. You do not need to ask to run tests. After pushing your topic branch, run ./scripts/create_pr.sh to automatically create the PR (via gh or GH_TOKEN) and open it in your browser. When creating the PR, follow the PR title/body rules for AI: do not use Conventional Commit prefixes in the PR title (e.g., no “chore(...):”), start the title with a capital letter, and use the minimal template with a “Purpose” section and an “AI Change Log”. See .github/PULL_REQUEST_TEMPLATE.md.
Run the full looped test suite and a brief local smoke test of the example before pushing (so you don’t rely on CI to catch issues):

Hard rules (must follow; no exceptions):
- Never push if looped tests are failing locally. If a single iteration fails in a loop run, stop, debug, and fix before pushing.
- Never modify tests to hide or “deflake” failures without prior approval from the maintainer. Fix the underlying product issue or propose the exact minimal test change for review first.
- If a failure happens intermittently, raise the loop count (e.g., 30+) until stable. Attach logs and a short analysis to the PR if you suspect flakiness.
- Use bounded waits and clear cancellation/teardown semantics in product code; avoid indefinite waits that depend on external signals when possible.

1) Install the hook in your clone:
- ln -sf ../../scripts/git-hooks/pre-push .git/hooks/pre-push
- chmod +x scripts/git-hooks/pre-push .git/hooks/pre-push

2) What it does by default:
- Builds and runs the test suite with a loop (loop=15)
- Builds the example app (devices_example)
- Runs the example locally for ~3 seconds to ensure it starts cleanly

3) Useful environment toggles:
(Do not use these to bypass failures; they are for targeted local runs only. For any push to a shared branch, the looped tests must pass locally.)
- SKIP_LOOP_TESTS=1 — skip the looped tests (e.g., for quick local pushes)
- SKIP_EXAMPLE=1 — skip the example smoke test
- BUILD_TYPE=Release|Debug — choose build type (default: Release)
- JOBS=N — parallel build jobs (defaults to system CPU count)
- PATHSPACE_CMAKE_ARGS="..." — pass extra CMake args
- ENABLE_PATHIO_MACOS=ON — on macOS, enable macOS backend paths in the unified providers

Debugging guidance:
- Add temporary logs at key coordination points (e.g., on wait registration, notify, shutdown start/finish) with stable tags (PathSpace, PathSpaceShutdown, Wait/Notify).
- Prefer INFO-level logs guarded by existing logging filters to keep noise low; remove or gate temporary logs before merge.
- When investigating concurrency issues, print thread IDs and counters at entry/exit of loops and before/after waits.
- Use bounded timeouts in product code where practicable; on shutdown, always notifyAll() and exit loops promptly.

Examples:
- SKIP_EXAMPLE=1 ./scripts/compile.sh --test
- SKIP_LOOP_TESTS=1 BUILD_TYPE=Debug JOBS=8 git push

---

## Commit message guidelines

Use a clear, conventional format so history is scannable and tooling-friendly. Conventional Commits style works well for C++ projects with repo-specific scopes.

PR Title & Body (AI authors):
- PR titles must NOT include Conventional Commit prefixes (e.g., “chore(...):”, “fix:”, etc.).
- Start PR titles with a capital letter and write them as a short, human-friendly subject.
- PR bodies should use the minimal template: a “Purpose” section (1–3 sentences) and an “AI Change Log” listing file-by-file edits.
- See .github/PULL_REQUEST_TEMPLATE.md for the exact structure.

Format (80-char max per line):
type(scope): imperative subject

Rules:
- type: one of
  - feat, fix, perf, refactor, docs, test, chore, build, ci, revert, style
- scope: a concise area, e.g., core, path, layer, task, type, tests, docs, build, scripts
- subject:
  - imperative mood, present tense (e.g., add, fix, improve)
  - concise; subject line must be ≤ 80 characters
- body:
  - explain what and why (not just how)
  - include concurrency and performance considerations where relevant
  - wrap all body lines to ≤ 80 characters (hard limit)
- footers (as needed):
  - Breaking-Change: <description>
  - Refs: #123, Fixes: #123
  - Co-authored-by: Name <email>

Examples:
- fix(iterator): rebind views/iterators to local storage for safe copies
- perf(waitmap): reduce notify scan with concrete-path fast path
- docs(overview): document compile_commands.json refresh workflow
- build(scripts): prefer Ninja and auto-parallelize by CPU core count
- refactor(path): unify iterator types and remove string_view end() usage
- test(multithreading): shorten perf case without reducing coverage
- chore(logging): gate SP_LOG_DEBUG output behind PATHSPACE_LOG
- feat(layer): add alias retarget notification path forwarding

Full examples with body:

fix(task): handle timeout edge-case in wait loop with minimal lock time
- Why: avoid holding registry lock across condition waits; fixes rare
  deadlock
- Concurrency: narrows critical section; uses short slices to re-check
  readiness
- Tests: adds regression test and exercises timeout in looped scenario
Refs: #123

perf(core): reduce contention in PathSpace::out by tuning wait slices
- Why: lower spurious wake-ups and context switches under high contention
- Performance: ~15% lower involuntary context switches in heavy
  tests
- Risk: semantics unchanged; only timing granularity adjusted

When a change affects core behavior (paths, NodeData, WaitMap, TaskPool, serialization), update both docs/AI_OVERVIEW.md and docs/AI_ARCHITECTURE.md. For build/script changes, update the “Tests & build” section in docs/AI_OVERVIEW.md. For PRs authored by AI, ensure the PR title/body rules above are followed (capitalized title without Conventional Commit prefixes; body includes Purpose + AI Change Log).

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
   - traverses components using `Iterator` to locate/create nodes in the Node trie — `PathSpace/src/pathspace/core/Node.hpp`
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
  |       |-- root : Node (trie)
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