# Plan: Lazy Creation of Widget “space” Subtrees

**Status:** Completed — December 29, 2025  
**Outcome:** Widget space roots are created strictly on first write. Capsule mirroring skips seeding `/space` when there is nothing to mirror, runtime writers ensure the root on demand, and regression tests guard the lazy contract.

## Goal
Stop creating empty `/space` subtrees for every widget. Only create `…/space` when data is actually written there (metrics/log/runtime). This removes empty `space` nodes from PathSpace and dumps.

## Completed work
- Guarded capsule mirroring: `WritePrimitives` now returns early (after clearing any existing subtree) when both the primitive set and index are empty, avoiding fresh `/space` creation in no-op mirrors.
- Runtime writes stay on-demand: metric/GPU paths in `PaintSurfaceUploader` call `ensure_widget_space_root` right before writing when a value is missing, so runtime wiring can materialize `/space` without prior seeding.
- Regression coverage: `tests/ui/test_WidgetEmptyNodes.cpp` adds an empty-primitives case to assert that no `/space` child is introduced when mirrors have nothing to write; the original kind-write test continues to cover first-write creation.
- Docs: `Widget_Schema_Reference.md` already notes lazy `space/log/metrics` creation; this finished plan records the implementation details and test guard.

## Validation
- Runtime build refreshed via `cmake --build build -j`.
- Full mandated loop: `ctest --test-dir build --output-on-failure -j --repeat-until-fail 5 --timeout 20` (pass).

## Acceptance criteria (met)
- Widgets without space data no longer gain a `…/space` node (no-op mirrors leave the child list unchanged).
- Widgets that write metrics/log/runtime still create `…/space` on-demand.
- Tests and looped suite pass.
