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
- [ ] Add lightweight persistence helpers: append-only writer, compaction routine, and recovery replay into a provided functor (no `UndoableSpace` wiring yet).
- [ ] Unit-test `JournalState` in isolation (append/undo/redo/retention, round-trip serialization).

### Phase 2 — Integrate with UndoableSpace
- [ ] Introduce a transitional `UndoJournalRootState` alongside existing `RootState`; wire feature flags to opt into the journal while keeping snapshot paths alive.
- [ ] Update transaction guards to collect mutation batches and flush them into the journal on commit; ensure nested transactions coalesce correctly.
- [ ] Swap `undo`/`redo` handlers to call journal replay APIs while still reporting telemetry via existing structures.
- [ ] Re-implement `_history/command` handlers (undo/redo/GC/toggle) against the journal.
- [ ] Adapt telemetry aggregation to pull from journal stats instead of snapshot metrics.
- [ ] Extend persistence path to load/save journals; keep old snapshot persistence reachable until Phase 4.
- [ ] Compile-time guards ensure persistence directories and options map cleanly to the new storage format.

### Phase 3 — End-to-End Validation
- [ ] Expand regression coverage: transaction batching, multi-step undo/redo, retention trimming, persistence recovery, command inserts (`_history/*`).
- [ ] Add fuzz-style sequences (random insert/take/undo/redo/trim) comparing journal state vs a reference model to catch replay drift.
- [ ] Verify telemetry compatibility via inspector tests or snapshots; ensure `_history/stats/*` outputs match legacy expectations for equivalent scenarios.
- [ ] Benchmark core flows (commit latency, undo/redo) versus the snapshot build to demonstrate improvements or parity.

### Phase 4 — Remove Snapshot Implementation
- [ ] Once tests are green with the journal gated on, delete snapshot-specific code: `CowSubtreePrototype`, snapshot codecs, metadata codecs tied solely to snapshots, persistence helpers, `UndoableSpaceState` fields no longer used.
- [ ] Purge residual references (include headers, build targets, docs) and update inspector tool notes to mention the journal log format.
- [ ] Rename transitional types (`UndoJournalRootState` → `RootState`) and clean up feature flags.
- [ ] Run targeted grep to ensure no leftover references to `Snapshot`, `UndoSnapshotCodec`, or `CowSubtreePrototype`.

### Phase 5 — Cleanup & Docs
- [ ] Update architecture docs (`docs/AI_Architecture.md`, `docs/Plan_Overview.md`, inspector plans) to describe the journal design.
- [ ] Document persistence format (binary log layout, versioning) for tooling consumers.
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
