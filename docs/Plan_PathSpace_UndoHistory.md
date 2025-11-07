# Plan: PathSpace Undo History Layer

> **Drafted:** November 1, 2025 — evolves the undo/redo effort beyond the copy-on-write prototype by specifying the wrapping layer, transactions, retention, and persistence story.

## Motivation
- Editors frequently mount subspaces that need isolated undo/redo semantics without impacting the entire PathSpace tree.
- Existing widget flows (`take → mutate → insert`) require transaction-aware history to avoid double-counting entries.
- Persistence across crashes or handoffs is desirable, so history must spill to disk while remaining opt-in and bounded.

## Scope
- Applies to subtrees explicitly wrapped in an undo layer; unaffected paths incur zero overhead. Nested plain PathSpaces mounted beneath an undoable layer do not inherit undo automatically.
- Establishes API, lifecycle, and telemetry for history stacks.
- Excludes higher-level UX (e.g., UI affordances) and schema-specific diffing; these can layer on later.
- Cross-root coordination is deliberately out of scope. Callers must colocate all data for a logical command under one history-enabled root (or route through a command-log shim) before enabling history; `enableHistory` will reject configurations that expect shared stacks.

## Objectives
1. Provide an `UndoableSpace` wrapper that inherits `PathSpaceBase` and can be mounted anywhere in a parent PathSpace.
2. Capture mutations via copy-on-write snapshots (reusing `history::CowSubtreePrototype`) while optionally supporting caller-defined batching.
3. Enforce retention via per-root budgets with keyframe compaction, and expose trim telemetry.
4. Persist undo stacks to a configurable temp directory so sessions can recover after a restart.
5. Surface a public API (`enableHistory`, `disableHistory`, `undo`, `redo`, `trimHistory`, `HistoryTransaction`) with clear concurrency guarantees.
6. Publish diagnostics (`/root/history/stats`, `/root/history/lastOperation`) to feed inspector tooling.

## Status — November 7, 2025
- ✅ (November 5, 2025) Persistence layer landed: undo/redo stacks flush snapshots + metadata atomically to disk, state metadata survives restarts, and recovery replays into the backing PathSpace before enabling history. Manual GC now flushes pending writes and trims files; telemetry tracks disk bytes/entries alongside cache residency.
- ✅ (November 6, 2025) Regression coverage now verifies that nodes containing tasks/futures or nested PathSpaces are rejected with descriptive errors, keeping undo stacks untouched when snapshots would be unsafe. Inspector-facing telemetry (`<root>/_history/unsupported/*`) now captures the offending path, reason, timestamp, and occurrence count so failures surface directly in the PathSpace inspector.
- ✅ (November 7, 2025) Execution payload policy locked: tasks/futures remain non-snapshottable, and callers mark undo-exempt subtrees with `HistoryOptions::executionOptOutPrefixes` (also writable via plan docs). Opt-out branches skip snapshot generation, log under `<root>/_history/executionOptOut/*`, and no longer poison undo commits.
- ➡️ Next focus: finalize the persistence format bake-off (Alpaca vs. binary metadata) and document the chosen encoding in `docs/AI_Architecture.md` plus integration guides.

## Architecture Overview
- **Wrapper layer:** `UndoableSpace` stores a pointer to the inner `PathSpaceBase` and overrides mutating methods to run inside `HistoryTransaction` scopes.
- **Default behaviour:** every mutating PathSpace call (`insert`, `take`, `clear`) generates its own undo entry. Redo is cleared whenever a new entry lands.
- **Optional batching:** advanced callers can opt into explicit `HistoryTransaction` scopes to coalesce multiple mutations into a single entry (e.g., script-driven macros). When no explicit scope exists, the wrapper auto-creates a single-operation scope per call.
- **Snapshot backend:** staged mutations become `CowSubtreePrototype::Mutation` objects. Committing a transaction emits a new snapshot (`Snapshot generation++`) and metadata (`timestamp`, `author`, `label`, `bytes`, `nodes`).
- **Stacks:** per-root structures hold undo and redo deques. New mutations clear redo. A per-root mutex guards stack and transaction state.
- **Notifications:** after applying an undo/redo snapshot, the wrapper iterates affected paths and calls `notify` on the inner space to keep observers in sync.

