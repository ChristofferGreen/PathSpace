# PathSpace — SceneGraph Tiled Renderer Plan

Status: Completed (December 31, 2025) — PathRenderer2D now defaults to the tiled renderer (software path) with opt-out via `PATHSPACE_DISABLE_TILED_RENDERER=1` or `PATHSPACE_ENABLE_TILED_RENDERER=0`; dirty diff/replace lands via `TileDirtyTracker` + partial tile clears; telemetry/present metrics are wired and rollout is on by default.
Scope: Renderer core replacement under `src/pathspace/ui` while preserving declarative API and present paths.
Audience: UI/rendering engineers, runtime maintainers, perf/telemetry owners.

## Background
- The current `PathRenderer2D` uses a bucket snapshot model with limited damage sharding and software-first execution. Upcoming UI features (rich widgets, higher DPI, Metal/Vulkan upload path) need finer-grained parallelism, entity-aware dirty tracking, and cache-friendly command storage.
- This plan introduces a tile-bucket renderer with an SoA render-command store, entity-id replacement, and backend-agnostic payload lanes, targeting compatibility with existing declarative runtime entry points (`SceneLifecycle`, `PresentWindowFrame`).

## Goals
- Keep the public renderer surface stable: `PathRenderer2D` (and declarative helpers) remain the call site while delegating to the new engine.
- Parallelize rendering by tile with predictable work units and bbox-clamped draws (no bleed outside command bounds).
- Entity-aware updates: replace commands in place by `entity_id`, recompute buckets only for changed bounding boxes, and dirty only the affected tiles.
- Backend independence: software tile renderer first; keep payload indirection so Metal/Vulkan encoders can attach without changing command serialization.
- Telemetry parity: maintain or extend present/damage metrics (tiles updated, bytes copied, backend flags) under existing diagnostics roots.

## Non-goals (phase 1)
- New widget APIs or schema changes (keep WidgetDeclarativeAPI intact).
- Full SVG or complex vector effects; primitives start with rounded-rect SDF and text SDF.
- Window/compositor overhaul; reuse the current LocalWindowBridge and present policy.

## Architecture Outline
- Command store (SoA): lanes for `bbox`, `z`, `opacity`, `kind`, `payload_handle`, `entity_id`, plus optional `clip`, `color`, `font_run`, `atlas_id`. Map `entity_id -> index` supports replacement/removal. Frame staging buffers enable defrag + batch updates.
- Tile grid: fixed-size tiles (tunable; default 64x64). `TileBucket{small_vector<cmd_id>, z_sorted}`; `tile_dirty` bitset drives worker dispatch. Helper: `for_each_tile(bbox, fn)` without heap allocs.
- Dirty computation: diff old vs new per-entity bbox + payload revision hash. Mark intersecting tiles dirty; drop removed entities; add new ones. Accept legacy `DirtyRectHint` (maps to a synthetic bbox list) for compatibility during migration.
- Backend kernels: software raster for rounded-rect SDF and glyph SDF; all writes clamped to bbox. Payload lane is API-agnostic (variant or opaque handle) so Metal/Vulkan encoders can attach without changing command serialization.
- Threading: reuse the existing thread pool; one task per dirty tile renders into a tile buffer, then tiles stitch into the framebuffer. Progressive copy path retained for present metrics; optional tile-coalesced copy for Metal blits.
- Presenter: unchanged API; propagate tile metrics and backend kind into existing diagnostics nodes; no change to `PresentWindowFrame` or HTML adapter payloads.

## Migration Strategy
1) Land the engine in `src/pathspace/ui/scenegraph/` with an adapter class used internally by `PathRenderer2D`.
2) Flip `PathRenderer2D` to delegate while keeping signatures, errors, and metrics stable; flag defaults on with an opt-out env for legacy soak.
3) Update `SceneLifecycle` and declarative present paths to pass through entity ids and optional dirty hints; maintain compatibility shims for current callers.
4) Keep the opt-out flag available for rollback while the default path runs tiled renderer by default.

