# PathSpace — AI Onboarding (Fresh Session)

> **Context update (October 15, 2025):** This onboarding now targets the “Atlas” AI context introduced for the current launch window; adjust legacy workflows to align with this context.
> **Session hand-off (October 17, 2025):** Software renderer incremental paths are healthy (~140 FPS for 64 px brush hints at 4 K) and the zero-copy path now skips redundant IOSurface→CPU copies. Use `./build/benchmarks/path_renderer2d_benchmark [--canvas=WIDTHxHEIGHT] [--metrics]` for per-phase timing (damage diff, encode, progressive copy, publish, present) and `./build/paint_example --debug` to watch live frame stats. Expect FPS to dip as brush history grows because the demo keeps each stroke as a drawable; stroke compositing is logged as optional follow-up in `docs/SceneGraphImplementationPlan.md`. 
> **Session hand-off (October 18, 2025):** CAMetalLayer fullscreen perf is covered by `PathSpaceUITests`, `capture_framebuffer` defaults to off, diagnostics now surface structured `PathSpaceError` payloads under `diagnostics/errors/live`, and the presenter reuses a bounded IOSurface pool so long paint sessions no longer exhaust range groups. Next focus: expand automation (`scripts/compile.sh`, CI docs) to collect UI logs and document the debugging playbook before moving deeper into Phase 6.
> **Session hand-off (October 19, 2025 — late):** PathWindowView now drives the CAMetalLayer presenter path, records `gpuEncodeMs`/`gpuPresentMs`, and example apps simply forward the window’s layer/queue. Keep `PATHSPACE_ENABLE_METAL_UPLOADS=1` gated for manual Metal runs while CI stays on the software fallback. Next focus: add Metal UITest coverage and continue trimming the macOS-specific event pump down to input only.
> **Streaming update (October 19, 2025 — night):** RendererTrigger + PathRenderer2D stream Metal targets directly into the cached CAMetalLayer texture and publish `textureGpuBytes`/`resourceGpuBytes` metrics. The next iteration is the dedicated Metal encoder path so we can bypass CPU raster entirely.
> **Diagnostics toggle (October 19, 2025):** Set `PATHSPACE_UI_DAMAGE_METRICS=1` when you need the incremental renderer’s damage/fingerprint/progressive tile counters (`damageRectangles`, `damageCoverageRatio`, `fingerprint{MatchesExact,MatchesRemap,Changes,New,Removed}`, `progressiveTiles{Dirty,Total,Skipped}`) written under `output/v1/common`. Leave it unset for normal runs to avoid the extra bookkeeping overhead.
> **Encode roadmap (October 19, 2025 — evening):** Dirty-rect hints now coalesce tile-aligned regions automatically; benchmark traces show encode still dominating full-surface work. Next implementation target is to parallelise the encode loop across dirty tiles so multi-core hosts climb past the current ~0.7 FPS full-repaint ceiling at 4K.

This guide bootstraps a brand-new AI assistant so it can work productively in the PathSpace repository without inheriting prior conversational context. Follow these steps at the beginning of every new session.

## 1. Confirm Scope & Branch Context
1. Read `AGENTS.md` (repository root) for the quick workflow checklist.
2. Run `git status` to ensure the working tree is clean, then `git fetch origin` to pick up upstream updates.
3. Identify the active branch (`git branch --show-current`). Create a topic branch from `origin/master` when starting new work.

## 2. Essential Documentation Pass
Review these documents (order matters):
1. `docs/AI_Onboarding.md` (this file) — overview and next steps.
2. `docs/AI_ARCHITECTURE.md` — PathSpace core architecture, concurrency policies, snapshot pipeline.
3. `docs/AI_PATHS.md` — canonical namespace/layout conventions.
4. `docs/AI_Plan_SceneGraph_Renderer.md` — renderer stack plan, snapshot semantics, target contracts.
5. `docs/SceneGraphImplementationPlan.md` — current phase status and outstanding tasks.
6. `docs/DebuggingPlaybook.md` — hands-on troubleshooting steps, log capture workflow, and diagnostics quick reference.
7. Any task-specific plans under `docs/` (e.g., `AI_Plan_CartaLinea.md`) when relevant.

Skim `docs/AI_TODO.task` for open epics/features and cross-check the implementation plan.

## 3. Repository Quick Tour
- `src/` — production code (core PathSpace, UI helpers, layers).
- `include/` — public headers (`pathspace/...`).
- `tests/` — doctest suites. UI-specific tests live under `tests/ui/`.
- `docs/` — architecture plans (this directory).
- `scripts/` — convenience wrappers (`compile.sh`, `create_pr.sh`, etc.).

## 4. Environment Health Check
Run the standard build/test loop before beginning changes:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./scripts/compile.sh --loop=15 --per-test-timeout 20
```
Investigate and resolve failures prior to new development.

> **Metal presenter coverage:** On macOS hosts with Metal GPUs, run `./scripts/compile.sh --enable-metal-tests --test` to compile with `PATHSPACE_UI_METAL=ON` and execute the Metal-enabled UITest (`test_PathWindowView_Metal`). Leave the flag off in CI/headless runs so automation stays on the software fallback.

## 5. Working Practices (AI Session)
- Keep edits ASCII unless the file already uses Unicode.
- Always cite relevant docs in commit messages and PR summaries.
- Run the full compile loop after meaningful code changes (tests-only edits may skip).
- Update documentation alongside code when behavior or contracts change. Use `docs/DebuggingPlaybook.md` to capture new log paths, diagnostics, or repro procedures whenever tooling changes.
- When you notice a missing or thin test, either land the coverage or file the follow-up in `docs/SceneGraphImplementationPlan.md` / `docs/AI_TODO.task` so the gap is tracked for the next session.

## 6. Rapid Context Refresh Commands
Use these when resuming work mid-session:
```bash
git status
git branch --show-current
git diff --stat
ls docs
rg "TODO" -n
```

## 7. Hand-off Notes
Before ending a session, record progress in the relevant plan (e.g., `docs/SceneGraphImplementationPlan.md`) and leave explicit next steps in the conversation or a README snippet for the next AI instance.

> **Quick status snapshot:** the latest build/test pointers, Metal presenter status, and open follow-ups now live in `docs/SceneGraphImplementationPlan.md`.

### Immediate next steps (October 19, 2025 — refreshed)
1. Land the Metal encoder path so Metal2D renders bypass the CPU framebuffer copy; update tests/metrics once streaming is GPU-backed.
2. Verify the shared `LocalWindowBridge` across examples (`paint_example`, `devices_example`) and capture any bridge regressions in UI diagnostics.
3. Fold `PATHSPACE_ENABLE_METAL_UPLOADS` into CI/docs (macOS GPU runners) and extend dashboards to ingest the refreshed residency metrics (`resourceGpuBytes`, `textureGpuBytes`, per-target cache totals).
4. Document the compiled artifact expectations for the new cycle (start with `./scripts/compile_paint.sh` and the 15× loop harness) so the next hand-off can validate quickly.

---
**Need a deeper dive?** Start with `docs/AI_ARCHITECTURE.md` and follow the cross-references. For renderer-specific tasks, consult `docs/AI_Plan_SceneGraph_Renderer.md` and its linked plans.
# Handoff Notice

> **Handoff note (October 19, 2025):** The workflow below captures the outgoing “Atlas” assistant. A fresh AI instance should start with `docs/AI_Onboarding_Next.md`, which summarizes the streamlined entry checklist, current priorities, and contacts. Keep this legacy file for context when reconciling older conversations or artifacts.
