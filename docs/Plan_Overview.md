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

## Status Snapshot — October 22, 2025
- HSAT (HTML asset codec) remains mandatory and documented; `pathspace_hsat_inspect` regression stays green after the latest loop run.
- HTML tooling quickstart/troubleshooting note now lives in `docs/HTML_Adapter_Quickstart.md` (published October 22, 2025) so incoming maintainers can run the HSAT + HTML harness without rediscovering the workflow.
- Pixel noise perf harness (`examples/pixel_noise_example.cpp`) now drives per-pixel full-surface churn with either backend; `--backend=<software|metal>` selects the renderer, Software2D remains the default, and the helper scripts capture paired baselines under `docs/perf/` (`pixel_noise_baseline.json`, `pixel_noise_metal_baseline.json`). The looped CTests (`PixelNoisePerfHarness`, `PixelNoisePerfHarnessMetal`) invoke `scripts/check_pixel_noise_baseline.py`, which enforces the shared perf budgets (≥50 FPS, ≤20 ms render/present, ≤20 ms present-call) and enables Metal uploads automatically when the baseline calls for it. ✅ (October 23, 2025) Captured a representative frame grab for the harness (`docs/images/perf/pixel_noise.png`) using the new `pixel_noise_example --write-frame=<path>` option so perf regressions include a visual alongside the metrics.
- Default software presents now skip serializing `SoftwareFramebuffer`; the capture path remains opt-in via `capture_framebuffer=true`, keeping production presents on direct IOSurface writes.
- Widget state scenes now publish canonical idle/hover/pressed/disabled snapshots under `scenes/widgets/<id>/states/*`; theme-aware styles (`Widgets::WidgetTheme`) landed and `widgets_example` uses env-selectable palettes without rewriting scenes.
- ✅ (October 23, 2025) Added a renderer/presenter performance guardrail (`scripts/perf_guardrail.py`) that runs the PathRenderer2D benchmark plus pixel noise example, compares against `docs/perf/performance_baseline.json`, writes history under `build/perf/`, and runs from both the pre-push hook and `scripts/compile.sh --perf-report`.
- ✅ (October 23, 2025) Phase 8 widget bindings fuzz harness landed (`tests/ui/test_WidgetReducersFuzz.cpp`), covering randomized pointer/keyboard flows, reducer drains, and republished action queues; monitor follow-on coverage in `docs/Plan_SceneGraph_Implementation.md`.
- ✅ (October 22, 2025) Split the monolithic `ui/Builders.cpp` into focused translation units (Scene/Renderer/Surface/Window/App/Widgets/Diagnostics + shared detail helpers), keeping each under 1 000 lines and revalidating the 15× looped CTest run.
- ✅ (October 23, 2025) Follow-up split trims widget internals further: widget helpers now live in `WidgetDrawablesDetail.inl` and `WidgetMetadataDetail.inl`, with runtime code in `WidgetBuildersCore.cpp`, `WidgetBindings.cpp`, `WidgetFocus.cpp`, and `WidgetReducers.cpp`, keeping the largest TU under 1 000 lines post-refactor.
- ✅ (October 23, 2025) Captured fresh PathRenderer2D FPS traces for 1280×720 and 3840×2160 using `scripts/capture_renderer_fps_traces.py`; results live in `docs/perf/renderer_fps_traces.json` so perf reviews can diff incremental vs full-surface behaviour.

## Supporting Documents
- `docs/AI_Architecture.md` — Core PathSpace architecture reference.
- `docs/AI_Paths.md` — Canonical path namespaces and layout conventions.
- `docs/AI_Todo.task` — Backlog items; keep plans and backlog consistent.
- `docs/AI_Debugging_Playbook.md` — Diagnostics workflow and troubleshooting commands.

## Updating This Index
- When a plan is added/retired or priority shifts, update the list and note the date.
- Keep the top section concise so maintainers can scan priorities quickly.