## Interfaces and Data Contracts
- Renderer entry: keep `PathRenderer2D::render(RenderParams)` signature; `RenderParams` gains optional `entity_commands` (SoA or POD array) and `legacy_dirty_hints`. Default path auto-converts existing buckets into commands.
- Command payloads: use lightweight POD for SDF rect (`corner_radius`, `fill_color`, `stroke`, `blur_sigma`, `bbox`), text (`glyph_run_handle`, `color`, `bbox`), and reserved `custom` lane for future SVG/paths.
- Entity semantics: `entity_id` is stable per drawable; same id in consecutive frames enables replacement. Missing ids imply removal. Id collisions trigger overwrite with warning metric.
- Telemetry: add `{tilesTotal, tilesDirty, tilesRendered, bytesCopied, backendKind, commandCount, entityReused, entityEvicted}` under existing renderer diagnostics root; keep legacy counters mirrored.
- Paths: renderer state persists under `renderers/<rid>/state/tiled/*`; present outputs stay at `targets/*/output/v1/*`.
- Config toggles: `PATHSPACE_ENABLE_TILED_RENDERER` (defaults on), `PATHSPACE_DISABLE_TILED_RENDERER` (opt-out to legacy renderer), `PATHSPACE_TILED_TILE_SIZE=<px>` (default 64), `PATHSPACE_TILED_MAX_BUCKET=<N>` (smallvec reserve), `PATHSPACE_TILED_MAX_WORKERS=<N>` (cap worker fan-out; 0 → hardware concurrency), `PATHSPACE_TILED_TRACE=1` (emit tile/command stats to log path).

## Milestones
1) Command store + bucket builder
   - Implement SoA storage, entity map, bbox-to-tile overlap helper, and z-sort per tile.
   - Add unit tests for bucket coverage (bbox→tiles correctness), replacement semantics, and smallvec growth limits.
2) Software tile renderer v1
   - CPU kernels for rounded-rect SDF and text glyph SDF; clamp to bbox.
   - Tile worker path with thread pool fan-out and framebuffer stitch; verify determinism vs legacy renderer.
3) Dirty/replace pipeline
   - Diff old/new command sets by entity; tile dirty marking; removal handling.
   - Maintain legacy `DirtyRectHint` entry point for compatibility.
4) Telemetry + present integration
   - Publish tile counts/bytes, backend kind, progressive copy stats alongside existing renderer metrics.
   - Keep `PresentWindowFrame` and HTML adapter outputs unchanged.
5) GPU encoder hooks (optional stretch)
   - Define payload lane contracts for Metal/Vulkan encoders; stub compile toggles.

## Risks and Mitigations
- Metric drift vs. existing renderer: run goldens and perf samples; mirror old counters until replacement is default.
- Bucket churn for highly overlapping content: tune tile size and bucket smallvec growth; add fallback full-surface path when overlap exceeds threshold.
- Thread-pool contention: allow pool injection from caller; default to current pool to avoid extra threads in tests.
- Text shaping latency: keep shaping/atlas path unchanged; only swap draw loop to tile-based execution.
- Bbox precision errors (float vs int): standardize on int32 pixel coords post-layout; clamp to surface bounds before bucketization.
- Memory blowup from large command sets: cap per-tile bucket length; spill to a secondary list with merge step; emit warning metric.
- Metal/Vulkan divergence: keep software path authoritative for tests; gate GPU encoders behind flags with fallbacks.

## Test Plan
- Extend `tests/ui/test_PathRenderer2D.cpp` with tile coverage cases (entity replacement, dirty tiles minimality, bbox clamping).
- Reuse golden frame comparisons and HTML replay to ensure output parity.
- Run `ctest --test-dir build --output-on-failure -j --repeat-until-fail 5 --timeout 20` after integration per AGENTS.md.
- Add micro-bench (benchmarks/ui) comparing FPS and tile occupancy vs legacy renderer at 1080p/4K.
- Fault-injection tests: drop malformed payloads, large bbox, and overlapping max buckets; assert graceful degradation.

