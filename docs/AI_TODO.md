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
  - id: PAPER-OUTLINE
    title: Write a paper outline for PathSpace (tech and philosophy)
    rationale: Establish scope, contributions, and structure early to guide writing and experiments.
    area: docs
    priority: P1
    complexity: M
    status: planned
    labels: [paper, research, writing]
    code_paths: []
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_ARCHITECTURE.md
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - 1–2 page outline with sections: Introduction, Motivation, System Overview, Design Principles, Core Abstractions (paths, NodeData, WatchRegistry/WaitMap, Task model), Concurrency, Performance, Case Studies, Related Work, Limitations, Future Work.
      - Clear thesis and list of contributions (bulleted).
      - Timeline and artifact plan referenced.
    steps:
      - Draft outline in docs/ (e.g., docs/paper/outline.md).
      - Iterate with stakeholders; finalize scope and contributions.
      - Align outline sections with existing docs to avoid duplication.
  - id: PAPER-FIGURES
    title: Prepare figures and diagrams
    rationale: Visuals clarify PathSpace architecture and data flow.
    area: docs
    priority: P2
    complexity: M
    status: planned
    labels: [paper, figures, diagrams]
    code_paths: []
    test_paths: []
    doc_paths:
      - PathSpace/docs/images/
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_ARCHITECTURE.md
    acceptance_criteria:
      - At least 4 figures: high-level architecture, data flow (insert/read/take), wait/notify sequence, concurrency model.
      - Stored as SVG in docs/images/ with alt text.
      - Referenced in both the paper draft and AI docs.
    steps:
      - Create Mermaid/SVG diagrams; ensure legibility in light/dark.
      - Add captions/alt text; cross-link from relevant sections.
  - id: PAPER-BENCH
    title: Benchmark plan and results for the paper
    rationale: Provide quantitative support for claims (latency, throughput, scalability).
    area: docs
    priority: P1
    complexity: L
    status: planned
    labels: [paper, benchmarks, performance]
    code_paths:
      - PathSpace/scripts/compile.sh
      - PathSpace/src/pathspace/task/TaskPool.cpp
      - PathSpace/src/pathspace/core/NodeData.cpp
    test_paths:
      - PathSpace/tests/unit/test_PathSpace_multithreading.cpp
    doc_paths:
      - PathSpace/docs/AI_ARCHITECTURE.md
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - Reproducible benchmark scripts and documented environment.
      - Results include med/avg/percentiles for key operations; graphs included.
      - Discussion of trade-offs and limitations; ties back to design principles.
    steps:
      - Define workloads; add scripts/bench targets as needed.
      - Capture results; generate plots (SVG/PNG) under docs/images/.
      - Integrate into paper draft with interpretation.
  - id: PAPER-ARTIFACT
    title: Artifact and replication package
    rationale: Ensure the paper is reproducible and useful to reviewers and readers.
    area: docs
    priority: P1
    complexity: M
    status: planned
    labels: [paper, artifact, reproducibility]
    code_paths:
      - PathSpace/scripts/compile.sh
      - PathSpace/.github/workflows/ci.yml
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_TODO.md
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - A tagged release with instructions to build, test, and reproduce experiments.
      - CI job verifies artifact building and test loops; checksums for large assets if any.
      - README with step-by-step replication guidance.
    steps:
      - Script a one-command setup; document dependencies clearly.
      - Add CI job to validate replication path on Linux (and macOS if added).
  - id: PAPER-RELATED
    title: Related work survey
    rationale: Position PathSpace among existing systems and abstractions.
    area: docs
    priority: P2
    complexity: M
    status: planned
    labels: [paper, related-work]
    code_paths: []
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - Annotated bibliography with 10–20 key references; mapping to PathSpace contributions.
      - Summary table (CSV/Markdown) of features vs. systems if applicable.
    steps:
      - Collect references; write summaries; identify differentiators.
  - id: PAPER-VENUE
    title: Select target venue and formatting
    rationale: Guides writing style, page limits, and evaluation criteria.
    area: docs
    priority: P1
    complexity: S
    status: planned
    labels: [paper, venue]
    code_paths: []
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - Venue chosen with deadline and formatting template captured.
      - Paper skeleton updated to match template.
    steps:
      - Evaluate venues (systems, PL, databases); pick best fit and deadline.
      - Add template (LaTeX/Markdown) under docs/paper/.
  - id: PAPER-DRAFT
    title: Full paper draft
    rationale: Produce a complete draft ready for internal review.
    area: docs
    priority: P1
    complexity: L
    status: planned
    labels: [paper, draft]
    code_paths: []
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - Complete draft with abstract, figures, results, and references.
      - Internal review feedback addressed; ready for submission polish.
    steps:
      - Convert outline to prose; integrate figures and benchmarks.
      - Iterate with reviewers; stabilize claims and wording.
  - id: PAPER-CITATION
    title: Citation and BibTeX
    rationale: Provide a stable reference for PathSpace versions/releases.
    area: docs
    priority: P3
    complexity: S
    status: planned
    labels: [paper, citation]
    code_paths: []
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - BibTeX entry and recommended citation text added to docs.
      - Links to tagged release/DOI if applicable.
    steps:
      - Add CITATION.cff and docs/citation.md with examples.
  - id: LAYER-PATHIO-DECIDE
    title: Decide PathIO fate (implement or remove)
    rationale: PathIO is a broken stub (constructor syntax error, duplicate Permission) and currently unused; clarify intent to reduce confusion or implement a proper IO bridge.
    area: core
    priority: P1
    complexity: S
    status: planned
    labels: [layer, io, cleanup]
    code_paths:
      - PathSpace/src/pathspace/layer/PathIO.hpp
      - PathSpace/src/pathspace/layer/PathIO.cpp
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - EITHER: PathIO is removed from the tree; references (if any) cleaned up.
      - OR: PathIO compiles with a valid constructor, no duplicate Permission type, and clear documented semantics.
    steps:
      - If removing: delete PathIO files and update CMake if needed.
      - If implementing: fix constructor, unify Permission (see LAYER-PERMISSION-UNIFY), implement minimal semantics or explicitly document unsupported ops.
  - id: LAYER-FS-COMPLETE
    title: Complete PathFileSystem (read robustness and write support)
    rationale: PathFileSystem is read-only and minimal; add write support (optional), robust error mapping, and explicit includes.
    area: core
    priority: P2
    complexity: M
    status: planned
    labels: [layer, filesystem]
    code_paths:
      - PathSpace/src/pathspace/layer/PathFileSystem.hpp
      - PathSpace/src/pathspace/layer/PathFileSystem.cpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathFileSystem.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - PathFileSystem.cpp explicitly includes <fstream>.
      - out() remains read-only for std::string or is extended per design; errors are precise (NotFound, TypeMismatch, Permission, etc.).
      - in() either returns clear Unsupported error or supports writing strings to files (creating parent dirs optional).
      - Tests updated to cover write behavior or assert read-only semantics.
    steps:
      - Add <fstream> include; audit path joining and error messages.
      - Decide on write semantics; implement or explicitly document read-only.
      - Extend tests accordingly.
  - id: LAYER-PERMISSION-UNIFY
    title: Unify Permission type across layers
    rationale: Avoid duplicate Permission structs (PathView vs PathIO); provide a single shared definition for consistency.
    area: api
    priority: P2
    complexity: S
    status: planned
    labels: [layer, api, cleanup]
    code_paths:
      - PathSpace/src/pathspace/layer/PathView.hpp
      - PathSpace/src/pathspace/layer/PathIO.hpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathView.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - A single Permission struct is defined in a shared header (e.g., layer/Permission.hpp).
      - PathView and any other layers include and use the shared type.
      - Tests compile and pass without name clashes.
    steps:
      - Introduce layer/Permission.hpp with the shared struct.
      - Replace local Permission definitions and fix includes.
  - id: ALLOC-PMR-BASE
    title: Introduce std::pmr infrastructure and central memory resource
    rationale: Allow users to plug in custom allocators/resources for reduced fragmentation, better locality, or instrumentation.
    area: core
    priority: P1
    complexity: M
    status: planned
    labels: [allocator, pmr, memory]
    code_paths:
      - PathSpace/src/pathspace/core/Node.hpp
      - PathSpace/src/pathspace/core/NodeData.hpp
      - PathSpace/src/pathspace/type/SlidingBuffer.hpp
      - PathSpace/src/pathspace/core/PathSpaceContext.hpp
    test_paths:
      - PathSpace/tests/unit/test_PathSpace_insert.cpp
      - PathSpace/tests/unit/test_PathSpace_multithreading.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_ARCHITECTURE.md
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - PathSpaceContext exposes setMemoryResource(std::pmr::memory_resource*) and getMemoryResource().
      - Default resource uses std::pmr::get_default_resource(); null is rejected.
      - Core containers support construction with a std::pmr::polymorphic_allocator from the context.
    steps:
      - Add memory_resource pointer to PathSpaceContext and thread getters to PathSpaceBase/Leaf construction sites.
      - Define aliases/types to ease adoption (e.g., PMRVector<T> = std::pmr::vector<T>).
      - Prepare Node/SlidingBuffer for allocator-aware migration (no functional change yet).
    risks:
      - Lifetime ordering of memory_resource; ensure resource outlives containers using it.
  - id: ALLOC-CONTAINER-MIGRATE
    title: Migrate internal containers to PMR-aware variants
    rationale: Enable allocator injection for hotspots (SlidingBuffer, trie children map).
    area: core
    priority: P1
    complexity: M
    status: planned
    labels: [allocator, pmr, containers]
    code_paths:
      - PathSpace/src/pathspace/type/SlidingBuffer.hpp
      - PathSpace/src/pathspace/core/Node.hpp
    test_paths:
      - PathSpace/tests/unit/type/test_SlidingBuffer.cpp
      - PathSpace/tests/unit/path/test_Iterator.cpp
    doc_paths:
      - PathSpace/docs/AI_ARCHITECTURE.md
    acceptance_criteria:
      - SlidingBuffer uses std::pmr::vector<uint8_t> and accepts a pmr::polymorphic_allocator.
      - Node::ChildrenMap uses a PMR allocator parameter (std::pmr::polymorphic_allocator for value_type).
      - All tests pass loop=15 with default resource.
    steps:
      - Add constructors to SlidingBuffer to accept a polymorphic_allocator; default to context resource.
      - Update Node::ChildrenMap allocator typedef to a PMR allocator; ensure phmap template parameters are updated accordingly.
      - Propagate context memory_resource into these components during construction.
    risks:
      - phmap allocator interactions; validate template parameter correctness and performance.
  - id: ALLOC-API
    title: Public API for user-supplied allocator/resource
    rationale: Let users supply custom memory strategies (monotonic pools, tracking allocators).
    area: api
    priority: P1
    complexity: S
    status: planned
    labels: [allocator, pmr, api]
    code_paths:
      - PathSpace/src/pathspace/PathSpace.hpp
      - PathSpace/src/pathspace/core/PathSpaceContext.hpp
    test_paths:
      - PathSpace/tests/unit/test_PathSpace_insert.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - PathSpace constructor or context exposes a setter for memory_resource.
      - Example demonstrates using a monotonic_buffer_resource backing.
    steps:
      - Add overloads/constructors to take std::pmr::memory_resource*.
      - Document expected lifetime: resource must outlive PathSpace and its containers.
  - id: ALLOC-TESTS
    title: Allocation-aware tests and instrumentation hooks
    rationale: Verify containers honor the provided allocator and maintain correctness under stress.
    area: core
    priority: P2
    complexity: M
    status: planned
    labels: [allocator, tests]
    code_paths:
      - PathSpace/src/pathspace/type/SlidingBuffer.hpp
    test_paths:
      - PathSpace/tests/unit/type/test_SlidingBuffer.cpp
      - PathSpace/tests/unit/test_PathSpace_multithreading.cpp
    doc_paths:
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - Custom memory_resource counting allocations is used in tests; assertions verify allocation paths.
      - Stress subcases with many inserts/reads succeed with custom resource.
    steps:
      - Implement a simple counting_resource in tests.
      - Add subcases toggling default vs custom resource.
  - id: ALLOC-DOCS
    title: Document allocator integration and best practices
    rationale: Guide users to choose and plug allocators safely (lifetime, pooling).
    area: docs
    priority: P2
    complexity: S
    status: planned
    labels: [allocator, docs]
    code_paths: []
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_ARCHITECTURE.md
    acceptance_criteria:
      - Overview contains a “Custom allocator” section with code snippets and caveats.
      - Architecture notes summarize allocator-aware components and perf trade-offs.
    steps:
      - Add examples for monotonic and unsynchronized_pool_resource usage.
      - Note thread-safety and deallocation behavior when popping data.
  - id: ALLOC-BENCH
    title: Benchmarks comparing default vs custom allocators
    rationale: Quantify impact on throughput/latency and memory usage.
    area: docs
    priority: P2
    complexity: M
    status: planned
    labels: [allocator, benchmarks]
    code_paths:
      - PathSpace/scripts/compile.sh
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_TODO.md
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - Bench scripts run with default resource and a tuned custom resource; results summarized in docs.
      - loop=15 remains green under both configurations.
    steps:
      - Add flags to bench scripts to select memory resource.
      - Capture metrics and include plots under docs/images/.
  - id: PATHIO-ARCH
    title: Define PathIO architecture for input/output devices (mouse, keyboard, gamepad, touch, tablet)
    rationale: Establish a clear, extensible layer to expose device I/O via PathSpace paths with consistent semantics.
    area: core
    priority: P1
    complexity: M
    status: planned
    labels: [io, devices, input, output, api, layer]
    code_paths:
      - PathSpace/src/pathspace/layer/PathIO.hpp
      - PathSpace/src/pathspace/layer/PathIO.cpp
      - PathSpace/src/pathspace/layer/PathView.hpp
      - PathSpace/src/pathspace/PathSpaceBase.hpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - PathIO header and implementation compile with a valid constructor and no duplicate Permission types.
      - Clear path conventions are documented (e.g., /io/mouse, /io/keyboard, /io/gamepad, /io/touch, /io/tablet).
      - Read/write semantics, blocking/timeout, and notify behavior are specified.
    steps:
      - Fix PathIO header (constructor) and remove local Permission type (see LAYER-PERMISSION-UNIFY).
      - Draft path scheme and minimal capability matrix per device type.
      - Document v1 scope and non-goals in AI_OVERVIEW.md.

  - id: PATHIO-MACOS-BACKENDS
    title: Implement macOS backends (Quartz/CoreGraphics, IOKit, HID) for device input
    rationale: Provide a concrete first backend to exercise PathIO semantics via real devices on macOS.
    area: core
    priority: P1
    complexity: L
    status: planned
    labels: [io, macos, hid, devices]
    code_paths:
      - PathSpace/src/pathspace/layer/PathIO.cpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - Mouse move/click events can be read via /io/mouse (non-blocking and blocking read with timeout).
      - Keyboard keydown/keyup events readable via /io/keyboard.
      - Notify/wait triggers on event arrival; loop=15 remains green with simulated inputs in tests.
    steps:
      - Abstract a small OS interface for event capture; stub in tests.
      - Implement macOS event polling/callback translation into PathSpace events.
      - Add tests using a simulated backend to avoid device dependency in CI.

  - id: PATHIO-EVENT-MODEL
    title: Define a stable event model and serialization for device inputs
    rationale: Ensure events are structured, versioned, and easy to deserialize/consume from PathSpace.
    area: api
    priority: P1
    complexity: M
    status: planned
    labels: [io, events, schema]
    code_paths:
      - PathSpace/src/pathspace/type/InputMetadata.hpp
      - PathSpace/src/pathspace/layer/PathIO.hpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_ARCHITECTURE.md
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - Event structs for mouse, keyboard, gamepad, touch, tablet with timestamps and device ids.
      - Serialization/deserialization covered by tests; type-safe reads in examples.
    steps:
      - Introduce minimal event types and wire InputMetadata serializers.
      - Add subcases that emit and read back event sequences.

  - id: PATHIO-STREAMING
    title: Support streaming reads and back-pressure for high-frequency devices
    rationale: Handle event bursts (e.g., mouse movement) without losing data or stalling consumers.
    area: concurrency
    priority: P2
    complexity: M
    status: planned
    labels: [io, streaming, backpressure]
    code_paths:
      - PathSpace/src/pathspace/type/SlidingBuffer.hpp
      - PathSpace/src/pathspace/core/NodeData.cpp
      - PathSpace/src/pathspace/layer/PathIO.cpp
    test_paths:
      - PathSpace/tests/unit/test_PathSpace_multithreading.cpp
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_ARCHITECTURE.md
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - Streaming read API or guidance for polling at bounded intervals.
      - Tests simulate bursts without timeouts; document dropped-event policy if applicable.
    steps:
      - Evaluate SlidingBuffer adequacy for event queues; add PMR support if needed.
      - Add tests with synthetic high-rate inputs and verify consumer behavior.

  - id: PATHIO-PERMISSIONS
    title: Integrate PathView permission model for device access control
    rationale: Enforce read/execute permissions per device path; avoid accidental writes where unsupported.
    area: api
    priority: P2
    complexity: S
    status: planned
    labels: [io, permissions, security]
    code_paths:
      - PathSpace/src/pathspace/layer/PathView.hpp
      - PathSpace/src/pathspace/layer/PathIO.hpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathView.cpp
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - Permission callback applied for /io/* subpaths; denied access returns InvalidPermissions.
      - Tests cover allowed/denied combinations for read/execute.
    steps:
      - Reuse PathView Permission pattern; add examples in docs and tests.

  - id: PATHIO-SIM-TESTS
    title: Simulation backends for deterministic CI tests
    rationale: CI cannot rely on real devices; simulate events and assert PathIO behavior.
    area: ci
    priority: P1
    complexity: M
    status: planned
    labels: [io, testing, simulation]
    code_paths:
      - PathSpace/src/pathspace/layer/PathIO.cpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - A test-only backend feeds scripted events to PathIO paths.
      - All PathIO tests pass in loop=15 without device access.
    steps:
      - Introduce a compile-time or runtime switch for simulated backend.
      - Write tests for mouse and keyboard streams.

  - id: PATHIO-OUTPUT-HAPTICS
    title: Define and stub output/haptics APIs
    rationale: Provide a forward path for output devices (rumble/haptics), even if initially unimplemented.
    area: api
    priority: P3
    complexity: M
    status: planned
    labels: [io, output, haptics]
    code_paths:
      - PathSpace/src/pathspace/layer/PathIO.hpp
      - PathSpace/src/pathspace/layer/PathIO.cpp
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - Output path conventions documented (e.g., /io/gamepad/<id>/rumble).
      - Calls return clear Unsupported errors until a backend exists.
    steps:
      - Add API shape and clear error mapping; defer backend.

  - id: PATHIO-CMAKE
    title: CMake wiring and platform guards for PathIO backends
    rationale: Build only supported backends per platform and keep CI portable.
    area: build
    priority: P2
    complexity: S
    status: planned
    labels: [build, cmake, io]
    code_paths:
      - PathSpace/src/pathspace/CMakeLists.txt
      - PathSpace/CMakeLists.txt
    test_paths: []
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_TODO.md
    acceptance_criteria:
      - Optional macOS backend target compiles only on macOS.
      - Linux/Windows stubs compile cleanly; tests use simulation backend on CI.
    steps:
      - Add options and platform checks in CMake; document flags.
 ```

Additional machine-readable tasks (device path namespace)

```yaml
tasks:
  - id: PATHS-DEVICE-NAMESPACE
    title: Design standard device path namespace (Plan 9/Linux style)
    rationale: Establish a canonical, extensible namespace for device I/O paths similar to Plan 9/Linux, enabling consistent discovery and interaction across mice, keyboards, gamepads, touch screens, and tablets.
    area: api
    priority: P1
    complexity: M
    status: planned
    labels: [io, devices, namespace, plan9]
    code_paths:
      - PathSpace/src/pathspace/layer/PathIO.hpp
      - PathSpace/src/pathspace/layer/PathIO.cpp
      - PathSpace/src/pathspace/layer/PathView.hpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
      - PathSpace/docs/AI_ARCHITECTURE.md
    acceptance_criteria:
      - Define canonical paths such as /dev/mouse/<id>, /dev/keyboard/<id>, /dev/gamepad/<id>, /dev/touch/<id>, /dev/tablet/<id>, and /screen/<id>.
      - Standardize reserved subpaths: /dev/<class>/<id>/events, /dev/<class>/<id>/state, /dev/<class>/<id>/control, and /dev/<class>/<id>/meta.
      - Document normalization rules (case, component encoding) and provide validation helpers.
    steps:
      - Draft the namespace map and reserved subpaths with examples.
      - Implement path validation and canonicalization helpers.
      - Update PathIO routing to dispatch to device-specific backends based on the canonical paths.
    risks:
      - Platform differences and OS-specific semantics; avoid leaking OS details into the public namespace.

  - id: PATHS-DEVICE-DISCOVERY
    title: Device discovery and enumeration
    rationale: Provide stable discovery endpoints and metadata so clients can enumerate available devices and their properties.
    area: api
    priority: P1
    complexity: M
    status: planned
    labels: [io, discovery, metadata]
    code_paths:
      - PathSpace/src/pathspace/layer/PathIO.cpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - /dev lists device classes; /dev/<class> lists devices; /dev/<class>/<id>/meta returns structured metadata.
      - Metadata includes vendor, product, device id, capabilities, connection type, and versioning.
      - Simulated backend yields deterministic enumerations for CI tests.
    steps:
      - Define metadata schema and serializers.
      - Implement enumeration in the simulation backend; document real backend plans.

  - id: PATHS-DEVICE-CAPABILITIES
    title: Capabilities and feature flags per device
    rationale: Let clients adapt to device features (multitouch, pressure, haptics, extra buttons) via a clear capability surface.
    area: api
    priority: P2
    complexity: S
    status: planned
    labels: [io, capabilities]
    code_paths:
      - PathSpace/src/pathspace/layer/PathIO.hpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_ARCHITECTURE.md
    acceptance_criteria:
      - Capability descriptors exposed at /dev/<class>/<id>/capabilities with versioned keys.
      - Tests assert presence/absence of capabilities and compatibility checks.
    steps:
      - Define capability keys and versioning; wire into metadata and discovery endpoints.

  - id: PATHS-DEVICE-SCREEN
    title: Standardize screen paths and operations
    rationale: Provide consistent paths for displays, modes, and optional frame capture.
    area: api
    priority: P2
    complexity: M
    status: planned
    labels: [screen, display, io]
    code_paths:
      - PathSpace/src/pathspace/layer/PathIO.cpp
    test_paths:
      - PathSpace/tests/unit/layer/test_PathIO.cpp
    doc_paths:
      - PathSpace/docs/AI_OVERVIEW.md
    acceptance_criteria:
      - /screen lists displays; /screen/<id>/meta and /screen/<id>/modes exist and return structured data.
      - Optional /screen/<id>/frame (simulation-only in tests) for capture semantics; documented as experimental.
    steps:
      - Define schemas; add simulated data; plan real backends in a later task.
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
    - Logging: grep -nR "sp_log\(" src/pathspace
- Prefer extending existing tests over introducing new frameworks.
- Add clear logging temporarily if needed; keep default logs quiet and remove/guard noisy lines before merging.
- Validate with: ./scripts/compile.sh --clean --test --loop=15

Appendix: Status vocabulary
- planned: agreed task not yet started
- in_progress: branch exists; partial implementation
- blocked: waiting on dependency
- done: merged to master and validated by CI (loop=15)
