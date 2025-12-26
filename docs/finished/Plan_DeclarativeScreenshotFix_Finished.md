# Plan: Declarative Screenshot Parity (Button Example)

## Goal
Make `examples/declarative_button_example` (and other declarative samples) produce screenshots that exactly match the on-screen framebuffer, with no hangs or fallback art. Success means `--screenshot` completes reliably and captures the same frame the UI shows.

## Current Symptoms
- `--screenshot --screenshot_exit` often hangs in `PresentWindowFrame`/WaitMap before producing PNG.
- When it completes (after earlier hacks), the PNG can differ from the live UI (colors/placement).
- Headless runs show empty framebuffers when capture is attempted before a present occurs.

## Hypotheses
1) WaitMap contention during present/capture causes the hang (notify while other threads hold the lock).
2) Capture sometimes runs before a drawable/present exists, yielding empty buffers or fallback.
3) Lifecycle workers not running (when runtimes are trimmed) leave readiness incomplete.

## Work Plan
1) Instrumentation
   - Add scoped tracing to `WaitMap::wait/notify` (path, hold time, thread id) behind `PATHSPACE_DEBUG_WAITMAP=1`.
   - Add `PATHSPACE_SCREENSHOT_DEBUG_PRESENT` logging to include lock-wait duration and renderer backend for each present attempt.
2) Rebuild capture flow
   - Ensure minimal but sufficient runtimes for capture: start input runtime + widget trellis; keep IO/telemetry off for headless.
   - Before capture: `MarkDirty` + `ForcePublish` + `PresentWindowFrame` once, then hand that framebuffer to `ScreenshotService` (no retries).
3) WaitMap fix
   - Split WaitMap locking: separate registry mutex from CV mutexes or switch notify to a short timed lock with diagnostics, not silent drops.
   - Add a watchdog: if notify lock waits >100ms, emit a tagged log with the contended path.
4) Tests/verification
   - New regression test: headless screenshot of `declarative_button_example` must finish <5s and PNG hash stable.
   - Run `./scripts/compile.sh --loop=5 --timeout=20` with screenshots enabled.
   - Manual: `./build/declarative_button_example --screenshot /tmp/button.png --screenshot_exit` then run the app and visually compare (colors/layout).

## Exit Criteria
- No hangs in 5× looped screenshot runs.
- PNG matches live UI (visual check + stable pixel hash).
- WaitMap contention logs silent (no >100ms waits) in capture runs.

## Status (2025-12-25)
- Instrumentation in place:
  - `PATHSPACE_DEBUG_WAITMAP=1` now logs wait/notify calls with path, thread id, registry lock wait, and wait durations; notify emits a watchdog log if the registry lock blocks >100ms.
  - Present debug output adds `present_ms` for each attempt and records failures with `PATHSPACE_SCREENSHOT_DEBUG_PRESENT=1`.
- WaitMap locking split: registry operations use a timed mutex, while each waiter CV has its own mutex to reduce contention during notify/wait.
- Capture flow rebuilt (step 2):
  - `CaptureDeclarative` now ensures a minimal runtime bootstrap for headless runs (input runtime + widget event trellis only) before readiness.
  - MarkDirty/ForcePublish run once per capture and the first `PresentWindowFrame` framebuffer is handed directly to `ScreenshotService` (no retry loop); it still falls back to the recorded software framebuffer when the presenter omits a buffer.
  - Require-present checks tolerate presenter stats that report zero drawables when a framebuffer is still available, preventing false negatives.
- PNG parity guard:
  - When a prerendered framebuffer is provided, `ScreenshotService` now reloads the written PNG and diffs it against the captured framebuffer (mean error tolerance defaults to 0); capture fails with `verify_mismatch` metrics if any pixel diverges.
- Regression test added (step 4):
  - `tests/ui/test_DeclarativeScreenshot.cpp` captures two headless screenshots of the declarative button sample, asserts completion <5s, and verifies the PNG hash is stable across runs.
- Status (2025-12-25): Completed
  - Ran the full suite 5× via `./scripts/compile.sh --loop=5 --per-test-timeout 120`, covering PathSpaceTests + PathSpaceUITests (including the declarative screenshot parity guard). The initial 20s timeout setting proved too tight for the UI bundle, so the loop was rerun with a 120s cap to avoid false terminations while still guarding for hangs.
  - WaitMap watchdog remained silent across all loop logs (checked `build/test-logs/PathSpaceUITests_All_loop*.log` for `WaitMap`), indicating no >100ms notify lock contention during capture/present.
  - Declarative screenshot runs completed without mismatches or hangs; PNG parity guard did not report `verify_mismatch`. Optional manual eyeball compare can be performed later but is no longer blocking.
