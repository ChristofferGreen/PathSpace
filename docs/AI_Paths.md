# PathSpace — Standard Paths

Reserved path names and conventions used by the code in `src/pathspace/`. Keep this file in sync with `src/pathspace/history/UndoHistoryUtils.hpp` and `src/pathspace/layer/PathSpaceTrellis.cpp` when adding or renaming nodes. Path syntax, globbing, and validation rules are described in `docs/AI_Architecture.md` ("Path System").

Conventions:
- Paths are absolute (`/a/b`) at the public API; relative forms are only used inside layers that adopt a mount prefix.
- Names starting with `_` are reserved for built-in control and telemetry surfaces.
- Application layouts (`/system/applications/<app>` and similar) are caller conventions, not enforced by the core.

## 1) History (`UndoableSpace`)

Every history-enabled root `<root>` owns a `_history` subtree (`UndoHistoryUtils::Paths`).

Commands (insert-only control nodes):

| Path | Effect |
| --- | --- |
| `<root>/_history/undo` | Undo the latest entry |
| `<root>/_history/redo` | Redo the latest undone entry |
| `<root>/_history/garbage_collect` | Run retention/compaction now |
| `<root>/_history/set_manual_garbage_collect` | Toggle `HistoryOptions::manualGarbageCollect` |
| `<root>/_history/set_tag` | `std::string` label stamped on subsequent journal entries |

Telemetry (read-only):
- `<root>/_history/stats/{undoCount,redoCount,undoBytes,redoBytes,liveBytes,bytesRetained,manualGcEnabled}`
- `<root>/_history/stats/{trimOperationCount,trimmedEntries,trimmedBytes,lastTrimTimestampMs}`
- `<root>/_history/stats/compaction/{runs,entries,bytes,lastTimestampMs}`
- `<root>/_history/stats/limits/{maxEntries,maxBytesRetained,keepLatestForMs,ramCacheEntries,maxDiskBytes,persistHistory,restoreFromPersistence}`
- `<root>/_history/head/generation`
- `<root>/_history/lastOperation/{type,timestampMs,durationMs,success,undoCountBefore,undoCountAfter,redoCountBefore,redoCountAfter,bytesBefore,bytesAfter,message,tag}`
- `<root>/_history/unsupported/{totalCount,recentCount,recent/*}` — nodes skipped because they hold nested spaces, tasks, or unserializable payloads

Diagnostics mirror: `/diagnostics/history/<encoded-root>/` (compatibility copy at `/output/v1/diagnostics/history/<encoded-root>/`) with `head/sequence` and `entries/<n>/{path,tag,operation,timestampMs,monotonicNs,sequence,barrier,valueBytes,inverseBytes,hasValue,hasInverse}`. `<encoded-root>` is the root path with `/` replaced as described in the journal persistence format section of `docs/AI_Architecture.md`.

See `docs/History_Journal_Wiring.md` for how these nodes are wired.

## 2) Trellis (`PathSpaceTrellis`)

`PathSpaceTrellis` fans several source paths into one readable path. Its control surface lives under `/_system`:
- `/_system/enable` and `/_system/disable` accept a `std::string` source path payload.
- `/_system/*` is not readable; any other command name returns `InvalidPath`.

## 3) Layers

- `PathView` filters access with a permission callback and may prepend a mount prefix.
- `PathAlias` rewrites paths with a configurable `targetPrefix` and forwards `in`/`out`/`notify` upstream.
- `BoundedPathSpace` caps the queue length per path (dropping the oldest same-type entries); `SnapshotCachedPathSpace` adds a read-optimized snapshot cache. Neither changes the path layout.

Details for each layer are in `docs/AI_Architecture.md` ("Views", "Snapshot Cache Layer").