### Testing Strategy (do-this-while-building)
- M1: Unit tests for SoA store add/replace/remove, bbox→tile coverage property tests (randomized bboxes), z-sort stability, and bucket overflow handling.
- M2: Golden-frame tests for rect/text scenes at 1x/2x DPI; determinism test comparing tile path vs legacy renderer byte-for-byte; bbox clamp regression where commands must not write outside bounds.
- M3: Dirty pipeline tests: minimal dirty tiles on small moves, entity removal clearing buckets, legacy `DirtyRectHint` adapter parity with old damage heuristics.
- M4: Telemetry tests: metrics nodes populated and increasing; present stats include tile counters; HTML replay unchanged.
- Cross-cutting: fuzz small scenes (≤50 cmds) with random bbox/z/opacity to ensure no crashes and stable output; soak repeat-until-fail loop for flake detection.
- Bench: per-commit microbench targets with thresholds; fail fast if perf regresses beyond agreed budget.

## Deliverables and Exit Criteria
- D1: Engine library in `src/pathspace/ui/scenegraph/` with SoA store, tile buckets, dirty diff, and software backend; unit tests passing.
- D2: `PathRenderer2D` delegates to the tiled engine by default (opt-out via `PATHSPACE_DISABLE_TILED_RENDERER` or `PATHSPACE_ENABLE_TILED_RENDERER=0`); outputs bit-for-bit identical to legacy golden frames at 1080p/4K for core widget scenes.
- D3: Declarative runtime (`SceneLifecycle`, HTML adapter) operates unchanged; present metrics include tile counters.
- D4: Benchmarks show ≥1.5x throughput vs legacy renderer on 4K canvas with mixed widgets; doc updates merged.
- Exit to default: flag removed, CI green (all ui tests, golden comparisons, html replay), perf target met or waived by maintainer.

## Performance Targets and Budgets
- Tile size default 64x64; adjustable via env to 32/128 for profiling.
- CPU budget: software path should sustain 60 FPS at 1080p with 1k drawables of mixed text/rects on 8-core desktop; 30 FPS at 4K as stretch.
- Memory budget: steady-state bucket storage < 2x command store size; per-frame allocations bounded (amortized, arena-backed).
- Latency budget: bucket build < 1 ms for 1k drawables; tile render tasks balanced within 10% of median duration.

## Rollout and Feature Flags
- `PATHSPACE_ENABLE_TILED_RENDERER` — master switch (defaults on).
- `PATHSPACE_DISABLE_TILED_RENDERER` — opt-out to legacy renderer for investigations/rollback; overrides the enable flag when set.
- `PATHSPACE_TILED_TILE_SIZE` — integer pixels.
- `PATHSPACE_TILED_MAX_BUCKET` — cap per tile list before overflow list engages.
- `PATHSPACE_TILED_TRACE` — log tile stats to diagnostics path.
- Soak status: default on with optional opt-out flag; continue monitoring golden frames/perf and keep disable flag as escape hatch during rollout.

## Data Structures (proposed)
- `RenderCommandStore` (SoA):
  - arrays: `bbox[] (int32x4)`, `z[] (int32)`, `opacity[] (float)`, `kind[] (enum)`, `payload[] (opaque)`, `entity_id[] (u64)`, optional `clip[]`, `color[]`, `font_run[]`.
  - maps: `entity_index (flat_hash_map<u64, uint32>)`.
  - staging: `pending_add`, `pending_remove`, `free_list` for defrag.
- `TileGrid`:
  - constants: `tile_w`, `tile_h`, `tiles_x`, `tiles_y`.
  - vectors: `buckets[tiles] -> small_vector<cmd_id>`, `dirty_bitset`.
  - helpers: `bucketize(bbox)`, `clear_dirty`, `reserve_buckets`.
- `TileTaskContext`: per-thread scratch (tile buffer, local command view, glyph cache handle) to avoid allocs.

