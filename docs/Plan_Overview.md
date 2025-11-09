# Handoff Notice

> **Drafted:** October 20, 2025 — overview of PathSpace planning documents for the current post-handoff cycle.

# PathSpace Plans Overview

## Purpose
Provide a single index of active planning documents, ordered by current priority and grouped by theme. Use this as your starting point before diving into any specific plan.

## Priority Ordering (October 21, 2025)

1. **Plan_SceneGraph_Renderer.md**  
   Core rendering/presenter roadmap, snapshot semantics, target contracts. Aligns all UI work.

2. **Plan_SceneGraph_Implementation.md**  
   Execution plan for landing the renderer stack (phases, diagnostics, testing discipline).

3. **Plan_Distributed_PathSpace.md**  
   Network mounting architecture enabling remote PathSpace access; prerequisite for web deployments and cross-host tooling.

4. **Plan_Surface_Ray_Cache.md** (deferred)  
   Future ray-query cache design. Read only when GPU path work resumes.

5. **Plan_PathSpace_Inspector.md**  
   Live web-based inspector for PathSpace; depends on JSON serialization and distributed mounts.

6. **Plan_WebServer_Adapter.md**  
   HTML/web delivery via embedded server, bridging native and browser apps.

7. **Plan_CartaLinea.md** (paused)  
   Cross-app deck/timeline/filesystem concept; re-evaluate once renderer priorities stabilize.

8. **Plan_PrimeScript.md** (research)  
   Exploratory unified scripting/shading language idea; no implementation scheduled.

## Recommended Implementation Focus (Q4 2025)
1. **Plan_SceneGraph_Implementation.md** — active execution path for the renderer/presenter stack defined in `Plan_SceneGraph_Renderer.md`; keep driving the in-flight phases to completion while validating against the renderer blueprint.
2. **Plan_Distributed_PathSpace.md** — begin Phase 0 once the current implementation milestone is stable; web/server work and the inspector depend on it.
3. **Plan_WebServer_Adapter.md** — build the baseline web endpoints (auth, REST, SSE) so downstream tooling has a foundation.
4. **Plan_PathSpace_Inspector.md** — prototype the read-only inspector after distributed mounts, JSON serialization, and web server infrastructure are in place.
5. **Plan_Surface_Ray_Cache.md** — revisit once core rendering + web requirements are satisfied (deferred).
6. **Plan_CartaLinea.md / Plan_PrimeScript.md** — keep paused/research-only until earlier items reach steady state.

