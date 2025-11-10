# Plan: PathSpace Undo/Redo Support

> **Status (November 10, 2025):** The mutation journal rewrite replaced the snapshot-based design described below. This document is retained for historical context. See `docs/finished/Plan_PathSpace_UndoJournal_Rewrite_Finished.md` for the active plan.

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
 - Nested plain PathSpaces mounted under a history-enabled node remain opaque unless they are explicitly wrapped by another undo layer.

### Snapshot Storage
- Each snapshot stores:
  - Changed node payloads (shared_ptr to immutable blobs).
  - Metadata (timestamp, author, optional description) serializes through the shared versioned binary codec; the Alpaca JSON fallback was removed on November 7, 2025.
- Use weak/shared ownership so unreferenced snapshots free automatically after compaction.

### Operations
- `PathSpace::undo(root, steps = 1)` pops snapshots from `undo_stack`, reapplies them to the live tree, and pushes the current state onto `redo_stack`. Exposed to callers via inserts to `/<root>/_history/undo`.
- `PathSpace::redo(root, steps = 1)` pulls from `redo_stack` and pushes the current state onto `undo_stack`. Exposed via `/<root>/_history/redo`.
- `PathSpace::clearHistory(root)` drops both stacks to reclaim memory.
- Provide high-level convenience functions (`PaintSurface::Undo`, etc.) that call these APIs and mark affected widgets dirty, while tooling can also interact with history through the `_history/*` control namespace (e.g., `_history/garbage_collect`, `_history/set_manual_garbage_collect`).
- **Single-root requirement:** We will not coordinate undo across multiple roots. Commands that touch several logical areas must structure their data so the full mutation resides under one history-enabled root (e.g., by routing through a command-log subtree or embedding related metadata alongside the primary payload). `enableHistory` will reject configurations that span multiple roots for a single logical command (duplicate `HistoryOptions::sharedStackKey` values raise an error).

### Retention & Compaction
- History options include `max_entries`, `max_bytes`, `compaction_interval`, and `manual_garbage_collect` (default off; forces callers to trigger durability/retention via control paths or API).
- When limits are exceeded, collapse oldest entries into keyframes (e.g., merge many incremental brush strokes into a single snapshot).
- Emit telemetry at `root/history/stats` (entry count, bytes retained, last trim timestamp) and log compaction events.

### Diagnostics & Testing
- Surface metrics for QA: average snapshot size, undo/redo latency, failed operations.
- Add regression tests: rapid stroke insertions, alternating undo/redo cycles, concurrent writers, and nested undoable subtrees.
- Log unsupported snapshot attempts (nested PathSpaces, tasks/futures, serialization failures) under `<root>/_history/unsupported/*` so the inspector can flag misconfigured nodes; add regression coverage that asserts the path/reason telemetry.
- Verify integration with `PaintSurface`: stroke history stored under `widgets/<id>/state/history` benefits from automatic snapshots; undo/redo updates `WidgetOp` streams and marks `render/dirty`.

## Open Questions
- Should history metadata store arbitrary user tags (e.g., command names) for better UX?
- How do we surface history stats and per-entry metadata to tooling without colliding with existing user paths (control namespace customization, documentation)?

## Status — November 7, 2025
- ✅ `history::CowSubtreePrototype` models copy-on-write subtrees with node/payload sharing, provides memory + delta instrumentation, and is validated by unit tests (`apply clones modified branch only`) exercising node reuse and stack accounting.
- ✅ Updated `scripts/run-test-with-logs.sh` temporary file allocation so the test loop tolerates mktemp collisions during 15× runs.
- ✅ Authored `docs/finished/Plan_PathSpace_UndoHistory_Finished.md` outlining the undo wrapper, `_history/*` control paths, retention, and persistence strategy for design review.
- ✅ (November 6, 2025) `_history/unsupported/*` telemetry now exposes the offending path, reason, timestamp, and occurrence counts for unsupported payload snapshots so the inspector surfaces failures without digging through logs; regression coverage updated to check the new nodes.
- ✅ (November 7, 2025) Reaffirmed that undo-enabled roots must contain only serializable payloads; executions and nested PathSpaces stay outside the history subtree and are reported via `_history/unsupported/*`.
- ✅ (November 7, 2025) Downstream plans (UndoHistory, inspector, paint widget) now reference the binary metadata codec, `_history/stats/*` telemetry, and integration checkpoints so consumers adopt the new persistence format consistently.
- ✅ (November 7, 2025) Shared-stack guard rails live: `HistoryOptions::sharedStackKey` declares an intended shared undo stack, and `enableHistory` rejects duplicate keys so cross-root commands regroup under a single history root or a command-log shim instead of relying on unsupported stack sharing.
- ✅ (November 8, 2025) Added `PathSpace::listChildren()` / `PathSpace::listChildren(ConcretePathView)` to enumerate child component names (including nested `PathSpace` mounts) without decoding payloads; coverage lives in `tests/unit/test_PathSpace_listChildren.cpp`.