## Threading Model
- Single producer (main render thread) builds buckets and dirty set.
- Thread pool executes one task per dirty tile; tasks pull commands from store by id, z-sorted.
- Stitch step runs on producer: blit tile buffers into framebuffer or enqueue Metal blits.
- Pool injection: allow caller to pass pool; default to existing renderer pool to keep tests deterministic.

## Integration Points
- `SceneLifecycle` publishes drawable buckets → adapter converts to `RenderCommandStore`.
- `PathRenderer2D` keeps legacy interface; adds path to pass entity ids and optional dirty hints.
- Present/publish paths stay unchanged; only renderer internals swap.
- HTML adapter continues to record commands from software framebuffer; no schema change.

## Example Simplification Plan
- Keep a single minimal “hello world” declarative button example as the canonical sample (`examples/minimal_button_example.cpp` or successor).
- Remove/retire other renderer/UI examples (paint, pixel noise, theme, replay, widgets) once the tiled renderer lands; archive any unique assets/fixtures needed for tests under `tests/data/`.
- Update `CMakeLists.txt` to build only the minimal example when `BUILD_PATHSPACE_EXAMPLES=ON`; ensure pre-push hooks/tests do not expect removed binaries.
- Refresh quickstarts (`docs/WidgetDeclarativeAPI.md`, `Widget_Contribution_Quickstart.md`, onboarding) to point to the minimal example and delete references to the removed samples.
- If HTML/export flows are still needed for regression, replace with a minimal render/export smoke that reuses the button scene.

## Dependency and Compatibility Notes
- Font/shaping stack unchanged (HarfBuzz/FreeType); glyph atlas handles flow through payload lane.
- Metal/Vulkan backends optional; build guards mirror existing `PATHSPACE_UI_METAL` toggles.
- No new path schemas for apps; renderer state paths are additive under `renderers/<rid>/state/tiled/*`.

## Work Breakdown (per milestone)
- M1 (store + buckets): define headers; implement SoA storage; bbox→tile helper; z-sort; entity map; tests for coverage/replacement; docs section updates.
- M2 (software backend): implement SDF rect/text kernels; tile task runner; framebuffer stitch; determinism checks; micro-bench harness.
- M3 (dirty pipeline): entity diffing; tile dirty mark/clear; removal handling; legacy `DirtyRectHint` adapter; tests for minimal dirty sets.
- M4 (telemetry/present): emit tile metrics; integrate with present stats; update diagnostics docs; ensure HTML replay unchanged.
- M5 (GPU hooks, optional): payload contracts; build flags; stub encoder calls; perf smoke if enabled.

**Status 2025-12-30:** Software path skeleton lands in `SceneGraph::SoftwareTileRenderer` (Rect/RoundedRect/TextGlyphs). Text now samples glyph atlases with per-tile clipping to avoid double-blend across tiles. PathRenderer2D now delegates to the tiled renderer behind `PATHSPACE_ENABLE_TILED_RENDERER`, passing frame info and emitting tile metrics. Next steps: telemetry/present parity and rollout defaults.
**Update 2025-12-30 (pm):** `SoftwareTileRenderer` fans out dirty tiles across worker threads (configurable `max_workers`) and tracks tile job/worker stats alongside render timing. Presenter wiring threads PathSurfaceSoftware frame info; PathRenderer2D delegation is soaking behind the env flag. Text rendering still uses the glyph-atlas clip path.
**Update 2025-12-30 (late):** Dirty/replace pipeline wired: `TileDirtyTracker` diffs entity ids and legacy `DirtyRectHint`s, `SoftwareTileRenderer` now preserves prior linear buffers and clears only dirty tiles (including removals). Tile overrides drive partial re-renders while untouched tiles reuse prior frame output.
**Update 2025-12-31:** Telemetry/present parity landed: tile renderer stats (tiles total/dirty/rendered, tile jobs/workers, tile size, tiled path used) flow from PathRenderer2D into `PathWindowPresentStats`, are written to `/output/v1/common` and diagnostics trees, and surface in DiagnosticsRuntime readers. Flags still gate the tiled path; rollout remains TODO.
**Update 2025-12-31 (late):** GPU encoder hooks added: `SoftwareTileRenderer` now exposes per-tile command views via `TileEncoderHooks`, providing bbox/z/opacity/kind/payload/entity data and frame metadata for future Metal/Vulkan encoders. Hooks are opt-in and noop by default to keep soak runs stable while GPU backends wire up.
**Update 2025-12-31 (night):** Retired legacy UI demos in favor of `minimal_button_example`; BUILD_PATHSPACE_EXAMPLES now builds only that target, helper scripts/pre-push smoke tests follow it, and docs/size guardrails were refreshed accordingly.
**Rollout 2025-12-31 (final):** Tiled renderer is the default software path. Set `PATHSPACE_DISABLE_TILED_RENDERER=1` or `PATHSPACE_ENABLE_TILED_RENDERER=0` to force the legacy renderer during investigations. Plan ready to archive after soak stability confirms no regressions.