## Status Snapshot — November 9, 2025
- ✅ (November 9, 2025) Journal telemetry now computes undo/redo/live byte totals directly from `UndoJournalState::stats`, removing the runtime dependency on replaying snapshot prototypes and paving the way for Phase 4 snapshot code removal.
- ✅ (November 9, 2025) Journal savefiles export/import via `history.journal.v1`; `UndoableSpaceSavefile.cpp` and `UndoSavefileCodec` stream journal entries with retention options, providing parity for undo/redo cursors and dropping snapshot payload packing.
- ✅ (November 9, 2025) Multi-threaded journal stress coverage now runs inside `tests/unit/history/test_UndoableSpace.cpp` (“journal handles concurrent mutation and history operations”), interleaving inserts, takes, undo/redo, and manual garbage-collect commands on four threads. The harness drains and replays the journal to assert cursor stability, covering the Phase 3 stress-test requirement from the Undo Journal rewrite plan.
- ✅ (November 9, 2025) Deterministic fuzz coverage for the undo journal (`tests/unit/history/test_UndoableSpace.cpp`, “journal fuzz sequence maintains parity with reference model”) now exercises random insert/take/undo/redo/garbage-collect sequences against a pure reference model, trimming stacks via telemetry to surface replay drift.
- ⚠️ (November 9, 2025) Snapshot-versus-journal history benchmarking landed via `benchmarks/history/undo_journal_benchmark.cpp` (enable with `-DBUILD_PATHSPACE_BENCHMARKS=ON`): 500-operation runs with 64-byte payloads show snapshot commit/undo/redo latencies of 3.79 s / 6.54 s / 6.53 s vs. journal 10.7 ms / 17.1 ms / 45.6 ms (>350× faster commit, >600× undo throughput). `tests/unit/history/test_UndoableSpace.cpp` still flags telemetry byte mismatches (“journal telemetry matches snapshot telemetry outputs”), so address parity before Phase 4 code removal.
- ✅ (November 8, 2025) Journal persistence now replays mutation logs on enable, restoring live node state and history from `journal.log`. Compaction runs after retention/GC, telemetry reflects disk usage, and the regression `tests/unit/history/test_UndoableSpace.cpp` (“journal persistence replays entries on enable”) covers the load path.
- ✅ (November 8, 2025) Persistence namespaces and encoded roots now pass compile-time token guards and runtime validation, rejecting path traversal tokens before wiring journal or snapshot persistence. Regression coverage lives in `tests/unit/history/test_UndoableSpace.cpp` (“persistence namespace validation rejects path traversal tokens”).
- ✅ (November 8, 2025) Journal telemetry endpoints now serve data via the mutation journal. `_history/stats/*`, `_history/lastOperation/*`, and `_history/unsupported/*` all flow through the new aggregator built on `UndoJournalState::Stats`, with commit-time retention syncing and regression coverage (`tests/unit/history/test_UndoableSpace.cpp`, `tests/unit/history/test_UndoJournalState.cpp`).
- ✅ (November 8, 2025) Journal-backed history roots now honor the `_history/undo|redo|garbage_collect|set_manual_garbage_collect` control paths. Manual retention defers trimming until the GC command runs, and new tests (`tests/unit/history/test_UndoableSpace.cpp`, `tests/unit/history/test_UndoJournalState.cpp`) cover the command routing and retention behaviour. Telemetry endpoints remain in-progress per the Undo Journal rewrite plan.
- ✅ (November 7, 2025) UndoableSpace persistence and unsupported-payload coverage remain solid, and the persistence-format bake-off is resolved: entry/state metadata now emits compact versioned binary headers, recovery paths share a single codec, and the undo tests cover encode/decode round-trips. `pathspace_history_inspect` now provides typed payload decoding, generation diffs, and inspector-ready JSON telemetry (documented in `docs/AI_Debugging_Playbook.md`), and `docs/Plan_PathSpace_UndoHistory.md` captures the new workflow. Savefile export/import helpers (`UndoableSpace::exportHistorySavefile` / `importHistorySavefile`) now emit PSJL bundles with undo/redo retention honored, and the new `pathspace_history_savefile` CLI + debugging playbook entries let editors automate PSJL capture/restore. CLI automation followed through: CTest `HistorySavefileCLIRoundTrip` plus the `pathspace_history_cli_roundtrip` harness keep the binaries honest, the pre-push hook executes the roundtrip check, and `importHistorySavefile` now persists decoded snapshots so no generations go missing. Fresh work (November 7, 2025) extends the telemetry ingestion: `scripts/history_cli_roundtrip_ingest.py` now writes `build/test-logs/history_cli_roundtrip/dashboard.html` alongside `index.json`, rendering undo/redo trends with inline charts and PSJL deep links so Grafana panels and the in-flight inspector UI can consume the feed without custom glue. Next follow-up: upgrade the paint example to mount its buffer under an `UndoableSpace`, exposing `_history/undo` / `_history/redo` controls to showcase interactive history flows.
- ✅ (November 1, 2025) `history::CowSubtreePrototype` landed as the copy-on-write prototype for undo history, with instrumentation/tests in place and `docs/Plan_PathSpace.md` updated; `scripts/run-test-with-logs.sh` now hardens mktemp handling so the 15× loop stays stable; `docs/Plan_PathSpace_UndoHistory.md` captures the layered design, transactions, retention, and persistence roadmap.
- ✅ (October 24, 2025) PathSurfaceMetal now allocates IOSurface-backed textures when `iosurface_backing` is set, keeping Metal surface caching in step with CAMetalLayer presentation and the updated UITest coverage.
- ✅ (October 24, 2025) `./scripts/compile.sh --test` now auto-enables example builds, runs the PixelNoise perf harness (software + Metal) inside the mandated 15× loop, and sets the looped per-test timeout baseline to 20 s so regressions surface without bespoke CTest invocations.
- HSAT (HTML asset codec) remains mandatory and documented; `pathspace_hsat_inspect` regression stays green after the latest loop run.
- HTML tooling quickstart/troubleshooting note now lives in `docs/HTML_Adapter_Quickstart.md` (published October 22, 2025) so incoming maintainers can run the HSAT + HTML harness without rediscovering the workflow.
- Pixel noise perf harness (`examples/pixel_noise_example.cpp`) now drives per-pixel full-surface churn with either backend; `--backend=<software|metal>` selects the renderer, Software2D remains the default, and the helper scripts capture paired baselines under `docs/perf/` (`pixel_noise_baseline.json`, `pixel_noise_metal_baseline.json`). The looped CTests (`PixelNoisePerfHarness`, `PixelNoisePerfHarnessMetal`) invoke `scripts/check_pixel_noise_baseline.py`, which enforces the shared perf budgets (≥50 FPS, ≤20 ms render/present, ≤20 ms present-call) and enables Metal uploads automatically when the baseline calls for it. ✅ (October 23, 2025) Captured a representative frame grab for the harness (`docs/images/perf/pixel_noise.png`) using the new `pixel_noise_example --write-frame=<path>` option so perf regressions include a visual alongside the metrics.
- Default software presents now skip serializing `SoftwareFramebuffer`; the capture path remains opt-in via `capture_framebuffer=true`, keeping production presents on direct IOSurface writes.
- Widget state scenes now publish canonical idle/hover/pressed/disabled snapshots under `scenes/widgets/<id>/states/*`; theme-aware styles (`Widgets::WidgetTheme`) landed and `widgets_example` uses env-selectable palettes without rewriting scenes.
- ✅ (October 23, 2025) Added a renderer/presenter performance guardrail (`scripts/perf_guardrail.py`) that runs the PathRenderer2D benchmark plus pixel noise example, compares against `docs/perf/performance_baseline.json`, writes history under `build/perf/`, and runs from both the pre-push hook and `scripts/compile.sh --perf-report`.
- ✅ (October 23, 2025) Phase 8 widget bindings fuzz harness landed (`tests/ui/test_WidgetReducersFuzz.cpp`), covering randomized pointer/keyboard flows, reducer drains, and republished action queues; monitor follow-on coverage in `docs/Plan_SceneGraph_Implementation.md`.
- ✅ (October 22, 2025) Split the monolithic `ui/Builders.cpp` into focused translation units (Scene/Renderer/Surface/Window/App/Widgets/Diagnostics + shared detail helpers), keeping each under 1 000 lines and revalidating the 15× looped CTest run.
- ✅ (October 23, 2025) Follow-up split trims widget internals further: widget helpers now live in `WidgetDrawablesDetail.inl` and `WidgetMetadataDetail.inl`, with runtime code in `WidgetBuildersCore.cpp`, `WidgetBindings.cpp`, `WidgetFocus.cpp`, and `WidgetReducers.cpp`, keeping the largest TU under 1 000 lines post-refactor.
- ✅ (October 23, 2025) Broke `WidgetDrawablesDetail.inl` into per-widget include shards (common/button/toggle/slider/list) so each inline module remains compact while `WidgetDetail.hpp` still exposes a single entry point.
- ✅ (October 23, 2025) Captured fresh PathRenderer2D FPS traces for 1280×720 and 3840×2160 using `scripts/capture_renderer_fps_traces.py`; results live in `docs/perf/renderer_fps_traces.json` so perf reviews can diff incremental vs full-surface behaviour.

## Supporting Documents
- `docs/AI_Architecture.md` — Core PathSpace architecture reference.
- `docs/AI_Paths.md` — Canonical path namespaces and layout conventions.
- `docs/AI_Todo.task` — Backlog items; keep plans and backlog consistent.
- `docs/AI_Debugging_Playbook.md` — Diagnostics workflow and troubleshooting commands.
- `docs/Plan_PathSpace_UndoHistory.md` — Detailed undo/redo layer design (wrapper, control paths, retention, persistence).

## Updating This Index
- When a plan is added/retired or priority shifts, update the list and note the date.
- Keep the top section concise so maintainers can scan priorities quickly.
