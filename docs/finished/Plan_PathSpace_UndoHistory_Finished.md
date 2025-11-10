# Plan: PathSpace Undo History Layer

> **Status (November 10, 2025):** Historical reference. The live implementation is the mutation journal captured in `docs/finished/Plan_PathSpace_UndoJournal_Rewrite_Finished.md`. Migration steps for legacy snapshot persistence live in `docs/AI_Debugging_Playbook.md` (“Undo journal migration runbook”).

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
7. Provide save/load helpers so an undo-enabled subtree can be exported as a versioned document savefile and restored later without manual path scripting.

## Status — November 7, 2025
- ✅ (November 5, 2025) Persistence layer landed: undo/redo stacks flush snapshots + metadata atomically to disk, state metadata survives restarts, and recovery replays into the backing PathSpace before enabling history. Manual GC now flushes pending writes and trims files; telemetry tracks disk bytes/entries alongside cache residency.
- ✅ (November 6, 2025) Regression coverage now verifies that nodes containing tasks/futures or nested PathSpaces are rejected with descriptive errors, keeping undo stacks untouched when snapshots would be unsafe. Inspector-facing telemetry (`<root>/_history/unsupported/*`) now captures the offending path, reason, timestamp, and occurrence count so failures surface directly in the PathSpace inspector.
- ✅ (November 7, 2025) Guardrail reaffirmed: history-enabled subtrees must contain only serializable payloads. Executions (lambdas, futures, nested PathSpaces) stay outside the undo root; `_history/unsupported/*` remains the enforcement/telemetry mechanism.
- ✅ (November 7, 2025) Persistence metadata now writes a compact binary format (little-endian, versioned) for both state and entry descriptors; recovery/telemetry paths use the shared codec and tests cover encode/decode round-trips. Alpaca JSON is no longer emitted for history metadata.
- ✅ (November 7, 2025) Downstream plans now reflect the binary metadata decision (Plan_PathSpace.md, Plan_PathSpace_Inspector.md, Plan_WidgetDeclarativeAPI.md, Plan_Overview.md); integration callouts point to the versioned codec and shared telemetry surface.
- ✅ (November 7, 2025) Added the `pathspace_history_inspect` CLI for on-disk history inspection, documented its workflow in `docs/AI_Debugging_Playbook.md`, and captured `_history/stats/*` inspector samples below so downstream tooling stays aligned with the binary codec.
- ✅ (November 7, 2025) `pathspace_history_inspect` now decodes serialized payloads (strings, numerics, booleans) with human-readable summaries, provides generation-to-generation diff output, and ships JSON helpers for `_history/stats/*` + `_history/lastOperation/*` so the inspector backend can stream telemetry directly.
- ✅ (November 7, 2025) Savefile export/import helpers landed: `UndoableSpace::exportHistorySavefile` / `importHistorySavefile` author PSJL (`history.journal.v1`, formerly `history.binary.v1`) bundles that preserve undo/redo stacks, retention budgets, and persistence settings; regression coverage exercises round-trip restore flows.
- ✅ (November 7, 2025) `pathspace_history_savefile` CLI wraps export/import flows, derives persistence locations automatically, and the debugging playbook now documents recovery/import steps so editors can script PSJL round-trips without bespoke harnesses.
- ✅ (November 7, 2025) CLI automation landed: `tests/HistorySavefileCLIRoundTrip` guards the export/import binaries, the local pre-push hook runs `pathspace_history_cli_roundtrip`, and `UndoableSpace::importHistorySavefile` now persists decoded journals so round-tripped PSJL bundles retain every generation.
- ✅ (November 7, 2025) Shared-stack guard rails solidified: `HistoryOptions::sharedStackKey` advertises an intended cross-root stack, and `enableHistory` now rejects duplicate keys so callers reorganize under a single undo root or route through a command-log shim before enabling history.
- ✅ (November 7, 2025) CLI roundtrip harness now emits `telemetry.json` (bundle hashes, entry/byte counts) and archives `original.psjl`/`roundtrip.psjl` under each test run’s artifact directory (`history_cli_roundtrip/`). Pre-push + CTest automation surface the telemetry for dashboards/inspector ingestion without manual copying.
- ✅ (November 7, 2025) Inspector/CI ingestion landed: `scripts/history_cli_roundtrip_ingest.py` scans test artifacts, rolls up `history_cli_roundtrip/telemetry.json` entries (plus PSJL bundle paths), and writes `build/test-logs/history_cli_roundtrip/index.json`. The pre-push hook invokes the helper so dashboards and the inspector backend have a stable feed of bundle hashes, entry/byte counts, and direct download links for every run.
- ✅ (November 7, 2025) Inspector UI + CI dashboards now consume `history_cli_roundtrip/index.json`: `scripts/history_cli_roundtrip_ingest.py` gained an `--html-output` mode that writes `build/test-logs/history_cli_roundtrip/dashboard.html` (inline charts + deep links to PSJL/telemetry artifacts), and the pre-push hook publishes the dashboard alongside `index.json` so Grafana panels and the upcoming inspector view render undo history trends out of the box.

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
- Each entry generates `<generation>.meta` (binary little-endian header) and `<generation>.snapshot` (binary COW data); writes are atomic (`rename` after `fsync`).
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
- Options control persistence, budgets, nested-space policy, and whether nested undo layers are allowed. Default behaviour disallows enabling history on a descendant of an undo-enabled subtree; nested plain PathSpaces are treated as opaque payloads (no automatic undo). `HistoryOptions::manual_garbage_collect` mirrors the `_history/set_manual_garbage_collect` control toggle so native callers can opt into deferred durability without sending path commands.
- `HistoryOptions::sharedStackKey` (optional) advertises the caller’s intent to reuse a single undo stack across multiple roots. The guard now rejects duplicate keys so callers either regroup their data beneath a single root or introduce a command-log shim instead of relying on unsupported cross-root stacks.