## Worklog (⚪ todo · 🟡 in‑progress · 🟢 done)
- 🟢 M1: Command store + bucket builder
- 🟢 M2: Software tile renderer v1 — `SoftwareTileRenderer` buckets active commands, renders Rect/RoundedRect/TextGlyphs into a software surface per tile, encodes to staging, parallelizes dirty tiles across worker threads (capped by `max_workers`), accepts frame info, and now runs via PathRenderer2D behind `PATHSPACE_ENABLE_TILED_RENDERER` while publishing tile metrics.
- 🟢 M3: Dirty/replace pipeline — `TileDirtyTracker` diffs entity ids + `DirtyRectHint`s to produce tile overrides; `SoftwareTileRenderer` clears and re-renders only dirty tiles while reusing prior linear buffers so removals/replacements stay localized.
- 🟢 M4: Telemetry + present integration
- 🟢 M5: GPU encoder hooks (optional) — `TileEncoderHooks` emits per-tile, z-sorted command views plus frame metadata for GPU backends; opt-in to keep software soak unchanged.
- 🟢 Examples: simplified to a single `minimal_button_example`, removed legacy demo targets from the build, and retargeted helper scripts/pre-push smoke tests to the minimal sample.
- 🟢 Docs/tests: refreshed README/onboarding/debugging docs and size guardrails to match the new example footprint; legacy demo references noted as archival.
- 🟢 Rollout: Tiled renderer defaults on (software path) with legacy fallback via `PATHSPACE_DISABLE_TILED_RENDERER` or `PATHSPACE_ENABLE_TILED_RENDERER=0`; plan archived.

## Lessons Learned (to fill as we go)
- Template: **Date — Owner — What we assumed / What happened / Adjustment**
- Capture surprises early (tile size, entity-id collisions, bucket overflow behaviour, perf regressions, CI flake modes) so defaults/flags and docs evolve with evidence.
- Keep entries short; link to logs/bench traces when useful.

## Postmortem (to be written after rollout)
- Single narrative block capturing timeline, decisions, reversals, metrics vs targets, and remaining debt.
- Include: scope vs execution, major incidents/flakes, perf deltas vs budgets, what shipped vs deferred, and whether the flag flipped to default.
- Close with follow-up actions and ownership for any lingering gaps.

## Documentation Checklist
- Update `docs/AI_Architecture.md` renderer references to mention tiled engine default-on state and opt-out flags.
- Add brief operator notes to `docs/WidgetDeclarativeAPI.md` if any caller-visible toggles surface.
- Record perf findings under `docs/perf/` with tile-size sweeps.
- Keep `ReleaseNotes_Q4_2025.md` (or current release notes) in sync now that the tiled path is default-on.

## Open Questions
- Default tile size per backend (software vs Metal/Vulkan) and whether to adapt to DPI.
- Do we expose entity ids to higher-level widgets for hit-testing reuse, or keep them renderer-internal?
- Should the damage heuristic fold into the new dirty pipeline or remain separate for now?
- Should command store live per-target or globally shared across targets of a renderer?
- Do we allow partial transparency ordering optimizations (tile-level z min/max) in phase 1 or defer?
