# Plan — Remove `relocateSubtree`

> **Drafted:** December 6, 2025  
> **Owner:** PathSpace core + inspector/UI maintainers  
> **Status:** Completed December 6, 2025 (archived)

> **Archive note (December 6, 2025):** All in-tree callers now move nested widget/inspector spaces via `take<std::unique_ptr<PathSpace>>` + `insert`, the `relocateSubtree` helper has been deleted from `PathSpace`, and CI now treats any new usage of the name as a regression. Out-of-tree consumers must migrate before syncing again.

## Motivation

`PathSpace::relocateSubtree` exists as a structural shortcut: it re-parents entire trie nodes to rename/move subtrees. The long-term architectural goal is to design paths so that every movable entity lives inside its own nested `PathSpace`, making a simple `take<PathSpace>` + `insert<PathSpace>` sufficient. Once all consumers follow that discipline, `relocateSubtree` becomes an unnecessary escape hatch and can be deleted.

## Desired End State

- All features that currently call `relocateSubtree` encapsulate their data under dedicated nested `PathSpace` instances (one movable unit per path). 
- Moving/renaming an entity is implemented via `take<PathSpace>` followed by `insert<PathSpace>` (or equivalent typed operations), leveraging the standard notification semantics.
- No code outside the core manipulates trie child maps or relies on pointer-level re-parenting.

## Current Callers & Data Shapes

1. **Inspector watchlists** (`inspector/InspectorHttpServer.cpp`) — ✅ **Completed December 6, 2025**
   - Paths: active entries live at `/diagnostics/errors/watchlists/<user>/watchlists/<id>/space` and archived entries land under `/diagnostics/errors/watchlists/<user>/watchlists_trash/<timestamped-id>/space`.
   - Content: each watchlist now owns a nested `PathSpace` with `/meta/{id,name,count,created_ms,updated_ms,version}` plus a typed `/paths` vector. The legacy JSON node is migrated in-place and scrubbed once the nested space lands.
   - Relocation: `remove_watchlist` now performs `take<std::unique_ptr<PathSpace>>` on the `/space` node (falling back to legacy strings only if migration fails) and reinserts the nested space at the trash destination. No `relocateSubtree` usage remains for watchlists.

2. **Inspector snapshots** (`inspector/InspectorHttpServer.cpp`) — ✅ **Completed December 6, 2025**
   - Paths: `/inspector/user/<user>/snapshots/<id>/space` now stores the inspector/export/meta leaves, and trash mirrors under `/snapshots_trash/<timestamped-id>/space` preserve the same layout for auditing.
   - Content: the nested `PathSpace` holds `/meta` (the JSON blob with label/note/options/bytes/diagnostics), `/inspector` (the serialized inspector snapshot), and `/export` (the PathSpaceJsonExporter payload). A background migrator rewrites legacy nodes in-place as soon as snapshots are listed or deleted.
   - Relocation: `delete_snapshot_record` performs `take<std::unique_ptr<PathSpace>>` on the `/space` node (migrating first when legacy data remains) and reinserts it under the trash namespace so notifications/version counters follow the standard take+insert semantics.

3. **Declarative UI widgets** (`ui/declarative/Widgets.cpp`) — ✅ **Completed December 6, 2025**
   - Paths: `/apps/<app>/windows/<window>/widgets/<name>/space` now owns `meta`, `state`, `render`, `focus`, `events`, and `panels` leaves while structural children remain under `/children/*`.
   - Content: widget fragments populate `/space/*` exclusively via `WidgetSpacePath`, so handler bindings, dirty flags, and reducer state no longer collide with child widgets or parent metadata.
   - Relocation: `Widgets::Move` performs `take<std::unique_ptr<PathSpace>>` on `<root>/space` and reinserts it under the new widget path before rebinding handlers and dirtying parents.
   - Tests/docs: `tests/ui/test_DeclarativeWidgets.cpp` and `tests/ui/test_DeclarativeTheme.cpp` now assert the `/space` layout, and `docs/AI_Paths.md` documents the nested-space schema for future contributors.

## Outcome — December 6, 2025

- Inspector watchlists, snapshots, and declarative widgets encapsulate their payloads under dedicated `<path>/space` nodes, and each move path performs a `take<std::unique_ptr<PathSpace>>` followed by an `insert` to the destination.
- `PathSpace::relocateSubtree` has been fully removed from `PathSpace.hpp/cpp` (and the sanitized `clean/` mirror); any attempt to reference it now fails to compile, forcing contributors onto the nested-space workflow.
- `WidgetDetail::Move` gained the same take/insert semantics in both runtime trees so UI moves reuse the same notification, dirty-marking, and handler-rebind flow as the inspector features.
- Documentation (this file, `docs/Plan_Overview.md`, and `docs/Memory.md`) now points to the nested-space invariant and the finished status so new contributors do not resurrect the helper.

