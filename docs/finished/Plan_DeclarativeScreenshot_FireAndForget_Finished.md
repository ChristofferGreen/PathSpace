# Plan: Declarative Screenshot Fire-and-Forget Simple API

> **Status:** Finished December 26, 2025. `CaptureDeclarativeSimple` is now a fire-and-forget scheduler (void), `RunOptions::run_once` exits after one frame for `--screenshot_exit`, UITests cover the async slot + no-present timeout path, and docs/sample usage were updated.

## Goal
Make `CaptureDeclarativeSimple` a pure “schedule and return” helper. It only arms the screenshot slot and immediately returns (no present, no wait, no result). The presenter hook captures on the next rendered frame. The button example stays one line for screenshots and still calls `RunUI`.

## Scope and rules
- Public surface:
  - `CaptureDeclarativeSimple` becomes `void` and only enqueues slot values.
  - `CaptureDeclarative` remains the advanced, fallible API for synchronous captures/tests.
- Capture happens in the presenter hook (already present): it checks the armed slot each present and writes the framebuffer of that present.
- No presents or waits inside the simple helper. Errors surface via the slot status, not the return type.
- Example must be one line for scheduling and must call `RunUI`; `--screenshot_exit` runs only long enough to hit the first frame.

## Detailed work items
1) API change
   - In `DeclarativeScreenshot.hpp/.cpp`, change `CaptureDeclarativeSimple` to `void`.
   - Implementation: acquire token, write slot request (`capture_mode=next_present`, sizes, output path, require_present=true, verify flags, theme if supplied), and return. Do not call `PresentWindowFrame`, `ForcePublish`, or wait for results.
   - Consider adding an optional `theme_override` parameter? (only if needed; otherwise keep minimal params).
2) Slot/result behavior
   - Ensure presenter hook already writes `status`, `artifact`, `error` on capture; no changes unless missing.
   - Simple helper should not clear/override results; just arm.
3) Button example
   - Parse `--screenshot`/`--screenshot_exit` as today.
   - Call `CaptureDeclarativeSimple(...)` (single line) before `RunUI`.
   - Call `RunUI`; when `--screenshot_exit` is set, run only long enough to process one frame/present then exit (add a `run_once`/`single_frame` option or equivalent flag handling).
   - Remove all manual present/capture_framebuffer toggles from the example.
4) RunUI support for “one frame then exit”
   - Add a small option to `RunOptions` (e.g., `run_once` or `max_frames=1`) and implement early exit after the first present. Default keeps current behavior.
   - Use that option in the example when `--screenshot_exit` is passed.
5) Tests
   - Keep synchronous validation using `CaptureDeclarative` (advanced API) for hashing/PNG checks.
   - Add a test that:
     - Calls `CaptureDeclarativeSimple` (now void), starts the UI loop for one frame, and then asserts the PNG exists and matches the expected hash.
     - Another test ensures slot status reports error if no frame ever presents (e.g., no RunUI).
   - Update any call sites expecting a result from the simple helper.
6) Docs
   - Update `WidgetDeclarativeAPI.md` to describe the fire-and-forget behavior and that errors are reported via slot status, not return.
   - Update finished plans to match the new simple API contract.
7) Validation
   - Run `./scripts/compile.sh --loop=5 --timeout=20` after changes and capture results.

## Rationale
- Today the “simple” API still drives readiness/present and returns a result, which violates the single-line, zero-side-effects expectation for a screengrab scheduler.
- Fire-and-forget simplifies CLI and examples, avoids double-presents, and makes capture parity with “whatever was just rendered” logically unavoidable.

## Detailed acceptance criteria
- API surface: only two public calls remain:
  - `CaptureDeclarativeSimple`: `void`, enqueue-only, non-blocking, no present, no waits.
  - `CaptureDeclarative`: unchanged advanced, synchronous, fallible API.
- Scheduling semantics:
  - Simple call only writes slot + token; presenter hook captures on next present.
  - Errors are recorded in slot status (`status`, `error` lanes) by the presenter; simple caller never throws/returns an error.
- Example flow:
  - One-line schedule in `examples/declarative_button_example.cpp`.
  - `RunUI` always called. With `--screenshot_exit`, UI runs exactly one frame (or until first present completes) then exits.
- Tests:
  - Advanced path still hashes PNG and covers failure cases.
  - New test for simple path: schedule, run one frame, assert PNG exists and status=`captured`/`match`.
  - New test for “no present” path: schedule without running UI → status=`timeout`/`error` in slot.
- Docs updated to reflect scheduling-only semantics and slot-based error reporting.
- Full looped test suite (`./scripts/compile.sh --loop=5 --timeout=20`) passes.

## Before / After (simple API)
| Aspect | Current | Target |
| --- | --- | --- |
| Signature | `Expected<ScreenshotResult> CaptureDeclarativeSimple(...)` | `void CaptureDeclarativeSimple(...)` |
| Side effects | Forces present/readiness, may block/wait, returns error | Writes slot only, no presents, non-blocking |
| Error channel | Return value + exceptions | Slot status (`status`, `error`) written by presenter |
| Example code | Multi-line, manual presents in sample | One line schedule; `RunUI` drives capture |

## Data flow (target)
1) Simple call writes: token, output/baseline/diff/metrics, capture_mode=next_present, size, require_present, verify flags, armed=true.
2) RunUI renders a frame → presenter hook sees armed slot → `ScreenshotService::Capture` writes PNG → slot status/artifact/error updated.
3) If `--screenshot_exit`, UI loop terminates after first present + slot completion.

## Migration / compatibility
- Existing callers expecting a returned `ScreenshotResult` from the simple API must migrate to:
  - Use advanced `CaptureDeclarative` if they need synchronous results, or
  - Poll the slot status/artifact for the asynchronous path.
- Update tests and docs accordingly; version the change in release notes.

## Rollout steps
1) Implement API signature change and enqueue-only behavior.
2) Add `RunUI` single-frame option and wire the button example.
3) Update presenter status writes if gaps exist.
4) Update docs and release notes.
5) Update/extend tests.
6) Run full test loop and fix regressions.

## Risks and mitigations
- Risk: Callers silently assume immediate PNG; mitigation: doc and test coverage; keep advanced API synchronous.
- Risk: Slot status not consumed → silent failures; mitigation: ensure presenter writes clear status/error and example logs it.
- Risk: Missing present → no capture; mitigation: `run_once` UI path in sample ensures at least one present when exit flag is used.

## Open questions
- Do we need a tiny helper to read slot status for CLI feedback? (Nice-to-have.)
- Should `RunUI` expose `max_frames` vs `run_once`? (Pick one for consistency.)

## Success criteria (final)
- Simple API is void, enqueue-only, no blocking/presents.
- Button example is one-line schedule + `RunUI` (single-frame when requested).
- Presenter captures next frame and reports status via slot.
- Tests cover async (simple) and sync (advanced) paths.
- Full test loop green.
