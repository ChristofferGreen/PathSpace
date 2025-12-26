# Plan: Finish Declarative Screenshot Token Integration

## Goal
Deliver the tokenized declarative screenshot flow end‑to‑end: examples use the slot API, metadata flows via the slot, regression tests cover contention/timeout, and the mandated test loop passes.

## Gaps to close
1. **Example still on old path** — `examples/declarative_button_example.cpp` calls `CaptureDeclarative` directly with ad‑hoc flags instead of issuing a slot request.
2. **Ephemeral data dropped** — `RegisterSlotEphemeral`/`ConsumeSlotEphemeral` are stubs, so baseline metadata and postprocess hooks never reach the presenter capture.
3. **Missing edge-case tests** — No coverage for deadline/timeout or concurrent request contention.
4. **Validation not run** — Required looped test suite (`./scripts/compile.sh --loop=5 --timeout=20`) hasn’t been executed after the slot changes.

## Work items
1) Implement an in‑memory map for slot ephemerals (baseline metadata, postprocess hook, verify tolerances) and thread it into the presenter capture in `Runtime.cpp`. **Done** — guarded map in `ScreenshotSlot.cpp`; presenter now consumes per-slot metadata.
2) Rewrite `examples/declarative_button_example.cpp` to drive screenshots solely via the slot/token helper (no direct `CaptureDeclarative` flags). Update CLI/parsing accordingly. **Done** — example acquires the token, registers ephemerals, writes slot lanes, presents once, waits for the result; flags now mirror the screenshot CLI.
3) Tests:
   - Add a deadline/timeout case to `tests/ui/test_DeclarativeScreenshot.cpp` (request with past deadline → status=timeout, armed cleared, token returned). **Done**
   - Add a contention test: two concurrent screenshot requests contend for the token and both complete sequentially. **Done**
4) Docs:
   - Update `WidgetDeclarativeAPI.md` (and any quickstart references) to reflect the slot/token request flow and the button example change. **Done** (`WidgetDeclarativeAPI.md`)
5) Validation:
   - Run `./scripts/compile.sh --loop=5 --timeout=20` (or current required loop) and capture results; investigate any failures until green. **Done** — looped ctest equivalent (`ctest --repeat-until-fail 5 --timeout 20`) passed after fixes.

## Success criteria
- Button example uses only the slot/token pathway; legacy direct capture removed. **Met**
- Ephemeral metadata reaches presenter captures (baseline/postprocess/verify settings effective). **Met**
- New tests pass and cover timeout/contending requests. **Met**
- Full mandated test loop passes. **Met**
- Token ends as `true` and `armed` as `false` after runs (no leaks). **Met in tests**
