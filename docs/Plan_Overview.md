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

## Status Snapshot — October 21, 2025
- HSAT (HTML asset codec) remains mandatory and documented; `pathspace_hsat_inspect` regression stays green after the latest loop run.
- Pixel noise perf harness (`examples/pixel_noise_example.cpp`) now drives per-pixel full-surface churn with the software renderer; runs windowed by default (uncapped compute, 60 Hz presents), live-resizes its surface to stay pixel-accurate, and supports `--headless` for pure throughput alongside `--present-refresh=<hz>`/`--report-metrics`/`--report-extended` when you need diagnostics. The looped CTest (`PixelNoisePerfHarness`) now runs the harness headless with deterministic seeding, enforcing ≥50 FPS and ≤20 ms average present/render budgets; baselines are versioned via `--write-baseline` with helpers in `scripts/capture_pixel_noise_baseline.sh` and live under `docs/perf/`.
- High-priority follow-up: drop default framebuffer copies during present—production runs must rely on direct IOSurface writes, keeping the capture path opt-in for debugging (tracked in `Plan_SceneGraph_Implementation.md`).
- Widget state scenes now publish canonical idle/hover/pressed/disabled snapshots under `scenes/widgets/<id>/states/*`; theme-aware styles (`Widgets::WidgetTheme`) landed and `widgets_example` uses env-selectable palettes without rewriting scenes.
- Next widget focus: finish the Phase 8 widget bindings fuzz harness—adjacent dirty-propagation coverage landed on October 21, 2025; keep `docs/Plan_SceneGraph_Implementation.md` updated as the fuzz work progresses.

## Supporting Documents
- `docs/AI_Architecture.md` — Core PathSpace architecture reference.
- `docs/AI_Paths.md` — Canonical path namespaces and layout conventions.
- `docs/AI_Todo.task` — Backlog items; keep plans and backlog consistent.
- `docs/AI_Debugging_Playbook.md` — Diagnostics workflow and troubleshooting commands.

## Updating This Index
- When a plan is added/retired or priority shifts, update the list and note the date.
- Keep the top section concise so maintainers can scan priorities quickly.
