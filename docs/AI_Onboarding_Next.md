# PathSpace — New AI Onboarding (Post-Atlas)

_Last updated: October 19, 2025_

Welcome! This repository just transitioned away from the “Atlas” assistant. The notes below get a fresh AI agent productive quickly while we stabilize the hand-off.

## 1. Immediate Checklist

1. **Sync & Branching**
   - `git fetch origin`
   - Create a topic branch from `origin/master` (`feat/<topic>` or `fix/<topic>` per Conventional Commits).

2. **Read-before-you-touch**
   - `docs/AI_ARCHITECTURE.md` (legacy, but now annotated with hand-off notes).
   - `docs/SceneGraphImplementationPlan.md` — shows the active renderer roadmap.
   - `docs/AI_TODO.task` — verify priority ordering.

3. **Build/Test Baseline**
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j
   ./scripts/compile.sh --test --loop=15 --per-test-timeout 20
   ```

4. **Verify Environment Flags**
   - Leave `PATHSPACE_ENABLE_METAL_UPLOADS` unset unless you are explicitly testing Metal uploads.
   - Use `PATHSPACE_UI_DAMAGE_METRICS=1` only when collecting diagnostics; disable for perf runs.

## 2. Current Priorities (Oct 19, 2025)

| Area | Action | Notes |
| --- | --- | --- |
| Software renderer parity | Finish progressive tile parallelism & encode sharding | See `docs/SceneGraphImplementationPlan.md` (“Software Renderer FPS Parity”). |
| GPU readiness | Material telemetry & residency now unified; next focus is GPU shading execution path | Track follow-up tasks in `docs/AI_TODO.task`. |
| Diagnostics | Ensure dashboards ingest new `materialDescriptors`, residency metrics, and HTML adapter fidelity metadata | Coordinate with tooling owners before schema changes. |
| HTML adapter | Wire replay harness + pipeline hooks, then land CI/headless verification | See SceneGraph plan Phase 7 for remaining tasks. |

## 3. Communication & Handoff Hygiene

- Annotate every modified doc with a short hand-off note (keep the pattern used during this transition).
- Document open questions or blockers in the PR body and, when relevant, in `docs/SceneGraphImplementationPlan.md`.
- Use the local pre-push hook (`scripts/git-hooks/pre-push.local.sh`) or `SKIP_LOOP_TESTS=1` only with maintainer approval.

## 4. Reference Index

| Document | Purpose |
| --- | --- |
| `docs/AI_ARCHITECTURE.md` | Core PathSpace architecture (paths, trie, concurrency). Archived but cross-referenced. |
| `docs/AI_PATHS.md` | Canonical path layout and namespace conventions. |
| `docs/AI_Plan_SceneGraph_Renderer.md` | Renderer and presenter plan, including snapshot semantics. |
| `docs/SceneGraphImplementationPlan.md` | Phase tracker with latest renderer/diagnostics updates. |
| `docs/DebuggingPlaybook.md` | Loop test expectations, log locations, and diagnostics tooling. |
| `docs/AI_TODO.task` | Structured backlog (P1/P2) with acceptance criteria. |

## 5. Ready to Work?

- Confirm the build/test loop passes locally.
- Align your planned work with an entry in `docs/AI_TODO.task` (add one if missing).
- Announce the scope in your PR description and keep doc updates synchronized with code changes. Remember to run `ctest -R HtmlCanvasVerify` when touching the adapter or HTML outputs so the headless replay harness stays green.
- When you spot a gap in test coverage, either add the test immediately or log a follow-up in `docs/SceneGraphImplementationPlan.md` / `docs/AI_TODO.task` so the need is visible to the next maintainer.

Welcome aboard and thank you for keeping the PathSpace docs in sync for the next AI maintainer.
