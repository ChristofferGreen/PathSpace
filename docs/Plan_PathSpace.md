# Plan: PathSpace Undo/Redo Support

## Motivation
- Declarative widgets (e.g., `PaintSurface`) need efficient undo/redo without reimplementing history per widget.
- Copying entire subtrees on every edit is too expensive; we need structural sharing with snapshot semantics inside `PathSpace`.

## Objectives
1. Add native undo/redo stacks to `PathSpace`, supporting subtree-scoped history.
2. Provide ordered child enumeration so downstream features (focus controller, inspectors) can traverse widgets without bespoke metadata.
3. Add an event-merging layer that combines widget defaults with scene-level overrides into a single dispatch plan.
4. Allow callers to opt-in to undo history per path (or via an undoable subclass) without affecting unrelated trees.
5. Provide retention controls (entry count, byte budget) and instrumentation for diagnostics.
6. Expose helpers so higher-level features (paint widget, editors) can trigger undo/redo/trim operations easily.

## Technical Approach

### Child Enumeration Helper
- Add `std::vector<std::string> PathSpace::listChildren()` to enumerate child component names at the current iterator, and `PathSpace::listChildren(ConcretePathView subpath)` for arbitrary subtrees.
- Results are ordered using the underlying trie ordering and do not decode payloads. Iterators remain valid even if the tree mutates between calls (names snapshot at invocation).
- Use cases: focus traversal, debugging tools, inspectors, and undo snapshot diagnostics.

### Event Route Merger
- Introduce `PathSpace::RouteMerger`, a helper that takes multiple route namespaces (widget defaults, scene overrides, global policies) and produces a single, ordered dispatch plan.
- Merge handlers by priority order; when duplicates exist, keep the override definition and record the decision under `routes/<widget>/<event>/stats`.
- Support merge modes (`replace`, `append`, `prepend`) specified in the override metadata. The merged result is cached per `(widget,event)` until invalidated by a schema change.
- Expose diagnostics (handler count, overrides applied, last refresh timestamp) for tooling.

### Opt-in API
- Introduce `PathSpace::enableHistory(ConcretePathView root, HistoryOptions opts)` to activate undo/redo for the subtree rooted at `root`.
- Internally wrap the node with a copy-on-write layer: when a child changes, duplicate only the modified branch while sharing the rest.
- Maintain per-root stacks:
  - `undo_stack`: snapshots representing prior states.
  - `redo_stack`: cleared on new edits, repopulated when undo is invoked.

### Snapshot Storage
- Each snapshot stores:
  - Changed node payloads (shared_ptr to immutable blobs).
  - Metadata (timestamp, author, optional description).
- Use weak/shared ownership so unreferenced snapshots free automatically after compaction.

### Operations
- `PathSpace::undo(root, steps = 1)` pops snapshots from `undo_stack`, reapplies them to the live tree, and pushes the current state onto `redo_stack`.
- `PathSpace::redo(root, steps = 1)` pulls from `redo_stack` and pushes the current state onto `undo_stack`.
- `PathSpace::clearHistory(root)` drops both stacks to reclaim memory.
- Provide high-level convenience functions (`PaintSurface::Undo`, etc.) that call these APIs and mark affected widgets dirty.

### Retention & Compaction
- History options include `max_entries`, `max_bytes`, and `compaction_interval`.
- When limits are exceeded, collapse oldest entries into keyframes (e.g., merge many incremental brush strokes into a single snapshot).
- Emit telemetry at `root/history/stats` (entry count, bytes retained, last trim timestamp) and log compaction events.

### Diagnostics & Testing
- Surface metrics for QA: average snapshot size, undo/redo latency, failed operations.
- Add regression tests: rapid stroke insertions, alternating undo/redo cycles, concurrent writers, and nested undoable subtrees.
- Verify integration with `PaintSurface`: stroke history stored under `widgets/<id>/state/history` benefits from automatic snapshots; undo/redo updates `WidgetOp` streams and marks `render/dirty`.

## Open Questions
- Should history metadata store arbitrary user tags (e.g., command names) for better UX?
- How do we expose transaction boundaries so multiple PathSpace edits collapse into a single undo entry?

## Next Steps
1. Prototype copy-on-write storage for a simple subtree, measuring memory impact.
2. Implement child enumeration and the route merger, then update widget/runtime plans to consume them.
3. Design `HistoryOptions` struct and telemetry schema.
4. Update paint widget plan to call the new APIs (already referenced in `docs/Plan_WidgetDeclarativeAPI.md`).
