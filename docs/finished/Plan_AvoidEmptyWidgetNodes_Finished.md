# Plan: Avoid Empty Widget Nodes in PathSpace

## Goal
Prevent creation of empty placeholder nodes (e.g., `/widgets/*/space` with no contents). Nodes should only be created when data is actually written, keeping PathSpace and dumps clean.

## Current issue
- The declarative runtime creates structural nodes (e.g., `space`, `log`, `metrics`) even when they remain empty. Dumps show many empty objects.
- This adds noise, increases dump size, and obscures meaningful content.

## Desired behavior
- Only create a node when writing a value or a non-empty child subtree.
- If a subtree becomes empty (all children removed and no values), remove/prune it.
- Keep schema compatibility: avoid breaking readers, but minimize empty artifacts.

## Work items
1) Runtime writes ✅ (Dec 28, 2025)
   - Guarded widget space creation via lazy `ensure_widget_space_root`; `reset_widget_space` no longer seeds empty nested spaces.
   - If a subtree becomes empty, it is left untouched; no auto-pruning added.
2) Schema/doc update ✅ (Dec 28, 2025)
   - Schema note added that widget space/log/metrics nodes are optional and materialize only when data is written.
3) Exporter/tests ✅ (Dec 28, 2025)
   - Added regression test `tests/ui/test_WidgetEmptyNodes.cpp` to assert widget space is absent until first write.
4) Validation ✅ (Dec 28, 2025)
   - Fixed `ensure_widget_space_root` so repeated writes no longer reinsert a fresh `PathSpace` and wipe previously written widget data.
   - Full mandated loop now green: `./scripts/compile.sh --release --loop=5 --per-test-timeout 20` (logs in `build/test-logs/PathSpaceUITests_All_loop{1..5}of5_20251228-1130*.log`, manifest `build/test-logs/loop_manifest.tsv`).

## Acceptance criteria
- PathSpace no longer creates empty `space/log/metrics` nodes for widgets that don’t use them (no post hoc pruning).
- Dumps shrink and show only nodes with data.
- Tests pass; full looped suite green.
