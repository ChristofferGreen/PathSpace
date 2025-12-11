# PathSpace History Layer Wiring

> **Status:** December 9, 2025 — documents how the old copy-on-write wrapper and the current mutation journal share the same stack plumbing inside `SP::History::UndoableSpace`. Use this note together with `docs/AI_Architecture.md` for high-level policy and `docs/finished/Plan_PathSpace_UndoJournal_Rewrite_Finished.md` for the full implementation history.

## Stack Plumbing
- `UndoableSpace` inherits `PathSpaceBase` and wraps an inner `PathSpace`. `enableHistory()` canonicalizes the requested root, validates guardrails, and stores a `UndoJournalRootState` (components, retention options, persistence paths) in `journalRoots` so subsequent mutations can look up the owning history stack.
- Every mutating call goes through the overridden `in()` / `out()` methods. They resolve the affected history root, capture the "before" payload via `captureJournalNodeData`, execute the inner operation, and record the new payload through `recordJournalMutation`. `JournalTransactionGuard` keeps mutations atomic so interleaved inserts/takes cannot partially update the journal.
- Control paths live under `<root>/_history`. Inserts into `_history/undo`, `_history/redo`, `_history/garbage_collect`, `_history/set_manual_garbage_collect`, and `_history/set_tag` are intercepted by `handleJournalControlInsert`, which runs the appropriate journal operation instead of touching the backing tree.
- `UndoableSpace::notify` delegates to the inner space, so any waiters registered against `<root>/*` or `<root>/_history/**` continue to receive notifications after the wrapper records telemetry or applies undo/redo steps. Nothing in the wrapper short-circuits the `NotificationSink` tokens emitted by the inner `PathSpace`.
- `visit()` and `listChildrenCanonical()` also forward to the inner tree so tooling (inspector, dump-json CLI) can traverse the logical children without being aware of the wrapper.

## Control Paths, Telemetry, and Notifications
- Telemetry paths are stable and rooted at `_history`. `UndoHistoryUtils::Paths::*` enumerates the locations for stats (`_history/stats/*`), last-operation fields (`_history/lastOperation/*` including the new `_history/lastOperation/tag`), generation counters, and unsupported payload logs. Reading these paths goes through `readHistoryStatsValue`, which enforces type expectations and surfaces the latest journal-derived counters.
- Control nodes are regular inserts, which means any caller that previously issued `_history/undo` or `_history/redo` commands against the copy-on-write layer can keep doing so. Waiters looking at `_history/stats/*` continue to trigger because the wrapper writes the updated counters back into the same canonical nodes.
- Undo/redo persistence happens transparently when `HistoryOptions::persistHistory` is set. The wrapper encodes the root path, builds a persistence directory, and keeps `_history/stats/manualGcEnabled`, `_history/stats/trimmedEntries`, and disk metrics in sync so automation has the same observability surface as before the journal rewrite.
- `_history/unsupported/*` entries are raised directly from the journal transaction when a payload cannot be serialized (nested PathSpaces, lambdas, futures, or custom objects without metadata). Each entry logs the offending path, reason, and occurrence count so dashboards and inspector panels can highlight integration issues without guessing at code paths.
- `_history/stats/limits/*` mirrors the finalized `HistoryOptions`: `maxEntries`, `maxBytesRetained`, `keepLatestForMs`, `ramCacheEntries`, `maxDiskBytes`, `persistHistory`, and `restoreFromPersistence` now publish as first-class nodes alongside the aggregate `HistoryLimitMetrics` struct. Inspector dashboards no longer need to infer budgets from savefiles, and declarative widgets can bind telemetry cards directly to the limit nodes.
- `_history/stats/compaction/{runs,entries,bytes,lastTimestampMs}` aliases the existing trim counters so tooling can present compaction health without hard-coding the legacy field names. The historical `_history/stats/trim*` nodes remain for backward compatibility.
- Diagnostics mirror (December 10, 2025): history telemetry is exposed read-only under `/diagnostics/history/<encoded-root>/` with a compatibility mirror at `/output/v1/diagnostics/history/<encoded-root>/`. The mirror includes `_history/stats/*`, `_history/lastOperation/*`, `_history/unsupported/*`, `head/sequence`, and per-entry metadata at `entries/<sequence>/{operation,path,tag,timestampMs,monotonicNs,barrier,valueBytes,inverseBytes,hasValue,hasInverse}` so inspector/ServeHtml tooling can consume history state without touching user namespaces.

## Guardrails and `HistoryOptions`
- `enableHistory()` rejects overlapping roots unless both the default options and the requested options set `allowNestedUndo`. Nested undo layers remain discouraged because the journal assumes a single writer for each root; sharing roots without `allowNestedUndo` will fail fast with `Error::Code::InvalidPermissions`.
- `HistoryOptions::sharedStackKey` advertises a caller's intent to reuse a single stack across multiple roots. The wrapper now compares keys across every registered root and fails the call if the key is already in use, forcing callers to regroup their data under one history-enabled subtree or funnel shared work through a command-log mount.
- Persistence knobs (`persistHistory`, `persistenceRoot`, `persistenceNamespace`, `maxDiskBytes`, `ramCacheEntries`, `keepLatestFor`) are all resolved during `enableHistory()` so there is a single source of truth per root. Mutation logging is always journal-based now; the wrapper logs a warning the first time it forces `useMutationJournal = true` for legacy callers that still request copy-on-write snapshots.
- `_history/set_manual_garbage_collect` toggles `HistoryOptions::manualGarbageCollect`. When enabled, the wrapper defers compaction until the command arrives; otherwise auto-retention trims the journal whenever budgets are exceeded. In both cases, `compactJournalPersistence()` updates disk metrics and publishes notifications so observers see the change.

