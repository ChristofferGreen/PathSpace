# Plan: Flatten Declarative Widget Children Structure

## Goal
Eliminate the extra “children/children” nesting in the declarative widget schema so child widgets live directly under a single `children` node. Today `button_column/children/children/children/goodbye_button` appears in dumps because of nested capsules. Target: `button_column/children/goodbye_button`.

## Current layout (problem)
- Each widget has a `children` capsule that also holds housekeeping (log/space/metrics/runtime). The actual child entries live under `children/children`, and each child has its own capsule.
- This produces paths like `.../widgets/button_column/children/children/children/goodbye_button`.
- Dumps are confusing and verbose; traversals require skipping capsule layers.

## Target layout
- Widget path: `.../widgets/<id>/children/<child_id>` directly.
- Housekeeping (log/space/metrics/runtime) moves to separate siblings (e.g., `.../widgets/<id>/runtime/...`) or is scoped under `children_runtime` (not mixed into the child map).
- No “children/children” double layer; one `children` map per widget.

## Work items
1) Schema change
   - Update declarative widget schema to place child widgets directly under `<widget>/children/<child_id>`.
   - Define a separate location for housekeeping (log/metrics/runtime) so it doesn’t occupy the child map.
2) Runtime writes
   - Update widget creation/publish code to write children into the flattened path.
   - Update runtime/trellis code to read children from the new path and write housekeeping to the new sibling location.
3) Migration/compat
   - Provide a transitional reader that can handle old layout if needed (for legacy data/tests), or migrate tests to the new layout.
4) Exporter/tests
   - Adjust tests and any exporters/parsers that assumed the nested layout.
   - Add a regression test that asserts the path for a button child is `.../children/<child>` with no extra layers.
5) Docs
   - Update all relevant docs under `docs/` (e.g., Widget schema/architecture/API guides) to reflect the flattened `children` structure and new housekeeping locations.
6) Validation
   - Run `./scripts/compile.sh --loop=5 --timeout=20`; ensure UI and dumps still render correctly.

## Acceptance criteria
- Child widgets appear directly under one `children` node (e.g., `button_column/children/hello_button`).
- Housekeeping/log/metrics are separated from the child map.
- Dumps reflect the flattened structure without extra “children” layers.
- Tests and full loop pass.
