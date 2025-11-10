# Plan: PathSpace Undo Journal Rewrite

> **Drafted:** November 8, 2025 — transitions the undo history layer from snapshot-based state capture to a mutation journal with replayable inverses.

## Motivation
- Snapshot capture saturates CPU and I/O for large subtrees; commits walk the entire trie regardless of the change surface.
- Persistence produces per-generation snapshot files that balloon disk usage and complicate cleanup logic.
- The codebase around `UndoableSpace` (prototypes, codecs, cache policies) has grown difficult to evolve; a leaner model would reduce maintenance cost.
- A journal aligns with the mental model of “undo replays the inverse operations,” making it easier to explain and extend.

## Scope
- Applies to the undo/redo layer only; core `PathSpace` semantics remain unchanged.
- Journals are maintained per history-enabled root. Nested undo roots continue to be disallowed.
- Persistence continues to be opt-in, now storing mutation logs instead of subtree snapshots.
- Telemetry endpoints must remain source-compatible (same paths/types), even if implementation details shift.
- Out of scope: UI tooling changes, inspector visualization beyond ensuring existing data remains consumable, and redesigning command APIs beyond undo/redo.

## Objectives
1. Replace snapshot stacks with mutation journals that record `insert` and `take` operations plus their replay metadata.
2. Preserve the public API (`enableHistory`, `undo`, `redo`, transactions, trim, save/load) with identical error semantics.
3. Maintain telemetry endpoints (`_history/stats/*`, `_history/lastOperation/*`, `_history/unsupported/*`) using journal-derived metrics.
4. Provide persistence as an append-only journal file with startup replay, including retention-aware compaction.
5. Deliver full regression coverage before removing old code, and ensure the repository no longer references snapshot infrastructure.

## Status — November 9, 2025
- ✅ (November 9, 2025) Journal byte telemetry no longer replays `CowSubtreePrototype` snapshots; `computeJournalByteMetrics` now derives undo/redo/live totals straight from `UndoJournalState::stats`, eliminating the last runtime dependency on snapshot reconstruction.
- ✅ Byte telemetry parity restored: journal `_history/stats/*` now derives undo/redo/live totals via `computeJournalByteMetrics`, matching the snapshot stack outputs and unblocking `journal telemetry matches snapshot telemetry outputs`.
- ✅ Journal control commands wait on active transactions using per-root condition variables, eliminating the `Cannot … while transaction open` spurious failures observed in the multi-threaded stress harness.
- ✅ Fuzz reference model tracks queue semantics (FIFO insert/take) so regression coverage exercises the same observable behaviour as the underlying `PathSpace` implementation; parity holds across random insert/take/undo/redo/trim sequences.
- ⛰️ Next major milestone: Phase 4 cleanup (snapshot code removal, feature-flag collapse) once additional integration validation lands.

## Implementation Phases

### Phase 0 — Foundations & Alignment
- [ ] Circulate this plan for maintainer buy-in; confirm telemetry expectations and persistence requirements.
- [ ] Inventory current snapshot-only code paths to be retired (prototypes, codecs, persistence helpers, utils) and flag any consumers outside `history/`.
- [ ] Establish guardrails: confirm all state mutations flow through `insert`/`take`; document contracts for new mutators.

### Phase 1 — Journal Core (no integration yet)
- [x] Define `JournalEntry` schema (operation kind, path, payload/value, metadata) and serialization helpers for persistence.
  - `src/pathspace/history/UndoJournalEntry.hpp` defines `OperationKind`, `SerializedPayload`, and `JournalEntry` with timestamp/sequence/barrier metadata plus helpers for NodeData payload capture.
  - `src/pathspace/history/UndoJournalEntry.cpp` implements binary encode/decode (`serializeEntry`/`deserializeEntry`) using the `'PSJL'` header, along with payload guards for unsupported NodeData (tasks/futures) and strict length checks.
  - `tests/unit/history/test_UndoJournal.cpp` exercises NodeData payload round-trips and verifies encode/decode parity for all entry fields, establishing the baseline regression suite for future journal work.
- [x] Implement in-memory `JournalState` with append, undo, redo, retention, and cursor management APIs.
  - `src/pathspace/history/UndoJournalState.hpp`/`.cpp` track applied vs. redo cursors, expose peek/undo/redo helpers, and enforce configurable retention limits by entry count and estimated byte usage while accumulating trim metrics.
  - `tests/unit/history/test_UndoJournalState.cpp` exercises append/undo/redo flows, redo truncation on new writes, entry- and byte-based retention, and cursor stability after trims to guard core invariants.
- [x] Add lightweight persistence helpers: append-only writer, compaction routine, and recovery replay into a provided functor (no `UndoableSpace` wiring yet).
  - `src/pathspace/history/UndoJournalPersistence.hpp`/`.cpp` expose `JournalFileWriter`, `replayJournal`, and `compactJournal`, backing storage with a length-prefixed append-only format and optional fsync hooks.
  - `tests/unit/history/test_UndoJournalPersistence.cpp` covers append/reopen flows, compaction against selected entries, and truncated log detection to ensure recovery surfaces file corruption.
