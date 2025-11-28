# PathSpace — AI Onboarding (Fresh Session)

> **Context update (October 15, 2025):** This onboarding now targets the assistant context introduced for the current launch window; adjust legacy workflows to align with this context.
> **Session hand-off (October 17, 2025):** Software renderer incremental paths are healthy (~140 FPS for 64 px brush hints at 4 K) and the zero-copy path now skips redundant IOSurface→CPU copies. Use `./build/benchmarks/path_renderer2d_benchmark [--canvas=WIDTHxHEIGHT] [--metrics]` for per-phase timing (damage diff, encode, progressive copy, publish, present) and `./build/paint_example --debug` to watch live frame stats. Expect FPS to dip as brush history grows because the demo keeps each stroke as a drawable; stroke compositing is logged as optional follow-up in `docs/Plan_SceneGraph.md`. 
> **Session hand-off (October 18, 2025):** CAMetalLayer fullscreen perf is covered by `PathSpaceUITests`, `capture_framebuffer` defaults to off, diagnostics now surface structured `PathSpaceError` payloads under `diagnostics/errors/live`, and the presenter reuses a bounded IOSurface pool so long paint sessions no longer exhaust range groups. Next focus: expand automation (`scripts/compile.sh`, CI docs) to collect UI logs and document the debugging playbook before moving deeper into Phase 6.
> **Session hand-off (October 19, 2025 — late):** PathWindowView now drives the CAMetalLayer presenter path, records `gpuEncodeMs`/`gpuPresentMs`, and example apps simply forward the window’s layer/queue. Keep `PATHSPACE_ENABLE_METAL_UPLOADS=1` gated for manual Metal runs while CI stays on the software fallback. Next focus: add Metal UITest coverage and continue trimming the macOS-specific event pump down to input only.
> **Streaming update (October 19, 2025 — night):** RendererTrigger + PathRenderer2D stream Metal targets directly into the cached CAMetalLayer texture and publish `textureGpuBytes`/`resourceGpuBytes` metrics. The next iteration is the dedicated Metal encoder path so we can bypass CPU raster entirely.
> **Shutdown snapshot (October 20, 2025):** `PathRenderer2DMetal` now renders rects, rounded rects, text quads, and textured images on the GPU, Metal UITests run by default (`./scripts/compile.sh` enables them unless `--disable-metal-tests` is passed), and `paint_example --metal` exercises the GPU pipeline end-to-end. Remaining Metal focus: hook material/shader bindings so GPU encode matches software telemetry, then expand coverage to glyph batches and material-driven pipelines.
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
2. `docs/AI_Architecture.md` — PathSpace core architecture, concurrency policies, snapshot pipeline.
3. `docs/AI_Paths.md` — canonical namespace/layout conventions.
4. `docs/WidgetDeclarativeAPI.md` — declarative runtime workflow (bootstrap, widget helpers, readiness guard, handler/focus/runtime services, testing discipline).
5. `docs/Plan_SceneGraph_Renderer.md` — renderer stack plan, snapshot semantics, target contracts.
6. `docs/Plan_SceneGraph.md` — current phase status and outstanding tasks.
7. `docs/AI_Debugging_Playbook.md` — hands-on troubleshooting steps, log capture workflow, and diagnostics quick reference.
8. Any task-specific plans under `docs/` (e.g., `Plan_CartaLinea.md`) when relevant.

Skim `docs/AI_Todo.task` for open epics/features and cross-check the implementation plan.

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
./scripts/compile.sh --loop=5 --per-test-timeout 20
```
Investigate and resolve failures prior to new development.

> **Metal presenter coverage:** On macOS hosts with Metal GPUs, run `./scripts/compile.sh --enable-metal-tests --test` to compile with `PATHSPACE_UI_METAL=ON` and execute the Metal-enabled UITest (`test_PathWindowView_Metal`). Leave the flag off in CI/headless runs so automation stays on the software fallback.

## 5. Working Practices (AI Session)
- Keep edits ASCII unless the file already uses Unicode.
- Always cite relevant docs in commit messages and PR summaries.
- Run the full compile loop after meaningful code changes (tests-only edits may skip).
- Update documentation alongside code when behavior or contracts change. Use `docs/AI_Debugging_Playbook.md` to capture new log paths, diagnostics, or repro procedures whenever tooling changes.
- Treat `docs/WidgetDeclarativeAPI.md` as the authoritative UI workflow. All new widget or sample work must use the declarative runtime; only touch the legacy imperative builders when explicitly migrating existing consumers.
- Use `SP::UI::Screenshot::CaptureDeclarative` for screenshots instead of hand-written readiness + `ScreenshotService::Capture` blocks. Pass your `ScreenshotRequest::Hooks` through `DeclarativeScreenshotOptions` so overlays/fallback writers keep working while the helper handles readiness, force-publish, and telemetry defaults.
- Legacy builder detections now live under `/_system/diagnostics/legacy_widget_builders/<entry>/*`; keep the counters at zero so we can delete the diagnostics block after the support window wraps (February 1, 2026). The old `LegacyBuilders::ScopedAllow` guard has been removed—every UI entry point now flows through `SP::UI::Runtime::*`, so the only reason those nodes would change is if an external consumer still runs an older binary. Investigate any deltas and update the migration tracker when you confirm the source.
- When you notice a missing or thin test, either land the coverage or file the follow-up in `docs/Plan_SceneGraph.md` / `docs/AI_Todo.task` so the gap is tracked for the next session.

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
Before ending a session, record progress in the relevant plan (e.g., `docs/Plan_SceneGraph.md`) and leave explicit next steps in the conversation or a README snippet for the next AI instance.

> **Quick status snapshot:** the latest build/test pointers, Metal presenter status, and open follow-ups now live in `docs/Plan_SceneGraph.md`.

### Immediate next steps (October 19, 2025 — refreshed)
1. Extend the new Metal encoder (currently rect-only) to cover rounded rects, images, glyph batches, and material/shader binding so Metal2D can stay on-GPU for full scenes; keep residency metrics and UITests in sync.
2. Verify the shared `LocalWindowBridge` across examples (`paint_example`, `devices_example`) and capture any bridge regressions in UI diagnostics.
3. Fold `PATHSPACE_ENABLE_METAL_UPLOADS` into CI/docs (macOS GPU runners) and keep dashboards ingesting the refreshed residency metrics plus new residency ratios/status nodes (`resourceGpuBytes`, `textureGpuBytes`, `diagnostics/metrics/residency/*` — updated October 20, 2025).
4. Document the compiled artifact expectations for the new cycle (start with `./scripts/compile_paint.sh` and the 5× loop harness) so the next hand-off can validate quickly.
5. Connect widget reducers to the new binding helpers (`Widgets::Bindings::Dispatch{Button,Toggle,Slider}`) so UI interactions emit dirty hints and ops under `widgets/<id>/ops/inbox/queue` instead of republishing full scenes.

---
**Need a deeper dive?** Start with `docs/AI_Architecture.md` and follow the cross-references. For renderer-specific tasks, consult `docs/Plan_SceneGraph_Renderer.md` and its linked plans.
# Handoff Notice

> **Handoff note (October 19, 2025):** The workflow below captures the outgoing assistant. A fresh AI instance should start with `docs/AI_Onboarding_Next.md`, which summarizes the streamlined entry checklist, current priorities, and contacts. Keep this legacy file for context when reconciling older conversations or artifacts.