## Persistence Strategy
- Default temp location: `${PATHSPACE_HISTORY_ROOT:-$TMPDIR/pathspace_history}/<space_uuid>/<encoded_root>/`.
- By default every committed history entry flushes to disk before the API returns (`fsync` metadata and snapshot, then `rename` atomically). Library users can opt into buffered persistence by inserting `_history/set_manual_garbage_collect = true`, which defers durable writes until they explicitly call `_history/garbage_collect` or `trimHistory`.
- Each entry generates `<generation>.meta` (JSON or Alpaca) and `<generation>.snapshot` (binary COW data); writes are atomic (`rename` after `fsync`).
- Keep the newest `ram_cache_entries` in memory; older entries load on demand.
- On startup, rebuild stacks from disk if `persist_history=true`, skipping incomplete files.
- `disableHistory` removes on-disk data; periodic sweeper cleans abandoned directories.
- Disk telemetry (`diskBytes`, `diskEntries`) mirrors persisted snapshots/meta and updates after retention or manual GC sweeps.

## Control Surface (Path-based API)
- The undo layer exposes its controls via reserved paths under each history-enabled root.
  - `insert("<root>/_history/undo", n)` — undo `n` steps (default 1 when payload missing/non-numeric). Negative/zero values are ignored.
  - `insert("<root>/_history/redo", n)` — redo `n` steps (same defaulting rules).
  - `insert("<root>/_history/garbage_collect")` — run retention/compaction immediately using the current `HistoryOptions`. Callers trigger this when the subtree is quiescent (e.g., scene minimized, document saved) instead of relying on idle detection heuristics.
  - `insert("<root>/_history/set_manual_garbage_collect", true|false)` — toggles whether durability waits for explicit commands. `false` (default) keeps flush-on-commit; `true` lets callers batch writes and manually trigger `_history/garbage_collect`.
  - New mutations automatically clear the redo stack; the layer updates `_history/stats/redoCount` accordingly.
- Read-only nodes provide state for tooling:
  - `<root>/_history/stats/{undoCount,redoCount,bytesRetained,diskBytes,lastOperation}`
  - `<root>/_history/head/generation` (current snapshot id) and optional per-entry metadata under `<root>/_history/entries/<generation>/…`.
  - `<root>/_history/unsupported/{totalCount,recentCount,recent/<index>/{path,reason,occurrences,timestampMs}}` — inspector-facing log of unsupported payload captures (nested PathSpaces, executable nodes, serialization failures).
  - `<root>/_history/executionOptOut/{totalCount,recentCount,recent/<index>/{path,occurrences,timestampMs}}` — confirms execution payload guardrails are routing tasks/futures out of undo snapshots.
- All interactions use standard PathSpace operations (`insert`, `read`, `listChildren`); no new public API surface is required.

## Retention & Compaction
- `HistoryOptions` include `max_entries`, `max_bytes`, `keep_latest_for`, `max_disk_bytes`, `ram_cache_entries`, and `manual_garbage_collect` (default `false`).
- On push, evict oldest entries until budgets pass; before eviction, optionally compact via replay to produce a single keyframe snapshot.
- Manual `trimHistory(root, predicate)` API enables targeted cleanup (e.g., post-publish). `_history/garbage_collect` exposes the same retention sweep over the control path for library clients that prefer declarative commands.
- `_history/set_manual_garbage_collect = true` defers durability and retention to caller-managed checkpoints; telemetry should reflect the current mode under `_history/stats/manualGcEnabled`.
- Telemetry tracks `entries`, `bytes`, `diskEntries`, `diskBytes`, `trimCount`, `lastTrim`.