## Concurrency & Threading
- Per-root mutex protects stacks and persistence writes.
- Underlying `Leaf`/`Node` locking remains unchanged.
- Transactions are thread-affine; nested transactions on the same thread reuse the existing scope to avoid redundant snapshots.
- Undo/redo operations run synchronously on the caller thread; future work could offload snapshot hydration to a dedicated worker.

## Telemetry
- Publish real-time stats under `<root>/history/stats`.
- Record last operation metadata (`type`, `duration_ms`, `entries_before`, `entries_after`) under `<root>/history/lastOperation`.
- Log unsupported snapshot attempts under `<root>/_history/unsupported/*`, including offending path, reason, timestamp (ms since epoch), and rolling occurrence counts so the inspector can highlight misconfigured subtrees.
- Emit structured logs for persistence failures and evictions (guards future inspector integration).

## Tooling & Debugging

### On-disk history inspection CLI
- `pathspace_history_inspect` ships with the core build (`cmake --build build -j`) and inspects persisted history directories (`${PATHSPACE_HISTORY_ROOT:-$TMPDIR/pathspace_history}/<space_uuid>/<encoded_root>`).
- The default output is a human-readable summary (counts, bytes, file coverage); `--json` emits a machine representation with per-generation metadata, disk sizes, and any warnings detected on disk. Use `--no-analyze` to skip loading snapshots when you only need file presence checks.
- `--dump <generation>` walks the snapshot tree for a specific generation and prints each payload with an optional hex preview (`--preview-bytes N`, default 16). Pair this with `PATHSPACE_HISTORY_ROOT=<tmp>` when reproducing issues locally so the tool points at the exact scratch directory a failing run produced.

Example usage:

```bash
cmake --build build -j
./build/pathspace_history_inspect "$PATHSPACE_HISTORY_ROOT/12c0e8d41fb84233/2f252f7061696e74" --json
./build/pathspace_history_inspect "$PATHSPACE_HISTORY_ROOT/12c0e8d41fb84233/2f252f7061696e74" --dump 42 --preview-bytes 32
```

Excerpt of the JSON summary (fields omitted for brevity):

```json
{
  "root": "/tmp/pathspace_history/12c0e8d41fb84233/2f252f7061696e74",
  "totals": {
    "entryCount": 5,
    "metaFileBytes": 960,
    "snapshotFileBytes": 32768,
    "recordedPayloadBytes": 24576
  },
  "entries": [
    {
      "kind": "live",
      "generation": 42,
      "metadataBytes": 8192,
      "timestampIso": "2025-11-07T18:42:15.317Z",
      "stats": {
        "uniqueNodes": 213,
        "payloadBytes": 8192
      }
    }
  ],
  "warnings": []
}
```

### Savefile CLI (`pathspace_history_savefile`)
- Build alongside the inspector tool: `cmake --build build -j` produces both executables.
- Run `./build/pathspace_history_savefile export --root <path> --history-dir <dir> --out <file.psjl>` to capture an undo-enabled subtree into a PSJL (`history.journal.v1`) bundle. The CLI derives the persistence namespace from `<dir>` by default; override with `--namespace` / `--persistence-root` when staging migrations. `--no-fsync` skips durability flushes for faster local experimentation.
- Run `./build/pathspace_history_savefile import --root <path> --history-dir <dir> --in <file.psjl>` to seed a new history directory (created on demand) before reproducing a bug or sharing state with collaborators. Append `--no-apply-options` when you need to preserve local retention budgets instead of the savefile’s stored values.
- Typical round-trip:

```bash
PATHSPACE_HISTORY_ROOT=${PATHSPACE_HISTORY_ROOT:-$TMPDIR/pathspace_history}
ROOT=/doc
UUID=12c0e8d41fb84233
ENCODED=$(printf '%s' "$ROOT" | xxd -p -c256)
DIR="$PATHSPACE_HISTORY_ROOT/$UUID/$ENCODED"

./build/pathspace_history_savefile export --root "$ROOT" --history-dir "$DIR" --out doc.psjl
./build/pathspace_history_savefile import --root "$ROOT" --history-dir "$DIR" --in doc.psjl
```