### `HistoryOptions` reference

| Field | Default | Notes |
| --- | --- | --- |
| `maxEntries` | `128` | Primary retention cap (0 = unlimited). Reflected via `_history/stats/limits/maxEntries`. |
| `maxBytesRetained` | `0` | Byte budget before compaction trims oldest entries. Mirrors `_history/stats/limits/maxBytesRetained`. |
| `keepLatestFor` | `0ms` | Minimum age to keep snapshots even if budgets are hit. Published as `_history/stats/limits/keepLatestForMs`. |
| `ramCacheEntries` | `8` | How many decoded entries stay resident; changing this immediately updates `_history/stats/limits/ramCacheEntries`. |
| `maxDiskBytes` | `0` | Disk budget for persisted journals. Surfaces at `_history/stats/limits/maxDiskBytes`. |
| `manualGarbageCollect` | `false` | Mirrors `_history/stats/manualGcEnabled`; callers toggle it via `_history/set_manual_garbage_collect`. |
| `allowNestedUndo` | `false` | Enables nested history roots only when both defaults and per-root options opt in. |
| `useMutationJournal` | `false` (forced to `true`) | Copy-on-write snapshots are deprecated; the wrapper always enables the mutation journal and logs when coercing legacy callers. |
| `persistHistory` | `false` | Enables persistence and `_history/stats/limits/persistHistory`. Requires `persistenceRoot` + `persistenceNamespace`. |
| `persistenceRoot` / `persistenceNamespace` | empty | Describe where persisted PSJL directories live once `persistHistory` is on. |
| `restoreFromPersistence` | `true` | Indicates whether persistence should hydrate existing entries on enable. Surfaced via `_history/stats/limits/restoreFromPersistence`. |
| `sharedStackKey` | `std::optional` | Declares intent to share stacks; duplicate keys are rejected. |

## Widget Migration Guidance
1. **Choose a single root per widget.** All state that participates in a logical command must live under one root (`/widgets/<id>/state/...` is the common case). Consolidate cross-widget metadata under the same subtree or store it behind a command-log shim so undo stacks never span multiple roots.
2. **Wrap the widget space.** When creating the declarative scene, construct an `SP::History::UndoableSpace` around the widget `PathSpace`, then call `enableHistory()` on the widget root with retention/persistence budgets that match the UI's expectations. Persisted scenes should set `persistHistory=true` and provide a namespace rooted in the app's support directory.
3. **Publish UI bindings.** The declarative runtime exposes helpers in `include/pathspace/ui/declarative/HistoryBinding.hpp`. Call `InitializeHistoryMetrics`, `CreateHistoryBinding`, and `PublishHistoryBindingCard` when the widget mounts to seed `/widgets/<id>/metrics/history_binding/*`, and wire buttons/shortcuts to insert into `<root>/_history/undo|redo` rather than invoking custom callbacks.
4. **Expose telemetry to tooling.** Ensure inspector/ServeHtml panels are pointed at `<root>/_history/stats/*` and `_history/lastOperation/*`. Declarative widgets should mirror the binding state (enabled/disabled, last error, undo/redo totals) so UI affordances stay in sync with the underlying journal state.
5. **Retire bespoke glue.** Legacy widgets that hand-wrote copy-on-write snapshots can delete their private undo helpers once the binding publishes telemetry and routes commands into `_history/*`. The journal wrapper already handles multi-threaded mutations, metrics, persistence, and notification fan-out.

## Operational Checklist and Follow-Ups
- Use `pathspace_history_savefile` to export/import PSJL bundles whenever QA needs to capture or replay a widget history bug; the tooling is guaranteed compatible because it shares the same persistence codec as the runtime.
- Verify new integrations by tailing `_history/lastOperation/*` during undo/redo spam. The mutation journal annotates each operation (`type`, `durationMs`, `bytesBefore`, `bytesAfter`), making it easy to spot unexpected payload explosions or transaction contention.
- Annotate user-visible commands with `_history/set_tag = "<label>"` before issuing mutations. The tag is stored on each journal entry and mirrored to `_history/lastOperation/tag`, allowing inspector/ServeHtml panels to display friendly command names alongside undo/redo telemetry.
- ✅ (December 9, 2025) `docs/finished/Plan_WidgetDeclarativeAPI_Finished.md` now links back to this note and spells out the shared history binding/telemetry workflow, so the `docs/finished/Plan_SceneGraph_Finished.md` backlog item is closed and contributors land on the authoritative contract immediately.
