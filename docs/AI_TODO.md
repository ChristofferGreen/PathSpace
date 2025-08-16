# PathSpace — AI TODO & Roadmap

Purpose
- Provide a concise, AI-friendly backlog of well-scoped tasks with machine-readable metadata.
- Keep tasks tied to concrete code and docs locations so assistants can navigate and implement safely.
- Ensure changes follow repo documentation rules and update the right sections.

How to use
- Parse the YAML in “Machine-readable tasks” to list or select work.
- For any task that touches core behavior (paths, NodeData, WaitMap/WatchRegistry, Task/TaskT, serialization), update both docs/AI_OVERVIEW.md and docs/AI_ARCHITECTURE.md as described below.
- Validate that all referenced repo-relative file paths exist and keep links stable (update paths in the same PR if you move files).
- Run: ./scripts/compile.sh --clean --test --loop=15 before pushing (see CI and pre-push notes in docs/AI_OVERVIEW.md).

Conventions for tasks
- Allowed fields:
  - id: short, stable identifier (e.g., REG-WATCH-GLOB)
  - title: human-readable summary
  - rationale: why this matters (correctness, performance, DX/UX)
  - area: one of [core, concurrency, api, docs, build, ci, observability]
  - priority: P0 (critical), P1 (high), P2 (medium), P3 (low)
  - complexity: S/M/L/XL (rough implementation complexity)
  - status: planned | in_progress | blocked | done
  - labels: free-form tags (e.g., “glob”, “wait-notify”, “docs”)
  - code_paths: repo-relative paths that will likely change
  - test_paths: tests to extend or add (prefer extending existing)
  - doc_paths: docs to update; follow rules in docs/.rules
  - acceptance_criteria: concrete bullet points for verification
  - steps: recommended implementation outline
  - risks: pitfalls to watch out for
- Keep tasks concise, reference the smallest reasonable set of files, and prefer updates to existing tests over creating new harnesses.

