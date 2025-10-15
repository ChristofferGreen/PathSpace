# Scene Graph Implementation Plan

> **Context update (October 15, 2025):** Implementation phases now assume the “Atlas” AI context; convert prior context cues to the Atlas vocabulary during execution.

## Context and Objectives
- Groundwork for implementing the renderer stack described in `docs/AI_Plan_SceneGraph_Renderer.md` and the broader architecture contract in `docs/AI_ARCHITECTURE.md`.
- Deliver an incremental path from today's codebase to the MVP (software renderer, surfaces, presenters, snapshot builder), while keeping room for Metal/Vulkan and HTML adapters outlined in the master plan.
- Maintain app-root atomic semantics, immutable snapshot guarantees, and observability expectations throughout the rollout.

Success looks like:
- A new `src/pathspace/ui/` subsystem shipping the MVP feature set behind build flags.
- Repeatable end-to-end tests (including 15× loop runs) covering snapshot publish/adopt, render, and present flows.
- Documentation, metrics, and diagnostics that let maintainers debug renderer issues without spelunking through the code.

## Workstream Overview
- **Typed wiring helpers** — `Builders.hpp` plus supporting utilities for app-relative path validation, target naming, and atomic parameter writes.
- **Scene authoring & snapshot builder** — authoring tree schema, dirty tracking, layout, `DrawableBucket` emission, and revision lifecycle/GC.
- **Renderer core (software first)** — target settings adoption, traversal of snapshots, draw command execution, color pipeline defaults, and concurrency controls.
- **Surfaces & presenters** — target configuration, render execution coordination, presentation policy enforcement, progressive software mode, and UI-thread integrations.
- **Input & hit testing** — DrawableBucket-driven hit paths, focus routing, and notification hooks for scene edits and event delivery.
- **Diagnostics & metrics** — unified `PathSpaceError` usage, per-target metrics, progressive copy counters, and troubleshooting hooks.
- **Resource residency & tooling** — enforce per-resource policy, adopt fingerprints across snapshots/renderers, refresh compile commands, and extend automation/test harnesses.

Each workstream lands independently but respects shared contracts (paths, atomic writes, revision pinning).

### Helper API Contract
- Location: `src/pathspace/ui/Helpers.hpp` with namespaces `Scene`, `Renderer`, `Surface`, `Window`, and `Diagnostics`.
- All helpers accept a `PathSpace&` plus typed `ConcretePath` aliases (`ScenePath`, `RendererPath`, etc.); no raw string paths cross the boundary.
- Responsibilities:
  - Enforce app-root containment and canonical naming (e.g., `renderers/<id>/targets/...`).
  - Serialize payloads into `NodeData` using existing Alpaca/typed codecs; no helper owns synchronization beyond atomic inserts/takes.
  - Return `SP::Expected<T>` for error propagation aligned with core PathSpace errors.
- Usage: SceneGraph code consumes these helpers exclusively instead of embedding path literals; tests mock them to isolate higher layers.
- Future additions (Metal/HTML adapters) extend these namespaces rather than reaching into core PathSpace directly.

## Immediate Next Steps
- Expand `PathRenderer2D` beyond bounding-box fills: interpret drawable command streams, honor layer/opaque vs alpha ordering, and validate color-management paths (with failure-handling tests and goldens).
- For Phase 4 follow-ups, prepare end-to-end scene→render→present scenarios, wire presenters through `Window::Present`, surface progressive-mode metrics, codify seqlock/deadline tests, and rerun the loop harness once integrations land.
- Begin Phase 5 test authoring for hit ordering, clip-aware picking, focus routing, and event delivery latency; follow with DrawableBucket-backed picking and wait/notify integration for dirty markers and auto-render scheduling.
- Line up Phase 6 diagnostics/tooling work: extend error/metrics coverage, normalize `PathSpaceError` reporting, expand scripts for UI log capture, and draft the debugging playbook updates before the next hardening pass.

## Phase Plan
### Phase 0 — Foundations (1 sprint)
- Audit existing `PathSpace` helpers for app-relative enforcement; add shared utilities if missing.
- Introduce feature flags/CMake options (`PATHSPACE_ENABLE_UI`, `PATHSPACE_UI_SOFTWARE`, `PATHSPACE_UI_METAL`, `PATHSPACE_ENABLE_APP`, `PATHSPACE_ENABLE_EXTRA`) without functionality to keep builds green.
- Update Doxygen/docs stubs to reserve namespaces (`pathspace/ui/*`) and ensure compile_commands integration works.
- ✅ (October 11, 2025) `SP::App` helpers landed in `src/pathspace/app/AppPaths.{hpp,cpp}` with root normalization, app-relative resolution, and target base validation for reuse across UI and tooling layers.
- ✅ (October 11, 2025) Top-level CMake now exposes `PATHSPACE_ENABLE_UI` (default OFF) plus `PATHSPACE_UI_SOFTWARE` / `PATHSPACE_UI_METAL`, `PATHSPACE_ENABLE_APP`, and `PATHSPACE_ENABLE_EXTRA` flags, and the library/tests gate feature-specific sources on these toggles.

