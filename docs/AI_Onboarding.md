# PathSpace — AI Onboarding (Fresh Session)

> **Context update (October 15, 2025):** This onboarding now targets the “Atlas” AI context introduced for the current launch window; adjust legacy workflows to align with this context.
> **Session hand-off (October 17, 2025):** Progressive repaint flicker in `./build/paint_example` is fixed and Phase 4 coverage now includes seqlock metrics, multi-target windows, dirty-hint diagnostics, and presenter tests. The outstanding priority is **P0 FPS parity**: 4K software renders still plateau around 13–14 FPS while small windows sustain ~80+ FPS. Take the next slot to parallelize progressive tile fan-out and/or trim copy work, keeping notes in `docs/SceneGraphImplementationPlan.md`. Always rerun `./scripts/compile.sh --loop=15 --per-test-timeout 20` before handing off.

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
6. Any task-specific plans under `docs/` (e.g., `AI_Plan_CartaLinea.md`) when relevant.

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

## 5. Working Practices (AI Session)
- Keep edits ASCII unless the file already uses Unicode.
- Always cite relevant docs in commit messages and PR summaries.
- Run the full compile loop after meaningful code changes (tests-only edits may skip).
- Update documentation alongside code when behavior or contracts change.

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

---
**Need a deeper dive?** Start with `docs/AI_ARCHITECTURE.md` and follow the cross-references. For renderer-specific tasks, consult `docs/AI_Plan_SceneGraph_Renderer.md` and its linked plans.