`rg relocateSubtree` now returns only historical references inside this finished document and `docs/Memory.md`; keep the search wired into CI/tooling to block regressions.

## Migration Strategy

1. **Audit & Model Extraction**
   - For each caller, document the tree structure under the paths being relocated (values, child nodes, nested spaces, diagnostics, waits).
   - Identify whether the subtree can be represented as a dedicated nested `PathSpace` without breaking existing APIs.

2. **Refactor to nested PathSpaces**
   - **Watchlists (completed December 6, 2025):**
     - Nested spaces now live at `/watchlists/<id>/space` (meta counters + vector payloads), `persist_watchlist`/`read_watchlist` honor the schema, and a best-effort migrator rewrites legacy JSON nodes in place.
     - `PathSpace::take<std::unique_ptr<PathSpace>>` landed in `Leaf.cpp`, making the take+insert workflow first-class and allowing `remove_watchlist` to move nested spaces (with a legacy fallback for malformed rows).
     - Docs (`AI_Debugging_Playbook`, inspector plan, Memory) track the new layout so downstream tooling expects `/space` everywhere.
   - **Snapshots (completed December 6, 2025):**
     - Nested spaces under `/snapshots/<id>/space` encapsulate `/meta`, `/inspector`, and `/export`, and the migrator upgrades legacy layouts on read/delete/list.
     - `delete_snapshot_record` now takes the nested space and reinserts it under `/snapshots_trash/<timestamped-id>/space`, so trash records retain the structured payload without touching `relocateSubtree`.
   - **Widgets:**
     - Encapsulate each widget’s state inside a nested PathSpace (e.g., `/apps/<app>/widgets/<name>/space`) and move all child paths (`/meta`, `/state`, handler bindings, descendant widgets) under it.
     - Adjust `WidgetDetail::*` helpers, handler rebind logic, renderer dirty markers, and child-mount code to operate within the nested space.
     - Provide an upgrade path that walks existing widgets, builds the nested structure in-place (possibly via temporary staging paths), and maintains handler consistency.
     - Once nested, `Widgets::Move` becomes a take+insert of the nested space plus metadata updates for parent dirtiness.
   - **Validation:** Ensure diagnostics, metrics, waits, and renderer observers still fire correctly once the hierarchy gains a `/space` layer. Update tests accordingly.

3. **API & Tooling Updates**
   - Update inspector and UI controllers to use the new nested-space layout (path changes, JSON transformations, etc.).
   - Provide migration scripts or auto-migrations for existing state (e.g., a one-time job that takes old watchlist entries and reinserts them into the nested-space layout).
   - Sweep the path-schema docs (`docs/AI_Debugging_Playbook.md`, `docs/Plan_PathSpace_Inspector_Finished.md`, `docs/finished/Plan_WebServer_Adapter_Finished.md`, `docs/Plan_Overview.md`, `docs/Memory.md`, and any other path inventories under `docs/`) to update diagrams/examples that currently show `/watchlists/*` or `/widgets/*` without the nested `/space` layer. Every consumer-facing doc should clearly describe the new layout and the take+insert workflow so future contributors stay aligned.

4. **Testing**
   - Extend unit/integration tests to cover the take+insert move path, ensuring notifications, waits, and diagnostics behave the same as the old `relocateSubtree` call.
   - Add regression tests that simulate concurrent moves to confirm there are no races (multiple clients taking/inserting the same nested space).

5. **Deprecation & Removal**
   - ✅ **December 6, 2025:** `relocateSubtree` carried a deprecation warning for the final release cycle and has now been deleted from `PathSpace` (source + clean mirrors). Any downstream caller must transition to nested spaces before rebasing onto this change.
   - ✅ **December 6, 2025:** Docs (`Memory.md`, `Plan_Overview.md`, finished plans) capture the removal and the required take+insert pattern so future contributors stay aligned.

## Risks & Mitigations

- **State migration complexity:** Moving existing data into nested spaces may require downtime or carefully sequenced take/insert operations. Mitigate with migration tools and backups.
- **Performance regressions:** Take+insert recreates notifications and version counters. Profile high-churn move paths (e.g., widgets) to ensure the new approach is acceptable.
- **Missed callers:** Continue running `rg relocateSubtree` (and code search in downstream repos) during the rollout to ensure no new usages appear.

## Open Questions

1. Should nested `PathSpace` usage be enforced via linting or runtime checks to prevent regressions?
2. Do we need new helper APIs (e.g., `moveNestedSpace(src, dst)`) that simply wrap the take+insert pattern for ergonomics once `relocateSubtree` is gone?
3. How do we handle partial failures (take succeeds, insert fails) during a move? Do we need transactional helpers or compensating actions?

## Follow-ups

1. Keep `rg relocateSubtree` (and downstream code search) wired into CI/tooling so any new call sites are rejected immediately.
