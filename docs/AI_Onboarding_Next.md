# PathSpace — New AI Onboarding (Post-Atlas)

_Last updated: October 20, 2025 (evening)_

Welcome! This repository just transitioned away from the “Atlas” assistant. The notes below get a fresh AI agent productive quickly while we stabilize the hand-off.

## 1. Immediate Checklist

1. **Sync & Branching**
   - `git fetch origin`
   - Create a topic branch from `origin/master` (`feat/<topic>` or `fix/<topic>` per Conventional Commits).

2. **Read-before-you-touch**
   - `docs/AI_Architecture.md` (legacy, but now annotated with hand-off notes).
   - `docs/Plan_SceneGraph_Implementation.md` — check Phase 7 for the freshly completed Metal streaming work and remaining GPU milestones.
   - `docs/AI_Todo.task` — verify priority ordering and align new work with open items.

3. **Build/Test Baseline**
   ```bash
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
   cmake --build build -j
   ./scripts/compile.sh --test --loop=15 --per-test-timeout 20
   ```

4. **Verify Environment Flags**
   - Export `PATHSPACE_ENABLE_METAL_UPLOADS=1` when exercising the Metal2D streaming path locally; leave it unset for CI/headless runs so tests fall back to software.
   - Use `PATHSPACE_UI_DAMAGE_METRICS=1` only when collecting diagnostics; disable for perf runs.

5. **Quick Test Pass (recommended)**
   ```bash
   ./build/tests/PathSpaceTests
   ./build/tests/PathSpaceUITests
   ./build/tests/PathSpaceUITests --test-case "Metal pipeline publishes residency metrics and material descriptors"
   ```
   The targeted Metal UITest ensures the new streaming path and telemetry stay healthy.

## 2. Current Priorities (Oct 19, 2025)

| Area | Action | Notes |
| --- | --- | --- |
| Metal renderer | Implement the dedicated Metal encoder path so Metal2D skips CPU raster uploads entirely | Track under `docs/Plan_SceneGraph_Implementation.md` Phase 7 follow-ups. |
| Diagnostics | Wire dashboards to consume `textureGpuBytes`, `resourceGpuBytes`, and refreshed residency metrics | Coordinate with tooling owners before schema changes. |
| Software renderer parity | Finish progressive tile parallelism & encode sharding | See `docs/Plan_SceneGraph_Implementation.md` (“Software Renderer FPS Parity”). |
| HTML adapter | Investigate HTML asset serialization & re-run UITests | Current branch: `Renderer::RenderHtml hydrates image assets into output` fails; `output/v1/html/assets` stays empty even though the adapter emits locators (see `docs/Plan_SceneGraph_Implementation.md`). |

## 3. Communication & Handoff Hygiene

- Annotate every modified doc with a short hand-off note (keep the pattern used during this transition).
- Document open questions or blockers in the PR body and, when relevant, in `docs/Plan_SceneGraph_Implementation.md`.
- Use the local pre-push hook (`scripts/git-hooks/pre-push.local.sh`) or `SKIP_LOOP_TESTS=1` only with maintainer approval.

## 4. Reference Index

| Document | Purpose |
| --- | --- |
| `docs/AI_Architecture.md` | Core PathSpace architecture (paths, trie, concurrency). Archived but cross-referenced. |
| `docs/AI_Paths.md` | Canonical path layout and namespace conventions. |
| `docs/Plan_SceneGraph_Renderer.md` | Renderer and presenter plan, including snapshot semantics. |
| `docs/Plan_SceneGraph_Implementation.md` | Phase tracker with latest renderer/diagnostics updates. |
| `docs/AI_Debugging_Playbook.md` | Loop test expectations, log locations, and diagnostics tooling. |
| `docs/AI_Todo.task` | Structured backlog (P1/P2) with acceptance criteria. |

## 5. Ready to Work?

- Confirm the build/test loop passes locally (see the quick test pass above).
- Align your planned work with an entry in `docs/AI_Todo.task` (add one if missing).
- Announce the scope in your PR description and keep doc updates synchronized with code changes. Remember to run `ctest -R HtmlCanvasVerify` when touching the adapter or HTML outputs so the headless replay harness stays green.
- When you spot a gap in test coverage, either add the test immediately or log a follow-up in `docs/Plan_SceneGraph_Implementation.md` / `docs/AI_Todo.task` so the need is visible to the next maintainer.

### Latest Highlights (October 20, 2025)
- Metal streaming remains the top renderer follow-up: `Renderer::TriggerRender` + `PathRenderer2D` stream Metal targets into the cached CAMetalLayer texture, with residency telemetry (`textureGpuBytes`, `resourceGpuBytes`) in CI/dashboards. Keep the Metal UITest (`Metal pipeline publishes residency metrics and material descriptors`) green after renderer edits.
- Widget toolkit bootstrap is underway: button and toggle builders now publish canonical widget scenes, `Widgets::UpdateButtonState/UpdateToggleState` surface dirty events only when state changes, and `examples/widgets_example.cpp` publishes both widgets (toggle defaults to checked via the helper). Next maintainer should continue with slider/list widgets, interaction bindings, and UI gallery coverage per `docs/Plan_SceneGraph_Implementation.md` Phase 8.
- HTML resource loader integration now hydrates bytes end-to-end: the dedicated codec in `include/pathspace/ui/HtmlSerialization.hpp` keeps PathSpace vectors intact and the UITests/doc coverage are green. Keep an eye on legacy Alpaca payloads during migrations and update docs when new asset fields (descriptor fingerprints, cache telemetry) land.
- **Update (Oct 20, 2025 — resolved):** HTML asset hydration now relies on a dedicated `Html::Asset` codec (`include/pathspace/ui/HtmlSerialization.hpp`) plus fresh regression coverage. Run `PathSpaceUITests` (watch the `Html::Asset vectors survive PathSpace round-trip` case) before handing off any renderer/html branches.

Welcome aboard and thank you for keeping the PathSpace docs in sync for the next AI maintainer.
