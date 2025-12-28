# Plan: Flatten Widget Children and Remove Empty Housekeeping

## Goal
Flatten widget children to a single `children/<child_id>` layer, eliminate empty housekeeping nodes (`space/log/metrics/runtime`), and prove it via the button example dump and test loop.

## Current state / problem
- Paths look like `.../button_column/children/children/children/hello_button`.
- Empty `space` nodes are created even when unused.
- Dumps are noisy and harder to read/traverse.

## Desired behavior / scope
- One `children` map per widget: `.../widgets/<id>/children/<child_id>`.
- Housekeeping lives in dedicated siblings (e.g., `.../widgets/<id>/runtime/...`) and is created only when data is written.
- No empty nodes created by default; no filters needed to hide them.

## Work items (status: ✖ not started, ⏳ in progress, ✔ complete)
- ✖ Runtime/schema: write children to `<widget>/children/<child_id>`; move housekeeping out of child map; create housekeeping lazily on write; update schema/path helpers.
- ✖ Consumers: update trellis/presenter/runtime traversal to read flattened children (add legacy shim only if necessary).
- ✖ Tests: regression for flat child paths (no `children/children`), labels present; regression that no `space` node exists when unused, but appears when data is written.
- ✖ Docs: update widget schema/reference to flattened children and optional housekeeping.
- ✖ Validation: build, dump, verify, and run loop.
- ✖ After completion: add a short overview/changelog of what changed and why it fixes nested children/empty housekeeping.

## Validation
- Build: `cmake --build build -j`
- Dump check: `./build/declarative_button_example --dump_json > out.json`
  - Expect `.../widgets/button_column/children/hello_button` (no extra `children` layers).
  - Expect labels present.
  - Expect no empty `space/log/metrics/runtime` nodes when unused.
- Full loop: `./scripts/compile.sh --loop=5 --timeout=20`

## Acceptance criteria
- Child widgets appear directly under one `children` node; no repeated `children` segments.
- Housekeeping nodes are absent unless populated (no empty `space` nodes).
- `out.json` matches expectations; test regressions pass; full loop green.

## Work log (✔/⏳/✖)
- ✖ Initial draft (current).
- ✖ Runtime/schema changes applied.
- ✖ Consumer traversal updated.
- ✖ Regressions added and passing.
- ✖ Validation commands run and passing.
- ✖ Overview/changelog added; plan moved to `docs/finished/Plan_FlattenWidgetChildren_Finished.md`.
