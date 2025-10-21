# PathSpace — New AI Onboarding

_Last updated: October 21, 2025 (shutdown refresh)_

Welcome! This repository just transitioned away from a previous assistant. The notes below get a fresh AI agent productive quickly while we stabilize the hand-off.

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
   ./scripts/compile.sh --loop=15 --per-test-timeout 20
   ```

4. **Verify Environment Flags**
   - `./scripts/compile.sh` now enables Metal presenter tests by default (builds with `PATHSPACE_UI_METAL=ON` and runs the Metal UITests). Use `--disable-metal-tests` only when the host lacks a compatible GPU.
   - Manual command line runs still respect `PATHSPACE_ENABLE_METAL_UPLOADS=1`; unset it when you explicitly want the software fallback.
   - Use `PATHSPACE_UI_DAMAGE_METRICS=1` only when collecting diagnostics; disable for perf runs.

5. **Quick Test Pass (recommended)**
   ```bash
   ./build/tests/PathSpaceTests
   ./build/tests/PathSpaceUITests
   ./build/tests/PathSpaceUITests --test-case "PathSurfaceMetal integrates with ObjC++ presenter harness"
   ```
   The targeted Metal UITest ensures the GPU bridge stays healthy after the latest ObjC++ harness updates.

## 2. Current Priorities (October 21, 2025)

| Area | Action | Notes |
| --- | --- | --- |
| Metal renderer | ✅ Completed (October 20, 2025) — material/shader bindings now flow through the shared descriptor cache | `PathRenderer2DMetal` covers rects, rounded rects, text quads, and images (see Phase 7); continue tracking glyph/material parity on the descriptor cache. |
| Diagnostics | ✅ Completed (October 20, 2025) — dashboards consume `textureGpuBytes`/`resourceGpuBytes` plus residency ratios/status under `diagnostics/metrics/residency` | Coordinate with tooling owners before schema changes. |
| Input & hit testing | 🚧 In progress (October 21, 2025) — Phase 5 tests now cover z-ordered multi-hit stacks, clip-aware picking, focus routing, keyboard/gamepad focus helpers, and auto-render wake latency | Next step: finish the interaction scheduling metrics/wait-budget coverage noted in `docs/Plan_SceneGraph_Implementation.md`. |
| Widgets | Phase 8 follow-up: expand widget UITests & focus navigation helpers | Styling/theme hooks now ship via `Widgets::WidgetTheme`; next up is goldens + interaction UITests and the remaining focus-navigation helpers noted in `docs/Plan_SceneGraph_Implementation.md`. |
| HTML tooling | Add HSAT inspection CLI/tests and extend coverage when new asset fields appear | Legacy serializer removed; HSAT is mandatory. |

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

### Latest Highlights (October 21, 2025)
- Widget builders now publish canonical idle/hover/pressed/disabled scenes under `scenes/widgets/<id>/states/*`; live scenes republish automatically when state changes, and doctests cover the new snapshots (October 21, 2025).
- Metal renderer is now material-aware: `PathRenderer2DMetal` consumes shared descriptors via `bind_material`, GPU frames stay in lock-step with software telemetry, and a new blending UITest asserts pipeline parity (`PathRenderer2DMetal honors material blending state`).
- Scene hit testing now returns z-ordered hit stacks (`HitTestResult::hits`) with bounded drill-down via `HitTestRequest::max_results`; doctests cover overlap, clipping, and limit cases (October 21, 2025).
- Widget focus navigation helpers (`Widgets::Focus`) maintain `widgets/focus/current`, toggle widget highlight states across button/toggle/slider/list types using `meta/kind`, and enqueue auto-render events for keyboard/gamepad traversal (October 21, 2025).
- Paint demo ships with a `--metal` flag that selects the Metal2D backend and auto-enables uploads for developers; software remains the default for CI.
- `./scripts/compile.sh` always builds with Metal support enabled and runs the Metal UITests unless `--disable-metal-tests` is passed. This keeps the GPU path green by default on macOS hosts.
- HTML adapter parity tests landed, so DOM and canvas command streams stay lock-step with the renderer geometry.
- Residency metrics are live under `diagnostics/metrics/residency/*`; dashboards now read the published ratios/status fields—extend telemetry when new counters appear.
- Presenter telemetry mirrors into window diagnostics sinks under `windows/<window>/diagnostics/metrics/live/views/<view>/present`, so dashboards can ingest present stats without crawling renderer targets (October 21, 2025).
- Widgets gallery supports keyboard focus (Tab/Shift+Tab) and logs reducer actions on every frame, making it easier to validate bindings without auxiliary tooling (October 21, 2025).
- Hit-test auto-render scheduling now has latency coverage (`tests/ui/test_SceneHitTest.cpp`), ensuring the wait/notify path wakes within the 20–200 ms budget (October 21, 2025).
- Widget binding helpers (`Widgets::Bindings::Dispatch{Button,Toggle,Slider,List}`) emit dirty rect hints, auto-schedule renders, and enqueue ops under `widgets/<id>/ops/inbox/queue` so reducers can react without republishing entire scenes.
- List widget builder (`Builders::Widgets::CreateList`) plus `UpdateListState` and `DispatchList` land with doctest coverage, enabling selection/hover/scroll ops and expanding `widgets_example`.
- `Builders::App::Bootstrap` wires a renderer/surface/window + present policy for a scene in one call, trimming boilerplate in examples/tests (October 21, 2025).
- Reducer helpers (`Widgets::Reducers::ReducePending`/`PublishActions`) drain widget ops into `ops/actions/inbox/queue`; widgets_example seeds a sample action and prints the reducer output.
- Stroke rendering is now a first-class primitive: `DrawCommandKind::Stroke` serializes shared point buffers, `PathRenderer2D` rasterizes polylines, the HTML adapter/replay round-trip stroke data, and `paint_example` emits strokes instead of per-dab rects (October 21, 2025).

## 6. Shutdown Snapshot (October 21, 2025 @ 23:55 UTC)
- Latest commit: `feat(ui): add widget state scenes` (local `fix/metal-present`, unpushed). Builders now author canonical widget state snapshots, live scenes republish when state toggles, and the plan/docs/tests are in sync.
- Validation: `ctest --test-dir build --output-on-failure -j --repeat-until-fail 15 --timeout 20` (15× PathSpaceTests, PathSpaceUITests, HtmlCanvasVerify, HtmlAssetInspect) — all green with Metal presenters enabled.
- Outstanding follow-ups before resuming:
  1. Add widget UITest goldens + interaction sequences (hover/pressed/disabled) and close the testing bullets in `docs/Plan_SceneGraph_Implementation.md` Phase 8.
  2. Implement the remaining widget focus-navigation helpers & state schema docs, then sync `docs/Plan_SceneGraph_Implementation.md`.
- Local worktree still holds edits to `docs/Plan_PrimeScript.md` and `docs/AI_Prompts.md` from prior sessions—confirm intent before publishing.
- Next session checklist:
  1. Branch from `origin/master`, cherry-pick or push the widget state scenes commit, and request review.
  2. Implement widget styling hooks and update examples/tests; rerun the 15× loop and HTML replay harness.
  3. Keep docs/plan/backlog entries synchronized with any follow-up changes.

Welcome aboard and thank you for keeping the PathSpace docs in sync for the next AI maintainer.
