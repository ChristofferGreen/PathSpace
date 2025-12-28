# Plan: Flatten Widget Children and Remove Empty Housekeeping

_Status: Finished (December 28, 2025). Archived for reference._

## Summary
- Child widgets now resolve to the canonical `<widget>/children/<child>` map. `WidgetChildren` flattens legacy `/children/children/*` capsules so traversal and mounts see the same root even on old dumps.
- Housekeeping entries (`space`, `log`, `metrics`, `runtime`) are filtered out of child listings to keep container maps focused on actual widgets.
- Added UITest coverage (`test_WidgetChildren.cpp`) to guard the flattening + housekeeping filters and keep `WidgetChildRoot` aligned with the active child root.
- Widget-space creation stays lazy via the existing guards, preventing empty placeholder nodes when no widget data is written.

## Validation
- `cmake --build build -j`
- `./build/declarative_button_example --dump_json > out.json` (hello/goodbye children appear directly under a single `children` map; no housekeeping nodes in the child list)
- `./scripts/compile.sh --loop=5 --timeout=20`