Machine-readable tasks
```yaml
tasks:
  - id: REG-WATCH-GLOB
    title: Add glob watch support to WatchRegistry
    rationale: Align wait/notify with WaitMap’s glob semantics to unify blocking reads and notifications.
    area: core
    priority: P1
    complexity: M
    status: planned
    labels: [glob, wait-notify, WatchRegistry, WaitMap]
    code_paths:
      - PathSpace/src/pathspace/core/WatchRegistry.hpp
      - PathSpace/src/pathspace/core/PathSpaceContext.hpp
      - PathSpace/src/pathspace/PathSpace.cpp
      - PathSpace/src/pathspace/core/WaitMap.hpp
    test_paths:
      - PathSpace/tests/unit/core/test_WaitMap.cpp
      - PathSpace/tests/unit/test_PathSpace_execution.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_ARCHITECTURE.md
    acceptance_criteria:
      - Waiting on a glob path (e.g., "/a/*/c") wakes when matching concrete notifications occur.
      - Existing WaitMap glob tests are extended or mirrored to cover WatchRegistry paths.
      - No deadlocks or self-wake races; loop=15 remains green.
    steps:
      - Extend internal structures to index glob waiters efficiently; avoid O(N) scans where possible.
      - Ensure PathSpace::out register/wait/notify calls use the registry consistently for both concrete and glob.
      - Mirror/extend “Path Pattern Matching” subcases to assert notifications for glob waiters.
      - Update AI docs to describe unified wait/notify semantics and complexity trade-offs.
    risks:
      - Performance regressions if naive scanning; ensure short critical sections and amortized behavior.

  - id: OUT-SLICE-TUNABLES
    title: Make blocking slice duration configurable
    rationale: Reduce latency variance; allow tuning per call or globally.
    area: api
    priority: P2
    complexity: S
    status: planned
    labels: [blocking, timeout, ergonomics]
    code_paths:
      - PathSpace/src/pathspace/core/Out.hpp
      - PathSpace/src/pathspace/PathSpace.cpp
    test_paths:
      - PathSpace/tests/unit/test_PathSpace_read.cpp
      - PathSpace/tests/unit/test_PathSpace_execution.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_ARCHITECTURE.md
    acceptance_criteria:
      - Out options expose a slice duration (e.g., milliseconds) with sensible default (current ~20ms).
      - Tests cover small and larger slices and confirm timeout behavior bounds.
    steps:
      - Add a field to Out (e.g., waitSliceMs) with defaults.
      - Thread through PathSpace::out loop; respect deadline math.
      - Update docs and examples.

  - id: LOG-ALLOWLIST
    title: Introduce logger allowlist (enabledTags) and prune noisy defaults
    rationale: Keep tests and CI quiet by default; enable targeted categories on demand.
    area: observability
    priority: P2
    complexity: S
    status: planned
    labels: [logging, DX]
    code_paths:
      - PathSpace/src/pathspace/log/TaggedLogger.hpp
      - PathSpace/src/pathspace/log/TaggedLogger.cpp
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - enabledTags allowlist is supported and defaults to empty; skipTags remains for convenience.
      - Minimal default output; targeted tags can be enabled at runtime for debugging.
    steps:
      - Add allowlist guard in logging hot path; prefer allowlist if non-empty.
      - Provide a simple API for toggling tags; document usage.

  - id: DOCS-BACKPRESSURE
    title: Document back-pressure handling and queue growth bounds
    rationale: Clarify system behavior under high producer/consumer skew and how to mitigate.
    area: docs
    priority: P1
    complexity: S
    status: planned
    labels: [docs, performance]
    code_paths:
      - PathSpace/src/pathspace/core/NodeData.cpp
      - PathSpace/src/pathspace/task/TaskPool.cpp
    test_paths:
      - PathSpace/tests/unit/test_PathSpace_multithreading.cpp
    doc_paths:
      - PathSpace/docs/AI_ARCHITECTURE.md
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - AI_ARCHITECTURE.md “Back-Pressure Handling” section is populated with current behavior, queueing, and mitigation.
      - Pointers to NodeData type queues, task queues, and any limits; trade-offs explained.
      - One test subcase references doc guidance via comments.

  - id: DOCS-FAULT-TOLERANCE
    title: Document fault tolerance and error propagation
    rationale: Make error surfaces predictable (task exceptions, read/take failures, shutdown behavior).
    area: docs
    priority: P1
    complexity: S
    status: planned
    labels: [docs, errors]
    code_paths:
      - PathSpace/src/pathspace/task/TaskPool.cpp
      - PathSpace/src/pathspace/core/NodeData.cpp
      - PathSpace/src/pathspace/PathSpace.cpp
    test_paths:
      - PathSpace/tests/unit/task/test_TaskPool.cpp
      - PathSpace/tests/unit/test_PathSpace_execution.cpp
    doc_paths:
      - PathSpace/docs/AI_ARCHITECTURE.md
    acceptance_criteria:
      - “Fault Tolerance” section describes exception handling, retries (if any), and read/take error codes.
      - Link to tests that exercise exception paths; clarify expected logs.

  - id: DOCS-DEFAULT-PATHS-VIEWS
    title: Fill in “Default Paths” and “Views” documentation
    rationale: Help engineers understand view semantics and expected defaults/mounts.
    area: docs
    priority: P2
    complexity: S
    status: planned
    labels: [docs, views]
    code_paths:
      - PathSpace/src/pathspace/layer/PathView.hpp
      - PathSpace/src/pathspace/layer/PathView.cpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathView.cpp
    doc_paths:
      - PathSpace/docs/AI_ARCHITECTURE.md
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - “Views” and “Default Paths” sections describe API, invariants, and examples mapped to existing tests.
      - Cross-references to file paths and tests are valid.

  - id: CI-MACOS
    title: Add a macOS CI job mirroring loop=15
    rationale: Catch platform-specific issues early (allocator, threading, libc++).
    area: ci
    priority: P2
    complexity: S
    status: planned
    labels: [ci, macos]
    code_paths:
      - PathSpace/.github/workflows/ci.yml
      - PathSpace/scripts/compile.sh
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - A macOS job runs ./scripts/compile.sh --clean --test --loop=15.
      - Branch protection includes both Linux and macOS checks.

  - id: FUTURE-PEEK-TESTS
    title: Add tests for peekFuture (legacy) and readFuture (typed any)
    rationale: Lock in handle exposure for execution nodes and prevent regressions.
    area: core
    priority: P2
    complexity: S
    status: planned
    labels: [execution, futures, tests]
    code_paths:
      - PathSpace/src/pathspace/core/Leaf.cpp
      - PathSpace/src/pathspace/core/NodeData.cpp
      - PathSpace/src/pathspace/PathSpaceBase.hpp
    test_paths:
      - PathSpace/tests/unit/test_PathSpace_execution.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - New subcases assert that peekFuture returns a valid Future when an execution exists.
      - readFuture returns a valid FutureAny for typed tasks; both paths non-blocking and consistent.

  - id: REG-UNIFY-SEMANTICS
    title: Unify WaitMap and WatchRegistry semantics or provide a clear adapter
    rationale: Reduce mental overhead; ensure one authoritative path for wait/notify semantics.
    area: concurrency
    priority: P2
    complexity: M
    status: planned
    labels: [wait-notify, unification]
    code_paths:
      - PathSpace/src/pathspace/core/WaitMap.hpp
      - PathSpace/src/pathspace/core/WatchRegistry.hpp
      - PathSpace/src/pathspace/core/PathSpaceContext.hpp
      - PathSpace/src/pathspace/PathSpace.cpp
    test_paths:
      - PathSpace/tests/unit/core/test_WaitMap.cpp
      - PathSpace/tests/unit/test_PathSpace_multithreading.cpp
    doc_paths:
      - PathSpace/docs/AI_ARCHITECTURE.md
    acceptance_criteria:
      - A single abstraction governs waiters; glob/concrete behaviors documented and tested.
      - No functional regressions; loop=15 remains green; performance notes updated.
```

Change management and docs rules (must follow)
- Update both docs/AI_OVERVIEW.md and docs/AI_ARCHITECTURE.md when core behavior changes (paths, NodeData, WaitMap/WatchRegistry, Task/TaskT, serialization).
- If build scripts or compile flags change, update the “Tests & build” section in docs/AI_OVERVIEW.md (compile.sh examples, flags).
- Keep references stable: use repo-relative paths like `src/pathspace/core/NodeData.cpp`.
- For images/diagrams, store under docs/images/ with appropriate alt text and size guidance.

Tips for AI editors
- Before edits, grep to scope changes:
  - Examples:
    - Watchers: grep -nR "wait(" src/pathspace | grep -E "(Context|Registry|WaitMap)"
    - Futures: grep -nR "peek.*Future" src/pathspace
    - Logging: grep -nR "sp_log\\(" src/pathspace
- Prefer extending existing tests over introducing new frameworks.
- Add clear logging temporarily if needed; keep default logs quiet and remove/guard noisy lines before merging.
- Validate with: ./scripts/compile.sh --clean --test --loop=15

Appendix: Status vocabulary
- planned: agreed task not yet started
- in_progress: branch exists; partial implementation
- blocked: waiting on dependency
- done: merged to master and validated by CI (loop=15)