### Phase 1 — Typed Helpers & Path Semantics (1 sprint)
Completed:
- ✅ (October 14, 2025) Added doctest coverage in `tests/ui/test_Builders.cpp` for idempotent scene creation, cross-app rejection, and atomic `Renderer::UpdateSettings` behaviour (path containment + queue draining).
- ✅ (October 14, 2025) Validated Builders via `./scripts/compile.sh --loop=15 --per-test-timeout 20` (15 iterations, 20 s per-test timeout).
- ✅ (October 11, 2025) Initial UI helper implementations (`src/pathspace/ui/Helpers.cpp`) enforce app-root containment for scene/renderer/surface/window calls with accompanying doctests in `tests/ui/test_SceneHelpers.cpp`; remaining work will flesh out Builders.hpp and atomic settings writes.
- ✅ (October 14, 2025) Documented Builders usage patterns and invariants in `docs/AI_Plan_SceneGraph_Renderer.md` (idempotent creates, atomic settings, app-root enforcement); no new canonical paths required for `AI_PATHS.md`.

### Phase 2 — Scene Schema & Snapshot Builder (2 sprints)
Completed:
- ✅ (October 14, 2025) `SceneSnapshotBuilder` now emits full SoA drawable data (transforms, bounds, material/pipeline metadata, command offsets/counts, command stream, opaque/alpha/per-layer indices) and documents the layout in `docs/AI_Plan_SceneGraph_Renderer.md` / `docs/AI_ARCHITECTURE.md`.
- ✅ (October 14, 2025) Added doctests in `tests/ui/test_SceneSnapshotBuilder.cpp` covering round-trip decoding plus retention under burst publishes.
- ✅ (October 15, 2025) Added long-running publish/prune stress coverage with metrics validation and GC metric emission (retained/evicted/last_revision/total_fingerprint_count) in `tests/ui/test_SceneSnapshotBuilder.cpp`.
- ✅ (October 15, 2025) Snapshot metadata now records resource fingerprints per revision; GC metrics aggregate total fingerprint counts for residency planning.
- ✅ (October 15, 2025) Documented the finalized binary artifact split (`drawables.bin`, `transforms.bin`, `bounds.bin`, `state.bin`, `cmd-buffer.bin`, index files) and retired the Alpaca fallback in `docs/AI_ARCHITECTURE.md` / `docs/AI_Plan_SceneGraph_Renderer.md`.

### Phase 3 — Software Renderer Core (2 sprints)
Completed:
- ✅ (October 15, 2025) Builders `Surface::RenderOnce` / `Window::Present` now call `PathRenderer2D` synchronously via `Renderer::TriggerRender`, return ready `FutureAny` handles, and update target metrics; doctests cover the integration path.
- ✅ (October 16, 2025) `PathRenderer2D` now executes recorded Rect and RoundedRect commands in linear light, keeps the opaque/alpha partitioning path alive, and publishes drawable/cull/command metrics with doctest coverage for success/error flows.

Remaining:
- Draft golden framebuffer comparisons, render/present concurrency loops, and failure-handling tests up front.
- Extend `PathRenderer2D` beyond Rect/RoundedRect (Image, TextGlyphs, Path, Mesh) with full pipeline flag handling and blend semantics.
- Add golden framebuffer comparisons plus richer diagnostics (sort keys, overdraw/progressive stats) for regression detection.
- Re-run the looped test suite and inspect goldens post implementation.

### Phase 4 — Surfaces, Presenters, and Progressive Mode (2 sprints)
Completed:
- ✅ (October 15, 2025) `PathSurfaceSoftware` progressive buffer + double-buffer landed with dedicated UI tests.
- ✅ (October 15, 2025) `PathWindowView` now presents buffered/progressive frames and writes presenter metrics via `Builders::Diagnostics::WritePresentMetrics`; UI tests split into `PathSpaceUITests` for isolation.

Remaining:
- Create end-to-end scenario tests (scene edit → render → present, policy permutations) and progressive-mode assertions before coding.
- Integrate presenters with `Window::Present` helper so render→present happens through PathSpace (auto-render scheduling still pending).
- Metrics for present policy outcomes and progressive copy stats; hook into diagnostics paths for renderer/presenter tooling.
- Codify seqlock memory-ordering and deadline behavior in tests; ensure implementations follow the spec’s atomic protocols.
- Revalidate via the loop harness and scenario suites.

### Phase 5 — Input, Hit Testing, and Notifications (1 sprint)
- Author tests for hit ordering, clip-aware picking, focus routing, and event delivery latency.
- Implement DrawableBucket-backed hit testing, clip stack reconstruction, and focus bookkeeping per spec.
- Integrate wait/notify flows for scene dirty markers, snapshot rebuild triggers, and presenter auto-render scheduling.
- Exercise the notification loops in the mandated test harness and document latency/ordering guarantees.