## Next Steps
1. ✅ Prototype copy-on-write storage for a simple subtree, measuring memory impact. (Captured by `history::CowSubtreePrototype` and associated tests.)
2. **Required design review:** walk the prototype with PathSpace maintainers to finalize the undo/redo stack API surface, retention policy knobs, and integration boundaries (see `docs/finished/Plan_PathSpace_UndoHistory_Finished.md`). Capture decisions in `docs/AI_Architecture.md` and any affected plan docs.
3. Document how the copy-on-write layer plugs into `PathSpace` proper (stack wiring, notification hooks, multi-root behaviour) and outline migration steps for existing widget callers.
4. Implement the route merger, then update widget/runtime plans to consume it. (Child enumeration landed on November 8, 2025 via the new `PathSpace::listChildren()` APIs.)
5. Design `HistoryOptions` struct and telemetry schema.
6. Update the paint widget plan to call the new APIs (already referenced in `docs/Plan_WidgetDeclarativeAPI.md`).
7. ✅ (November 7, 2025) Run the Alpaca vs. binary metadata bake-off for persistence, adopt the versioned binary codec, and update `docs/AI_Architecture.md`, `docs/finished/Plan_PathSpace_UndoHistory_Finished.md`, and paint/inspector plans to reference the shared format.
8. ✅ (November 7, 2025) Published debugging guidance: `pathspace_history_inspect` now documents on-disk inspection (see `docs/AI_Debugging_Playbook.md`) and this plan includes sample inspector payloads for `_history/stats/*` and `_history/lastOperation/*`.
9. ✅ (November 7, 2025) Document save/load support: landed the PSJL (`history.journal.v1`) savefile codec plus `UndoableSpace::exportHistorySavefile` / `importHistorySavefile` helpers, preserving undo/redo stacks and retention budgets with regression coverage.
10. ✅ (November 7, 2025) Publish CLI + doc workflows for the savefile helpers (`pathspace_history_savefile` wrapping export/import) so editors and recovery guides can automate PSJL round-trips without bespoke scripting.
11. ✅ (November 7, 2025) Wired the savefile CLI into automation: `tests/HistorySavefileCLIRoundTrip` exercises export/import binaries, `pathspace_history_cli_roundtrip` runs from the pre-push hook, and importer persistence now writes every decoded snapshot back to disk so PSJL bundles round-trip cleanly.
12. ✅ (November 7, 2025) Hooked the CLI roundtrip harness into automation: telemetry files (`history_cli_roundtrip/telemetry.json`) and PSJL pairs now land in the artifact directories for every CTest/pre-push run, giving dashboards and inspector tooling a stable feed of bundle hashes + entry counts. `scripts/history_cli_roundtrip_ingest.py` aggregates those artifacts into `build/test-logs/history_cli_roundtrip/index.json` with hash/entry trends and deep-link metadata so inspector/CI consumers can ingest the data without bespoke scraping.
13. ✅ (November 7, 2025) Integrated the aggregated history telemetry into inspector dashboards and tooling: `scripts/history_cli_roundtrip_ingest.py` now serves both `index.json` and `dashboard.html` (charts plus PSJL links), and the pre-push hook publishes the HTML so Grafana + inspector views stay in sync with undo retention metrics.
14. ➡️ Rewrite the declarative UI widgets (see `docs/Plan_WidgetDeclarativeAPI.md`) so builders/layouts publish through the consolidated routing layer and expose history-aware state consistently.
15. ➡️ Once the widget rewrite is complete, promote the paint example (and eventual `PaintSurface` widget) to mount its buffer under an `UndoableSpace`, wiring `_history/undo` / `_history/redo` controls so the demo validates interactive undo/redo paths.


addendum:
maybe use something like https://github.com/shytikov/pragmasevka as the default font