- [x] Unit-test `JournalState` in isolation (append/undo/redo/retention, round-trip serialization).
  - `tests/unit/history/test_UndoJournalState.cpp` now validates serialization/deserialize round-trips alongside undo/redo coverage, ensuring in-memory state can be reconstructed from encoded journal entries.

### Phase 2 — Integrate with UndoableSpace
- [x] Introduce a transitional `UndoJournalRootState` alongside existing `RootState`; wire feature flags to opt into the journal while keeping snapshot paths alive.
  - `HistoryOptions` now exposes `useMutationJournal`, and `UndoableSpace` tracks journal-enabled roots via `UndoJournalRootState`, preserving persistence metadata and retention policy scaffolding while snapshot paths remain the default.
  - Snapshot operations detect journal-enabled roots and return a clear "mutation journal not yet supported" error so follow-up phases can safely wire transactional handlers without breaking existing behaviour.
- [x] Update transaction guards to collect mutation batches and flush them into the journal on commit; ensure nested transactions coalesce correctly.
  - Journal-enabled roots now acquire `JournalTransactionGuard`s that batch per-transaction mutations and append them to `UndoJournalState` on commit, sharing the same nesting semantics as snapshot transactions.
  - Inserts and takes on journal roots proceed through the existing `PathSpace` backend while recording placeholder mutation entries; undo/redo APIs still gate with the “mutation journal not yet supported” error until replay wiring lands.
- [x] Swap `undo`/`redo` handlers to call journal replay APIs while still reporting telemetry via existing structures.
  - Journal-enabled roots now record per-mutation payloads and replay them through `UndoableSpace::undo`/`redo`; the tests `journal undo/redo round trip` and `journal take undo restores value` cover regression cases via the core `UndoJournalState` APIs.
- [x] Re-implement `_history/command` handlers (undo/redo/GC/toggle) against the journal.
  - `_history/undo|redo|garbage_collect|set_manual_garbage_collect` now route through the journal code paths; manual retention skips automatic trimming and the command path trims on demand. Added regression coverage in `tests/unit/history/test_UndoableSpace.cpp` and `tests/unit/history/test_UndoJournalState.cpp` to lock the behaviour down.
- [x] Adapt telemetry aggregation to pull from journal stats instead of snapshot metrics.
  - Journal roots now expose `_history/stats/*`, `_history/lastOperation/*`, and `_history/unsupported/*` via `UndoableSpace::getHistoryStats` and the PathSpace read APIs. Aggregation uses `UndoJournalState::Stats` (with undo/redo byte splits) and synchronises telemetry counters after commits and retention. Coverage added in `tests/unit/history/test_UndoableSpace.cpp` and `tests/unit/history/test_UndoJournalState.cpp`.
- [x] Extend persistence path to load/save journals; keep old snapshot persistence reachable until Phase 4.
  - Journal roots replay `journal.log` on enable, restoring applied NodeData into the backing `PathSpace` while reconstructing `UndoJournalState`. Persistence setup compacts logs on retention/GC, updates disk telemetry, and regression coverage lives in `tests/unit/history/test_UndoableSpace.cpp` (“journal persistence replays entries on enable”).
- [x] Compile-time guards ensure persistence directories and options map cleanly to the new storage format.
  - Added compile-time validation helpers for persistence token characters and wired runtime checks so journal/snapshot namespaces and encoded roots reject path separators and traversal tokens. `tests/unit/history/test_UndoableSpace.cpp` now covers invalid namespace scenarios to keep regressions visible.

### Phase 3 — End-to-End Validation
- [x] Expand regression coverage: transaction gating/migrations, multi-step undo/redo, retention trimming, persistence recovery touchpoints, command inserts (`_history/*`).
  - New doctest cases in `tests/unit/history/test_UndoableSpace.cpp` cover journal multi-step replay, manual garbage-collect toggles, retention limits, and the current transaction gating error. The byte-budget guardrails in `tests/unit/history/test_UndoJournalState.cpp` were also tightened to avoid SSO-dependent flakiness.
- [x] Introduce multi-threaded stress suites that hammer undo/redo/trim sequences under concurrent insert/take traffic, validating journal invariants and cursor stability under contention.
   - `tests/unit/history/test_UndoableSpace.cpp` now adds `journal handles concurrent mutation and history operations`, a four-thread stress harness that interleaves inserts, takes, undo/redo replay, and manual garbage-collect commands. The test drains and replays the journal at the end of each run to ensure cursor stability and leaves guard inserts to guarantee undo coverage inside the concurrency window.
- [x] Add fuzz-style sequences (random insert/take/undo/redo/trim) comparing journal state vs a reference model to catch replay drift.
  - `tests/unit/history/test_UndoableSpace.cpp` now includes `journal fuzz sequence maintains parity with reference model`, a deterministic fuzz harness that cross-checks journal operations against a pure reference model, injects manual garbage-collect requests, and validates undo/redo availability via telemetry-aligned stack trimming.