### Inspector `_history/stats/*` sample
The inspector backend should surface undo telemetry as JSON nodes so the browser UI can display history state alongside live PathSpace data. The sample below reflects a healthy root after several edits; values come directly from the `_history/stats/*` and `_history/lastOperation/*` paths that `UndoableSpace` publishes.

```json
[
  { "path": "/doc/_history/stats/undoCount",          "type": "uint64", "value": 4,  "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/redoCount",          "type": "uint64", "value": 0,  "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/undoBytes",          "type": "uint64", "value": 16384, "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/redoBytes",          "type": "uint64", "value": 0,  "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/liveBytes",          "type": "uint64", "value": 4096,  "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/bytesRetained",      "type": "uint64", "value": 20480, "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/manualGcEnabled",    "type": "bool",   "value": false, "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/trimOperationCount", "type": "uint64", "value": 1,  "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/trimmedEntries",     "type": "uint64", "value": 2,  "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/trimmedBytes",       "type": "uint64", "value": 6144, "timestampMs": 1730985602123 },
  { "path": "/doc/_history/stats/lastTrimTimestampMs","type": "uint64", "value": 1730985599000, "timestampMs": 1730985602123 },
  { "path": "/doc/_history/lastOperation/type",       "type": "string", "value": "insert", "timestampMs": 1730985602123 },
  { "path": "/doc/_history/lastOperation/success",    "type": "bool",   "value": true,   "timestampMs": 1730985602123 },
  { "path": "/doc/_history/lastOperation/durationMs", "type": "uint64", "value": 12,     "timestampMs": 1730985602123 },
  { "path": "/doc/_history/lastOperation/undoCountAfter", "type": "uint64", "value": 4, "timestampMs": 1730985602123 }
]
```

Use the sample when wiring the inspector API and UI so `_history/stats/*` values display with the correct types and timestamps; treat the versioned binary codec (`history.journal.v1`) as the authoritative source for on-disk journals and metadata.

## Open Questions
- **Future migration:** Track the C++26 reflection-based serializer rollout; once compilers ship it, plan to replace Alpaca with standard reflection serialization for both in-memory snapshots and on-disk metadata. Capture prerequisites (toolchain support, compatibility shims) before scheduling the migration.

## Execution Plan
1. **Design Review (Required)** — walk this document with maintainers; ratify API, persistence defaults, and telemetry. Update docs/AI_Architecture.md accordingly.
2. **Implement Wrapper Skeleton** — add `UndoableSpace`, transactions, in-memory stacks, COW integration (no persistence yet). Extend tests to cover insert/take/undo/redo flows and transaction batching. ✅ (November 4, 2025) implemented in-tree; persistence/telemetry still pending.
3. ✅ (November 5, 2025) **Retention & Telemetry** — auto-trim budgets, surface `_history/stats/*` and `_history/lastOperation/*`, add manual GC controls, and extend doctest coverage.
4. ✅ (November 5, 2025) **Persistence Layer** — on-disk storage, recovery tests, failure-path logging, and cache policies landed. Benchmarks run inside the 15× loop.
5. ✅ (November 7, 2025) **Integration Tasks** — updated `Plan_PathSpace.md`, paint widget plan, and inspector documentation to reference the binary metadata codec, `_history/stats/*` telemetry, and downstream integration checkpoints.
6. ✅ (November 7, 2025) **Savefile Export/Import** — designed the PSJL (`history.journal.v1`) save format, added `UndoableSpace::exportHistorySavefile` / `importHistorySavefile` helpers that preserve undo/redo stacks while honoring retention + persistence, backed the flow with regression tests, and surfaced the CLI (`pathspace_history_savefile`) + recovery playbooks so editors can automate captures.
7. ✅ (November 7, 2025) Added CLI automation: CTest `HistorySavefileCLIRoundTrip` plus the `pathspace_history_cli_roundtrip` harness exercise the binaries end-to-end, importer persistence bugs are fixed, and the pre-push script now runs the roundtrip check by default.

## Dependencies
- `history::CowSubtreePrototype` (landed).
- `Leaf::in` / `NodeData` serialization details (for payload extraction).
- Telemetry/logging infrastructure (`TaggedLogger`, metrics conventions).

## Deliverables
- Code: `UndoableSpace`, transaction helpers, unit/integration tests, persistence module, and savefile export/import helpers.
- Docs: updates to `docs/AI_Architecture.md`, `docs/Plan_PathSpace.md`, telemetry schemas, operator quickstart for enabling history, and save/load workflow notes.
- Tooling: optional CLI or script to inspect on-disk history directories for debugging plus savefile authoring/loading commands.
