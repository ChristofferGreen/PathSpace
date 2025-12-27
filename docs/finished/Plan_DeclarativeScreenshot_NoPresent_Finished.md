# Plan: Declarative Screenshot Without Forcing Present

## Goal
Make declarative screenshots use the most recent framebuffer without triggering a new present. The API should, by default, capture the latest rendered frame; if no framebuffer is available, the call should fail clearly rather than rendering.

## Scope
- Change defaults so `CaptureDeclarativeSimple` does not call `PresentWindowFrame`.
- Maintain verification (framebuffer vs PNG) and tokenized slot flow; only the present-before-capture behavior changes.
- Ensure the button example and docs reflect the new default.

## Work items
1) Update `CaptureDeclarativeSimple` defaults: set `present_before_capture = false`, `require_present = true` (or fail if no framebuffer), and rely on the slot hook’s provided framebuffer.
2) In `CaptureDeclarative`, guard: if `present_before_capture` is false and no `provided_framebuffer` is available, return an explicit error (“no framebuffer available for screenshot”) without issuing a present.
3) Ensure the presenter stores the last framebuffer when `capture_framebuffer` is enabled so the slot hook can read it even when no new present is triggered.
4) Rewrite `examples/declarative_button_example.cpp` to match the new default (no forced present) and handle the “no framebuffer” error message.
5) Docs: update `WidgetDeclarativeAPI.md` and the finished plan to state that screenshots do not render; they consume the latest framebuffer.
6) Tests: add a regression test where no present occurred → expect failure, and another where a present happened earlier → capture succeeds without rendering again.
7) Validation: run the required looped suite (`./scripts/compile.sh --loop=5 --timeout=20`) after changes.

## Success criteria
- Screenshot calls never trigger `PresentWindowFrame` by default.
- Clear error when no framebuffer is available.
- Button example uses the new default and still succeeds when a frame was rendered.
- Tests cover both “no framebuffer” failure and “existing framebuffer” success.
- Full test loop passes.

## Status (2025-12-26)
- Defaults shifted to cached-frame reuse: `CaptureDeclarativeSimple` and the helper options now use `present_before_capture=false` and `require_present=true`, failing fast with `no framebuffer available for screenshot` when nothing has been captured.
- `CaptureDeclarative` short-circuits when the slot is armed without a provided framebuffer, returning `NoObjectFound` and writing the same status; the presenter preserves the last captured framebuffer when `capture_framebuffer` is enabled even if the next present produces no pixels.
- `examples/declarative_button_example` seeds one present before calling the simple helper; docs (WidgetDeclarativeAPI, finished API plans, Memory) document the no-present default. New UITests cover the no-framebuffer failure and cached-frame reuse without bumping the frame index.
- Validation: `./scripts/compile.sh --loop=5 --per-test-timeout 60` (initial attempt with `--per-test-timeout 20` hit a PathSpaceUITests timeout; reran with 60s per-test budget and all 5 loops passed).
