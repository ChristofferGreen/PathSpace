# Plan: Declarative Screenshot Token Slot

## Goal
Make declarative screenshot requests thread-safe without introducing structs/queues by using a single-path slot guarded by a token. The presenter captures on the next eligible frame and writes results back to the same slot. Success is gated on passing the standard test loop.

## Constraints
- PathSpace allows multiple lanes (typed values) on one path; no arrays/struct packing.
- Only one request at a time per window/view; ordering enforced by a token.
- No long-lived locks in the render loop; capture must not stall presenting.
- Preserve existing headless/force-software behaviours and PNG parity checks.

## Tokenized slot protocol (per window/view)
Path: `ui/screenshot/<window>/<view>`
- Token lane: `token` (bool). Held by callers; released after request lanes are written or on error.
- Request lanes (written while holding token):
  - `output_png` (string, required)
  - `capture_mode` (string: `next_present` | `frame_index` | `deadline_ns`)
  - `frame_index` (uint64, optional)
  - `deadline_ns` (uint64, optional steady-clock)
  - `width`, `height` (int, optional; default surface size)
  - `force_software`, `allow_software_fallback`, `present_when_force_software` (bools)
  - `verify_max_mean_error` (double, optional)
  - `armed` (bool) — last write to signal a ready request
- Result lanes (written by presenter; also clears `armed`):
  - `status` (`ok` | `error` | `timeout`)
  - `artifact` (string path), `mean_error` (double, optional), `backend` (string)
  - `completed_at_ns` (uint64), `error` (string on failure)

## Helper: ScopedScreenshotToken
- Acquire: `take<bool>(token_path)` with retry/timeout; initialize to `true` on first use if path is missing.
- Release: inserts `true` in destructor to hand the token back.
- Used by callers before writing request lanes; presenter never touches the token.

## Presenter hook
- Add `MaybeHandleScreenshotSlot(...)` inside `PresentWindowFrame` (or immediately after framebuffer is produced).
- If `armed` is false, return.
- Check trigger:
  - `next_present`: capture current frame.
  - `frame_index`: capture when `present_stats.frame_index >= frame_index`.
  - `deadline_ns`: capture when `now >= deadline` (steady clock).
- Capture with existing framebuffer (hardware or recorded software) via `ScreenshotService::Capture` using `provided_framebuffer` to avoid extra presents.
- Write result lanes and clear `armed`.

## Caller flow (CaptureDeclarative and CLIs)
1) Resolve view, readiness, optional theme; ensure runtimes.
2) Acquire token at `ui/screenshot/<window>/<view>/token`.
3) Write request lanes; set `armed=true` last.
4) Wait/poll for `status` with timeout; on success, return `ScreenshotResult` assembled from result lanes.
5) Token releases on scope exit even if errors occur while writing lanes.

## Edge cases
- If presenter can’t find drawables and `require_present` (implicit when baseline/diff) is true, set `status=error` with message.
- If deadline passes with no frame, mark `status=timeout` and clear `armed` to unblock next requests.
- Ensure software fallback remains available when presenter omits a framebuffer.

## Work items
1) Add `ScopedScreenshotToken` helper (new header/impl in `src/pathspace/ui/screenshot/`).
2) Implement slot read/write utilities for the lanes above (no structs; individual reads/inserts).
3) Hook `MaybeHandleScreenshotSlot` into presenter path; feed current framebuffer to `ScreenshotService::Capture`.
4) Update `CaptureDeclarative` (and screenshot CLI helper) to use the token + slot instead of direct `ScreenshotService::Capture`.
5) Docs: update `WidgetDeclarativeAPI.md` to describe slot/token behaviour and capture modes.
6) Tests: extend `tests/ui/test_DeclarativeScreenshot.cpp` (or helper) with:
   - Token serialization: two threads contending must both complete sequentially (can simulate with two sequential calls).
   - Slot capture success (<5s) for `next_present` mode with deterministic hash.
   - Timeout/error path when deadline is in the past.

## Success criteria
- All declarative screenshot tests pass in loop: `./scripts/compile.sh --loop=5 --timeout=20` (or current suite command) after changes.
- No hangs or token leaks (token lane ends as `true`, `armed` is false after run).
- Captured PNG matches framebuffer parity guard (no `verify_mismatch`).