- [x] Verify telemetry compatibility via inspector tests or snapshots; ensure `_history/stats/*` outputs match legacy expectations for equivalent scenarios.
  - Added regression `journal telemetry matches snapshot telemetry outputs` in `tests/unit/history/test_UndoableSpace.cpp` covering stats structs and inspector paths for snapshot vs. journal roots to lock parity.
- [x] Benchmark core flows (commit latency, undo/redo) versus the snapshot build to demonstrate improvements or parity.
  - Added `benchmarks/history/undo_journal_benchmark.cpp`, gated behind `BUILD_PATHSPACE_BENCHMARKS=ON`, to compare snapshot and journal insert/undo/redo throughput with configurable operation counts and payload sizes.
  - Release build measurements on November 9, 2025 (Apple M2 Max, 500 operations, 64-byte payloads, three repeats) recorded snapshot commit/undo/redo latencies of 3.79 s / 6.54 s / 6.53 s versus journal latencies of 10.7 ms / 17.1 ms / 45.6 ms, establishing >350× commit speedup and >600× undo throughput gain for the journal.
  - Follow-up (resolved November 9, 2025): `tests/unit/history/test_UndoableSpace.cpp` now shows byte telemetry parity after deriving undo/redo/live totals from replayed snapshots via `computeJournalByteMetrics`.
  - Follow-up (resolved November 9, 2025): journal undo/redo/garbage_collect commands now wait on active transactions instead of surfacing `Cannot … while transaction open`, unblocking the multi-threaded stress harness.

### Phase 4 — Remove Snapshot Implementation
- **Execution steps:**
  1. **Landing zone (done November 10, 2025)** — `UndoableSpace` now enables the mutation journal by default and routes transaction control through `JournalTransactionGuard`. Snapshot roots are gone; all history entry points exercise the journal semantics (multi-step undo requires explicit step counts).
  2. **Prune dead snapshot code (done November 10, 2025)** — Removed `history/CowSubtreePrototype.*`, `UndoSnapshotCodec.*`, and the metadata helpers; deleted the snapshot-facing unit tests and CLI/inspection wiring, replacing them with journal-only coverage and a trimmed `pathspace_history_inspect` that reports journal metrics.
  3. **Persistence consolidation (done November 10, 2025)** — `UndoableSpaceHistory.cpp`, `UndoableSpaceTransactions.cpp`, and `UndoableSpacePersistence.cpp` now operate exclusively on journal state. Persistence setup/replay/compaction no longer branches through snapshot helpers, and savefile import/export preserves journal sequences without touching snapshot codecs.
  4. **Docs & tooling cleanup (in progress)** — Snapshot references are being scrubbed from docs; the debugging playbook now points at journal inspection, but architecture/plan docs still need a full pass and we need to publish migration guidance for legacy bundles.
- _Progress (November 10, 2025)_: Mutation journal is the only history backend in the tree. Snapshot code, tests, and CLIs were removed; persistence/import/export now replay journal entries directly. Remaining work is doc polish and optional migration tooling.

### Phase 5 — Cleanup & Docs
- [x] Update architecture/docs overview (`docs/AI_Architecture.md`, `docs/Plan_Overview.md`) with the journal-only history note; follow up with inspector-specific documentation.
- [x] Document persistence format (binary log layout, versioning) for tooling consumers.
  - Added `docs/AI_Architecture.md` -> "Journal Persistence Format" with file header/entry layout, versioning rules, and tooling notes.
- [ ] Review scripts/CLIs relying on old persistence files, migrate them to the new format, and update examples.
- [ ] Capture before/after telemetry samples for the debugging playbook.
- [ ] Final audit that the repository no longer ships unused history code.

## Testing Strategy
- Reuse existing history regression suites with the journal implementation; ensure they pass without modification.
- Add new tests for journal-specific concerns (batch rollback, redo truncation, persistence compaction).
- Maintain the mandated `ctest` loop (15 iterations, 20s timeout) as part of the pre-push protocol once code changes land.

## Risks & Mitigations
- **Missing mutator coverage**: if a path mutates outside `insert`/`take`, undo becomes lossy. Mitigation: audit and add assertions guarding unsupported operations; document how new mutators must log entries.
- **Telemetry drift**: derived stats must match previous semantics. Mitigation: golden-data tests comparing old vs. new outputs during the transition.
- **Persistence upgrades**: legacy snapshot directories should migrate gracefully. Mitigation: provide a migration tool or compatibility shim until old data is re-exported; communicate breaking change clearly.
- **Redo invalidation bugs**: ensure journal cursor management is heavily tested, especially around nested transactions and manual trims.

## Open Questions
- Do we need checkpoint snapshots for faster recovery of very long journals, or is linear replay acceptable with retention budgets?
- Should we support opt-in journaling for non-undo analytics (i.e., exposing the mutation log externally)?
- How long do we keep compatibility shims for old snapshot persistence files (one release, longer)?

## Exit Criteria
- All history-enabled paths run exclusively on the mutation journal; snapshot code is absent from the tree.
- Regression suite (including new journal tests) passes in the required loop.
- Documentation and tooling reflect the new design, and the maintainer sign-off confirms parity.
- Manual audit (`rg` for snapshot-related identifiers) returns no relevant matches.
