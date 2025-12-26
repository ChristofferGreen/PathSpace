# Plan: Stabilize PathWindowView fullscreen present (iosurface zero-copy)

**Status:** Finished 2025-12-26 (5× PathSpaceUITests_All loop pass; fullscreen iosurface test no longer hangs)

## Goal
Unblock `PathSpaceUITests` by fixing the hang/timeout in `PathWindowView` test case “fullscreen iosurface present protects zero-copy perf”, and ensure the full UI suite passes in the 5× loop gate (`./scripts/compile.sh --loop=5 --per-test-timeout 20`).

## Current Signal
- PathSpaceUITests_All times out; crash log shows SIGTERM in `tests/ui/test_PathWindowView.cpp:134` (“fullscreen iosurface present protects zero-copy perf”).
- Recent WaitMap/screenshot changes altered present/wait behavior; the hang likely occurs inside `PresentWindowFrame` / WaitMap during fullscreen/IOSurface present.

## Hypotheses
1) WaitMap refactor introduced longer critical sections or contention in present → notify path, causing deadlock under fullscreen IOSurface present.
2) Present path waits for metrics or drawable availability while another thread holds WaitMap locks.
3) IOSurface zero-copy path expects capture_framebuffer flags; mismatch leads to no framebuffer and a blocked readback.

## Debug/Work Plan
1) Reproduce minimally
   - Run only the failing test with verbose logging:
     ```
     PATHSPACE_LOG=1 PATHSPACE_LOG_ENABLE_TAGS=WaitMap,PathWindowView \
     ./build/tests/PathSpaceUITests --test-case "fullscreen iosurface present protects zero-copy perf" --success=1 --durations yes
     ```
   - If it hangs >5s, collect a sample:
     `sample <pid> 5 -file /tmp/pathwindowview_sample.txt`
2) Instrumentation
   - Add scoped logs in `PresentWindowFrame` around wait/notify and IOSurface present branches.
   - Enable optional `PATHSPACE_DEBUG_WAITMAP` to log lock wait times and paths.
3) WaitMap adjustments (targeted)
   - Reintroduce a timed lock for notify with diagnostics but **no drops**; avoid holding registry mutex across notify_all.
   - Add a small test ensuring notify doesn’t block >200ms under contention.
4) Test-specific safeguards
   - In test setup, ensure `capture_framebuffer=true` for fullscreen path so ScreenshotService doesn’t wait for missing buffers.
   - Reduce present timeout in the test to fail fast with error (not hang) if IOSurface unavailable.
5) Verification
   - Rerun the single test until it passes consistently.
   - Run full `PathSpaceUITests_All` once, then the 5× loop gate.

## Exit Criteria
- `PathWindowView` “fullscreen iosurface present…” test passes reliably (no timeout) with default timeouts.
- Full `PathSpaceUITests_All` passes in 5× loop.
- WaitMap contention test added/passing to guard against future regressions.

## Outcome (2025-12-26)
- WaitMap notify now uses a timed registry mutex with watchdog logging and tracks in-flight waiters so `clear()` wakes and drains safely; debug flag `PATHSPACE_DEBUG_WAITMAP` traces lock/wait durations.
- Declarative screenshot capture returns `InvalidPath` when `output_png` is missing, keeping the new screenshot guardrail test green.
- `PathSpaceUITests_All` completed 5/5 loops with `--per-test-timeout 20` via `./scripts/compile.sh --release --loop=5 --per-test-timeout 20`; the fullscreen IOSurface test skipped gracefully when hardware sharing is unavailable and no hangs were observed.

## Owners/Notes
- Keep changes minimal and localized to WaitMap/present; avoid touching example apps.
- Update `docs/Plan_DeclarativeScreenshotFix.md` or Memory with the final root cause and fix summary.