## API Surface (Draft)
```cpp
class UndoableSpace : public PathSpaceBase {
public:
    explicit UndoableSpace(std::unique_ptr<PathSpaceBase> inner, HistoryOptions opts);

    auto enableHistory(ConcretePathView root, HistoryOptions opts) -> Expected<void>;
    auto disableHistory(ConcretePathView root) -> Expected<void>;
    auto undo(ConcretePathView root, std::size_t steps = 1) -> Expected<void>;
    auto redo(ConcretePathView root, std::size_t steps = 1) -> Expected<void>;
    auto trimHistory(ConcretePathView root, TrimPredicate pred) -> Expected<TrimStats>;
    auto getHistoryStats(ConcretePathView root) const -> Expected<HistoryStats>;

    class HistoryTransaction {
        // Optional RAII scope for callers that want to coalesce multiple mutations.
    };
};
```
- Options control persistence, budgets, nested-space policy, and whether nested undo layers are allowed. Default behaviour disallows enabling history on a descendant of an undo-enabled subtree; nested plain PathSpaces are treated as opaque payloads (no automatic undo). `HistoryOptions::manual_garbage_collect` mirrors the `_history/set_manual_garbage_collect` control toggle so native callers can opt into deferred durability without sending path commands. `HistoryOptions::executionOptOutPrefixes` lets integrators declare subtrees that hold tasks/futures; those branches are skipped during snapshot capture, recorded under `_history/executionOptOut/*`, and never populate undo stacks.

## Concurrency & Threading
- Per-root mutex protects stacks and persistence writes.
- Underlying `Leaf`/`Node` locking remains unchanged.
- Transactions are thread-affine; nested transactions on the same thread reuse the existing scope to avoid redundant snapshots.
- Undo/redo operations run synchronously on the caller thread; future work could offload snapshot hydration to a dedicated worker.

## Telemetry
- Publish real-time stats under `<root>/history/stats`.
- Record last operation metadata (`type`, `duration_ms`, `entries_before`, `entries_after`) under `<root>/history/lastOperation`.
- Log unsupported snapshot attempts under `<root>/_history/unsupported/*`, including offending path, reason, timestamp (ms since epoch), and rolling occurrence counts so the inspector can highlight misconfigured subtrees.
- Track opt-out hits under `<root>/_history/executionOptOut/*` so tooling can confirm undo-exempt subtrees are working as intended (`totalCount`, `recentCount`, per-entry `{path,occurrences,timestampMs}`).
- Emit structured logs for persistence failures and evictions (guards future inspector integration).

## Open Questions
- **Persistence format bake-off (near term):** Evaluate Alpaca vs. the existing binary metadata for history entries—measure file size, write amplification, and recovery tooling complexity—then lock one format for GA. Document the decision in `docs/AI_Architecture.md` once the comparison lands.
- Cross-root undo is intentionally out of scope. Commands that span multiple logical areas must organize their data beneath a single history-enabled root (or route through a command-log wrapper) so one stack captures the full mutation. Document this constraint in integration guides and enforce it with guardrails in `enableHistory`.
- **Future migration:** Track the C++26 reflection-based serializer rollout; once compilers ship it, plan to replace Alpaca with standard reflection serialization for both in-memory snapshots and on-disk metadata. Capture prerequisites (toolchain support, compatibility shims) before scheduling the migration.

## Execution Plan
1. **Design Review (Required)** — walk this document with maintainers; ratify API, persistence defaults, and telemetry. Update docs/AI_Architecture.md accordingly.
2. **Implement Wrapper Skeleton** — add `UndoableSpace`, transactions, in-memory stacks, COW integration (no persistence yet). Extend tests to cover insert/take/undo/redo flows and transaction batching. ✅ (November 4, 2025) implemented in-tree; persistence/telemetry still pending.
3. ✅ (November 5, 2025) **Retention & Telemetry** — auto-trim budgets, surface `_history/stats/*` and `_history/lastOperation/*`, add manual GC controls, and extend doctest coverage.
4. ✅ (November 5, 2025) **Persistence Layer** — on-disk storage, recovery tests, failure-path logging, and cache policies landed. Benchmarks run inside the 15× loop.
5. **Integration Tasks** — update `Plan_PathSpace.md` next steps, ensure paint widget plan references new APIs, revisit inspector tooling notes.

## Dependencies
- `history::CowSubtreePrototype` (landed).
- `Leaf::in` / `NodeData` serialization details (for payload extraction).
- Telemetry/logging infrastructure (`TaggedLogger`, metrics conventions).

## Deliverables
- Code: `UndoableSpace`, transaction helpers, unit/integration tests, persistence module.
- Docs: updates to `docs/AI_Architecture.md`, `docs/Plan_PathSpace.md`, telemetry schemas, operator quickstart for enabling history.
- Tooling: optional CLI or script to inspect on-disk history directories for debugging.