### Phase 6 — Diagnostics, Tooling, and Hardening (1 sprint)
- Extend diagnostics-focused tests (error propagation, metrics publication) prior to implementation changes.
- Normalize error reporting through `PathSpaceError`; surface last error + revision info.
- Expand scripts/tests (`compile.sh`, CTest targets) to cover UI components and gather logs on failure.
- Document debugging playbook (how to inspect metrics/errors) in `docs/` and re-run test loops to confirm.

### Phase 7 — Optional Backends & HTML Adapter Prep (post-MVP)
- For each backend, author adapter-specific tests (integration replay, ObjC++ harness, HTML command parity) before the implementation.
- Metal surface/presenter gated by `PATHSPACE_UI_METAL`; confirm ObjC++ integration and CI coverage.
- HTML adapter scaffolding (command stream emitter + replay harness) behind experimental flag.
- Resource loader integration for fonts/images when snapshots require them.

## Dependencies and Ordering
- Helpers (Phase 1) unblock snapshot builder and surfaces/presenters by standardizing paths.
- Snapshot builder (Phase 2) must land before renderer to provide immutable input revisions.
- Renderer (Phase 3) depends on RenderSettings semantics and DrawableBucket schema from Phase 2.
- Surfaces/presenters (Phase 4) rely on renderer outputs and present policy definitions; progressive mode requires renderer metrics hooks.
- Input/hit testing & notifications (Phase 5) relies on renderer outputs, snapshot metadata, and presenter hooks established in earlier phases.
- Diagnostics/tooling (Phase 6) builds on metrics and error reporting emitted by previous phases; schedule alongside hardening once core flows stabilize.

## Cross-Cutting Concerns
- **Atomicity** — All settings and publish operations must remain single-path replacements; tests should assert no partial reads/writes occur.
- **Concurrency** — Rely on PathSpace’s atomic reads/writes, revision epochs, and wait/notify primitives; avoid external mutexes and validate the data-driven coordination model via tests.
- **GC & retention** — Implement revision pinning and cleanup thresholds (retain ≥3 revisions or ≥2 minutes) early to avoid storage issues.
- **Instrumentation** — Ensure every phase exposes metrics compatible with the frame profiler plan (`output/v1/common`, progressive stats, diagnostics rings).
- **Notifications** — Align dirty markers, wait queues, and auto-render triggers with the scheduling model; cover producer/consumer wakeups in tests.
- **Resources** — Respect per-item residency policies, propagate fingerprints consistently, and validate cache eviction rules with tests.
- **Documentation discipline** — Mirror behavioral changes in `AI_ARCHITECTURE.md`, update diagrams as necessary, and capture CLI/tool scripts.

## Testing Strategy
- Extend `scripts/compile.sh` to cover new targets and maintain the 15× loop with 20 s timeout.
- Add deterministic golden outputs (framebuffers, DrawableBucket metadata) with tolerance-aware comparisons.
- Stress tests: concurrent edits vs renders, present policy edge cases, progressive tile churn, input-routing races, error-path validation (missing revisions, bad settings).
- CI gating: require looped CTest run and lint/format checks before merging.

## Open Questions to Resolve Early
- Finalize `DrawableBucket` binary schema (padding, endianness, checksum) before snapshot/renderer work diverges.
- Decide on minimum color management scope for MVP (srgb8 only vs optional linear FP targets).
- Clarify resource manager involvement for fonts/images in MVP vs deferred phases.
- Confirm threading constraints for macOS presenters (runloop integration, CAMetalLayer adoption).
- Nail down metrics format (`output/v1/common` vs `diagnostics/metrics`) to keep profiler expectations stable.
- Define sequencing for path-traced lighting and tetrahedral acceleration work (post-MVP vs incremental alongside GPU backends).

## Documentation & Rollout Checklist
- Update `README.md` build instructions once UI flags ship.
- Keep `docs/AI_Plan_SceneGraph_Renderer.md` cross-references aligned; link to this implementation plan from that doc and vice versa.
- Add developer onboarding snippets (how to run tests, inspect outputs) to `docs/`.
- Track milestone completion in `AI_TODO.task` or equivalent planning artifact.

## Related Documents
- Specification: `docs/AI_Plan_SceneGraph_Renderer.md`
- Core architecture overview: `docs/AI_ARCHITECTURE.md`

## Maintenance Considerations
- Ensure feature flags allow partial builds (e.g., disable UI pipeline when unsupported).
- Monitor binary artifact sizes for snapshots; consider tooling to inspect revisions.
- Plan for future GPU backend work without blocking MVP—keep interfaces abstract and avoid hard-coding software assumptions.
- Establish guardrails for progressive mode defaults to avoid regressing latency-sensitive apps.
- Respect residency policies and track resource lifecycle metrics so caches stay healthy.
