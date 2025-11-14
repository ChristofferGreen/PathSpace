# PathSpace Trellis (Fan-In Draft)

_Last updated: November 14, 2025 (Buffered fan-in roadmap drafted)_

> **Status (November 12, 2025):** Queue-mode traces now log explicit `serve_queue.result` entries (success, empty, error), and latest-mode wait paths emit both `wait_latest.*` results and matching `notify.ready` entries. Targeted tests (`Queue mode blocks until data arrives`, `Latest mode trace captures priority wake path`, `Latest mode priority polls secondary sources promptly`, `Latest mode priority wakes every source`) now pass in single-shot runs after widening the final wait slice and recording the readiness events. Phase A of the internal `PathSpace` migration is in place (trellises now bootstrap a private `PathSpace` mounted under `/_system/trellis/internal/*`, ready for runtime bookkeeping in later phases).
> **Status (November 12, 2025 — evening):** Queue-mode readiness and trace buffers now dual-write to the embedded `PathSpace` under `/_system/trellis/internal/state/<hash>/runtime/*` while the legacy `TrellisState` containers remain authoritative. (At the time `PATHSPACE_TRELLIS_INTERNAL_RUNTIME=0` disabled the mirrors; as of November 13 it prints a rollback warning and leaves descriptor updates enabled.) New tests assert the internal runtime queue/count and trace snapshots stay aligned with the backing space.
> **Status (November 13, 2025):** Phase C landed with waiter accounting, round-robin cursor updates, and shutdown flags mirrored into the internal runtime tree. New coverage (`Internal runtime mirrors ready queue state`, `Internal runtime tracks waiters, cursor, and shutdown`) exercises the mirrored paths and keeps the feature-flag fallback honest. Queue and latest-mode ordering held across the 15× loop after restoring cursor updates to dual-write the internal snapshot.
> **Update (November 13, 2025 — noon):** Trellis runtime mirrors now use bespoke serialization helpers for waiter snapshots/flags instead of Alpaca. This unblocks the doctests from crashing, but back-pressure coverage (`Back-pressure limit caps simultaneous waiters per source`) still reports a timeout—see immediate follow-ups.
> **Status (November 13, 2025 — afternoon):** Multi-waiter/shutdown stress coverage (`Internal runtime multi-waiter shutdown clears waiters`) now runs in the 15× loop. Concurrent waiters register under the internal runtime mirror, shutdown drains the snapshot cleanly, and the `max_waiters` back-pressure limit persists into runtime state without manual patches—keeping the feature flag safe to toggle.
> **Status (November 13, 2025 — evening):** Phase D removed the legacy `TrellisState` ready queue, waiter maps, traces, and latest-value caches. The runtime descriptor now owns ready queue order, buffered counts, waiters, round-robin cursor, shutdown flags, trace history, and the new latest snapshot payloads. `PATHSPACE_TRELLIS_INTERNAL_RUNTIME` no longer disables the mirrors; setting it to `0` prints a rollback warning while keeping the descriptor active.
> **Status (November 14, 2025 — morning):** Descriptor-first tooling landed: queue-mode coverage now asserts descriptor, internal trace mirror, and persisted stats stay aligned via a new `PathSpaceTrellis::debugRuntimeDescriptor` hook, and `AI_Debugging_Playbook.md` documents the workflow. The failing `Queue mode blocks until data arrives` regression is green again with the descriptor-backed trace assertions.

> **Priority focus (November 14, 2025):** With Phase D merged, descriptor-driven tooling is the default surface. The buffered fan-in roadmap below now assumes the descriptor as its authoritative state and captures milestones for exposing per-source buffers, leaving Phase E free to focus on config mutation transactions.

## Highest priority (as of November 14, 2025)

- ✅ **Descriptor-first tooling.** `AI_Debugging_Playbook.md`, `AI_ARCHITECTURE.md`, and queue-mode regression tests reference the descriptor as the primary surface. `PathSpaceTrellis::debugRuntimeDescriptor` keeps tests out of persistence while exercising descriptor-backed data; rollback guidance now points to reverting commits rather than toggling feature flags.
- ✅ **Buffered fan-in roadmap (November 14, 2025).** Descriptor-backed fan-in design captured below covers new per-source buffer projections, snapshot schema, and validation cadence so implementation can begin without re-litigating legacy mutex state.
- 🔄 **Mutex retirement plan.** Schedule a follow-up milestone after buffered fan-in implementation to relocate enable/disable/config rewrites into internal `PathSpace` compare-and-swap transactions. Once these flows run entirely through the embedded space we can delete `TrellisState::mutex` and let the internal PathSpace own multithreaded coordination end-to-end.
  - ✅ **Runtime descriptor snapshots (November 13, 2025, evening):** Trellis state now publishes `TrellisRuntimeDescriptor` metadata under `/_system/trellis/internal/state/<hash>/runtime/descriptor`, capturing config, ready queue, waiters, cursor, back-pressure limits, trace entries, and latest snapshot payloads for live inspection.

### Migration roadmap (drafted November 12, 2025 — Codex)

We will land the embedded-`PathSpace` migration in four incremental phases so each step keeps the suite green and isolates risk.

1. **Phase A — Bootstrap internal space (no behavioural changes).** ✅ _Completed November 12, 2025._
   - Instantiate a private `PathSpace` inside `PathSpaceTrellis`, mounting it under a reserved prefix (e.g. `/_system/trellis/internal/*`) while sharing the existing `PathSpaceContext`.
   - Expose a test-only hook to confirm the embedded space exists after construction/enables; runtime bookkeeping will migrate into this space during Phase B/C.

2. **Phase B — Ready-queue and trace storage migration.** ✅ _Completed November 12, 2025._
   - Queue-mode readiness buffers now dual-write to `/_system/trellis/internal/state/<hash>/runtime/ready/{queue,count}` while the legacy `readySources` deque stays authoritative.
   - Trace snapshots mirror into `/_system/trellis/internal/state/<hash>/runtime/trace/latest`, kept in lock-step with persisted stats.
   - Mirroring originally shipped behind `PATHSPACE_TRELLIS_INTERNAL_RUNTIME`; the flag now logs a rollback warning but keeps the descriptor and compatibility mirrors enabled. Unit coverage (`Queue mode mirrors internal runtime ready queue and trace`) guards the internal paths.

3. **Phase C — Waiter accounting and round-robin cursor.** ✅ _Completed November 13, 2025._
   - Waiter counts, round-robin cursor updates, and shutdown flags now dual-write to `/_system/trellis/internal/state/<hash>/runtime/{waiters,cursor,flags/shutting_down}` while legacy mutex fields remain authoritative.
   - New unit coverage (`Internal runtime mirrors ready queue state`, `Internal runtime tracks waiters, cursor, and shutdown`) exercises the mirrored runtime paths alongside the existing queue/latest trace regressions.
   - The `PATHSPACE_TRELLIS_INTERNAL_RUNTIME` flag stays in place for emergency rollback; loop stability was validated with the flag enabled and disabled.

4. **Phase D — Legacy removal and cleanup.** ✅ _Completed November 13, 2025._
   - Runtime descriptors (`/_system/trellis/internal/state/<hash>/runtime/descriptor`) are now the single source of truth for config, ready queue ordering, buffered counts, round-robin cursor, waiters, shutdown flags, trace history, and latest snapshot payloads.
   - `TrellisState` has been reduced to configuration fields (mode, policy, source list, max waiters) plus its mutex; all bookkeeping mutates the descriptor helpers and per-trellis internal paths.
   - `PATHSPACE_TRELLIS_INTERNAL_RUNTIME=0` no longer disables the mirrors. Setting the flag prints a rollback warning while keeping descriptor updates enabled so the fallback path is to revert the commit, not to toggle the feature.
   - Persistence helpers replay state directly into the internal runtime tree, and new descriptor-driven helpers keep the compatibility mirrors (`ready/{queue,count}`, `waiters`, `cursor`, `flags/shutting_down`, `trace/latest`, `descriptor`) aligned.

**Validation cadence**
- After each phase: `cmake --build build -j` followed by `./scripts/compile.sh --clean --test --loop=15 --release`.
- Add focused doctest invocations for any new stress cases introduced in Phases B/C (and descriptor-first follow-ups).
- Continue running `./scripts/compile.sh --test --loop=15 --release --runtime-flag-report` to capture loop timings. Setting `PATHSPACE_TRELLIS_INTERNAL_RUNTIME=0` now prints a rollback warning while leaving descriptor updates enabled, so safety checks rely on regressions staying green rather than disabling the mirrors.

**Risks & mitigations**
- *Descriptor drift or missing nodes.* Regression coverage now reads the runtime descriptor (`Runtime descriptor mirrors trellis state`) to ensure ready queues, waiters, cursor, shutdown flags, and traces remain in sync with live traffic.
- *Performance regressions from descriptor writes.* Profile queue/latest hot paths with the loop harness; if descriptors introduce measurable overhead, revert the descriptor commit rather than toggling the warning-only flag.
- *Persistence schema compatibility.* Maintain existing stats/config nodes; the internal runtime tree stays private and excluded from persisted state to avoid upgrade churn.

> **Previous snapshot (November 11, 2025):** `PathSpaceTrellis` ships with queue/latest fan-in, persisted configuration reloads, live stats under `_system/trellis/state/*/stats`, and per-source back-pressure limits. Latest mode performs non-destructive reads across all configured sources, honours round-robin and priority policies, and persistence keeps trellis configs + counters across restarts. Buffered fan-in remains in Deferred work.

## Goal
Provide a lightweight "fan-in" overlay that exposes a single public path backed by multiple concrete source paths. Consumers read/take from the trellis path and receive whichever source produces the next payload (including executions). The trellis is implemented as a `PathSpaceBase` subclass that forwards most operations to an underlying `PathSpace` but intercepts control commands and reads for managed paths.

## Buffered fan-in roadmap — November 14, 2025

**Objective**  
Expose buffered fan-in semantics (per-source depth, dequeue windows, and buffered replay) via the runtime descriptor so tooling and downstream readers can observe the same state the implementation mutates.

**Descriptor extensions**
- Add a `std::vector<TrellisBufferedSource>` to `TrellisRuntimeDescriptor` containing `{source, bufferedDepth, oldestTimestampNs, newestTimestampNs}` so each source advertises how many payloads (queue mode) or snapshots (latest mode) are staged.
- Promote the existing aggregated counter to `bufferedTotals` (`readyCount`, `earliestReadyNs`, `latestReadyNs`) so stats readers retain fast access to global depth without iterating sources.
- Persist per-source consumer cursors when we introduce buffered replay windows so clients can resume from a known descriptor revision.
- Mirror descriptor data under `_system/trellis/internal/state/<hash>/runtime/buffered/*` for CLI tooling that prefers leaf nodes over the consolidated descriptor payload.

**Implementation phases**
1. **BF-1: Descriptor plumbing.** Extend `TrellisRuntimeDescriptor`, serialization helpers, and mirrors so buffered depth derives exclusively from descriptor writes. Update queue/latest execution paths to populate per-source entries whenever we enqueue, dequeue, or drop payloads.
2. **BF-2: Persistence and stats.** Augment `_system/trellis/state/<hash>/stats` with `buffered_ready_per_source/<source>` snapshots fed from the descriptor. Ensure reload paths seed descriptor depth from persisted stats when configs restore.
3. **BF-3: Buffered replay policy.** Teach queue mode to serve batches without losing ordering guarantees, using the descriptor cursor to decide which buffered entry to expose. Latest mode continues to expose deduped snapshots, but we record `latestSnapshots[i].sequence` so tooling can detect staleness.
4. **BF-4: Tooling + inspector.** Update CLI tooling, inspector mocks, and documentation to read the new descriptor fields. Surface per-source depth bars alongside existing wait/serve counters.

**Validation cadence**
- Extend `tests/unit/layer/test_PathSpaceTrellis.cpp` with descriptor assertions (`Buffered fan-in descriptor lists every ready source`, `Buffered depth drains after dequeue`) covering queue and latest modes.
- Keep the mandated 15× loop harness (`./scripts/compile.sh --test --loop=15 --release --runtime-flag-report`) to catch regressions across descriptor writes; include assertions that per-source depth remains bounded during shutdown.
- Add fuzz coverage for simultaneous produce/consume across multiple sources once buffered replay lands.

**Documentation + tooling**
- Update architecture/debugging docs (done in this change) when descriptor fields ship.
- Inspector and PathIO tooling should rely on `_runtime/descriptor` first, using the mirrored subtree only when incremental reads are required.
- Track follow-on notes for Phase E (config transactions) once buffered fan-in phases BF‑1 → BF‑4 complete.

## Control surface
- **Enable** – insert a command struct at `_system/trellis/enable`
  ```cpp
  struct EnableTrellisCommand {
      std::string name;                  // output path, e.g. "/system/trellis/widgetOps/out"
      std::vector<std::string> sources;  // absolute concrete paths
      std::string mode;                  // "queue" | "latest"
      std::string policy;                // "round_robin" | "priority"
  };
  ```
  The trellis validates the payload, sets up state, registers notification hooks on each source, and keeps bookkeeping (optionally mirroring under `_system/trellis/state/<id>/config`). `policy` accepts optional comma-separated modifiers—today `max_waiters=<n>` enables per-source back-pressure caps (e.g. `"priority,max_waiters=2"`).
- **Disable** – insert `_system/trellis/disable` with:
  ```cpp
  struct DisableTrellisCommand { std::string name; };
  ```
  This tears down hooks, wakes waiters with `Error::Shutdown`, and removes state.

All other inserts/read/take requests pass through to the backing `PathSpace` unchanged.

## Internals
- `class PathSpaceTrellis : public PathSpaceBase` stores:
  - `std::shared_ptr<PathSpaceBase> backing_` – the concrete space used to satisfy requests.
  - `std::unordered_map<std::string, std::shared_ptr<TrellisState>> trellis_` protected by a mutex.
- `TrellisState` now keeps only the configuration knobs and its mutex; all runtime bookkeeping lives in the embedded `PathSpace`:
  ```cpp
  struct TrellisState {
      TrellisMode              mode{TrellisMode::Queue};
      TrellisPolicy            policy{TrellisPolicy::RoundRobin};
      std::vector<std::string> sources;              // canonical absolute paths
      std::size_t              maxWaitersPerSource{0};
      mutable std::mutex       mutex;
  };
  ```
- _Current-state note:_ Ready queue ordering, buffered counts, active waiters, round-robin cursor, shutdown flags, bounded traces, and source snapshots now mutate `TrellisRuntimeDescriptor` under `/_system/trellis/internal/state/<hash>/runtime/descriptor` (with compatibility mirrors for `ready/{queue,count}`, `waiters`, `cursor`, `flags/shutting_down`, and `trace/latest`). Tooling reads the descriptor instead of stitching together the individual mirrors.
- Queue mode performs non-blocking fan-out across the configured sources; if nothing is ready and the caller requested blocking semantics, `PathSpaceTrellis` delegates the wait to the backing `PathSpace` using that space's native timeout/notify machinery.
- Latest mode performs a non-destructive sweep across the configured sources following the active policy. Round-robin rotates the selection cursor whenever a source produces data so subsequent reads surface other producers without clearing their backing queues.
- Trellis configs persist automatically: enabling a trellis stores `TrellisPersistedConfig` under `/_system/trellis/state/<hash>/config`; new `PathSpaceTrellis` instances reload the configs on construction. Back-pressure limits live alongside the config under `/_system/trellis/state/<hash>/config/max_waiters`.
- Trellis stats mirror live counters under `/_system/trellis/state/<hash>/stats` (`TrellisStats` with mode, policy, sources, servedCount, waitCount, errorCount, backpressureCount, lastSource, lastErrorCode, lastUpdateNs). Stats update after each serve, record waits keyed off blocking reads, increment `backpressureCount` when the waiter cap is hit, and preserve the last error code until a new error overwrites it (successful serves no longer clear the code). Buffered readiness is exposed separately via `/_system/trellis/state/<hash>/stats/buffered_ready`.
- Latest-mode traces persist both inside the descriptor and under `/_system/trellis/state/<hash>/stats/latest_trace` as a bounded (`64` entries) `TrellisTraceSnapshot`. Each `TrellisTraceEvent` records a nanosecond timestamp plus a human-readable message (register, wait/block, notify, result) so priority wake paths can be inspected without the temporary `std::cout` logging.

## Read/take behaviour
- `out()` override checks whether the requested path matches a trellis output:
  - If yes: call `read_trellis(state, meta, opts, obj, consume)`.
    - In **Queue** mode, attempt non-blocking `take`/`read` on each source respecting the active policy. When blocking is requested and no source is ready, the layer blocks on the next policy-selected source using the backing `PathSpace` wait loop.
    - In **Latest** mode, issue non-destructive reads across all sources following the active policy. Round-robin advances whenever a source supplies data so repeated reads surface other producers; `take` requests are treated as reads to keep source state intact.
    - Executions are forwarded untouched so downstream code still observes callable nodes.
  - If no: delegate to `backing_->out()`.

## Modes & policy
- **Mode::Queue** – each source preserves FIFO order; `Policy` decides which source to pull from next (`RoundRobin` rotates through non-empty sources; `Priority` always favors the first configured source that has data).
- **Mode::Latest** – provides a non-destructive mirror of the most recent value for each source. Reads sweep the configured sources using the active policy; `take` calls behave like reads so the backing source retains its state.

## Validation rules
- `name` must be an absolute concrete path (not under `_system/trellis/` itself) and unused.
- `sources` must be absolute, concrete, distinct, and different from `name`.
- Reject empty source lists and unknown `mode`/`policy` strings.
- On error, return the usual `InsertReturn::errors` entry (e.g., `Error::InvalidPath`, `Error::AlreadyExists`).

## Waiters & shutdown
- Blocking callers still rely on the backing `PathSpace` wait/notify loop, but the trellis now enforces the optional `maxWaitersPerSource` cap by updating `descriptor.waiters` (`registerWaiter`/`unregisterWaiter` mutate the snapshot directly). The `Runtime descriptor mirrors trellis state` doctest keeps the snapshot aligned.
- On disable or shutdown, the descriptor’s `flags.shuttingDown` flips to `true` and waiters are drained from `descriptor.waiters`, so outstanding waits observe `Error::Shutdown` without depending on legacy mutex bookkeeping.
- Latest-mode priority waits limit each per-source blocking slice to 20 ms so the polling loop revisits secondary sources within the caller’s timeout budget; trace snapshots and descriptor updates record the resulting wait/notify cycle.

## Usage examples
1. **Widget event bridge** – enable a trellis where `sources` are multiple widget op queues; automation waits on `/system/trellis/widgetOps/out`.
2. **Lambda launch pad** – combine execution queues under `/jobs/<id>/inbox`; worker calls `take(out_path)` to run whichever job becomes available first.
3. **Status mirror** – use latest mode to keep `/system/trellis/devices/telemetry` updated with the most recent status across `/devices/<id>/state`.

## Testing checklist
- Enable / disable via command inserts (success and failure cases).
- Round-robin ordering across two sources producing alternately.
- Priority mode favors the first source when both have queued items.
- Blocking consumer wakes as soon as one of the sources produces.
- Disable wakes blocked readers with `Error::Shutdown`.
- Latest mode mirrors recent values without popping sources; cover both policies (priority and round-robin) plus blocking waits.
- Persistence reload validates that trellis configs survive restart and disappear after disable.
- Stats surface served/wait/error/back-pressure counters and last-source metadata under `/_system/trellis/state/<hash>/stats`.
- Back-pressure caps reject excess concurrent waits (`Error::CapacityExceeded`) and bump `backpressureCount`.
- Buffered readiness counter exposed under `/_system/trellis/state/<hash>/stats/buffered_ready`.
- Latest trace snapshot under `/_system/trellis/state/<hash>/stats/latest_trace` captures waiter/notify/result events; covered by `tests/unit/layer/test_PathSpaceTrellis.cpp` (“Latest mode trace captures priority wake path”).
- Priority polling latency covered by `tests/unit/layer/test_PathSpaceTrellis.cpp` (“Latest mode priority polls secondary sources promptly”).
- Queue-mode waits now emit trace entries (`tests/unit/layer/test_PathSpaceTrellis.cpp`, “Queue mode blocks until data arrives”) so buffered fan-in tooling can reuse the same inspection surface.
- Legacy back-pressure/config nodes are removed on disable; validated by `tests/unit/layer/test_PathSpaceTrellis.cpp` (“Trellis configuration persists to backing state registry”).
- Buffered depth accounting covered by `tests/unit/layer/test_PathSpaceTrellis.cpp` (“Queue mode buffers multiple notifications per source”, “Buffered ready count drains after blocking queue wait”).
- Internal runtime snapshots covered by `tests/unit/layer/test_PathSpaceTrellis.cpp` (“Internal runtime mirrors ready queue state”, “Internal runtime tracks waiters, cursor, and shutdown”, “Internal runtime multi-waiter shutdown clears waiters”); keep flag-off smoke runs in the loop to validate fallback readiness.

### Runtime descriptor snapshot (added November 13, 2025; roadmap refreshed November 14, 2025)
- The internal runtime now exports `TrellisRuntimeDescriptor` under `/_system/trellis/internal/state/<hash>/runtime/descriptor`. Fields capture the persisted config (name/mode/policy/sources), ready queue contents, buffered-ready count, round-robin cursor, `max_waiters_per_source`, shutdown flag, waiter snapshot, and the most recent trace events.
- Tooling can read the descriptor for live diagnostics instead of stitching together queue, waiter, and trace mirrors. See `tests/unit/layer/test_PathSpaceTrellis.cpp` (“Runtime descriptor mirrors trellis state”) for coverage.
- Buffered fan-in implementation will extend the descriptor with per-source buffered depth (`TrellisBufferedSource`) and sequence metadata; see “Buffered fan-in roadmap — November 14, 2025” for the staged rollout and validation plan.

## Deferred work
- Optional glob-based source discovery.
- Buffered fan-in implementation (Phases BF-1 → BF-4) that realises the roadmap above and ships descriptor-backed per-source buffering plus replay semantics.
- **Phase E — config transactions without mutexes.** After buffered fan-in stabilises, move enable/disable/config rewrites into internal-space CAS helpers so the embedded `PathSpace` owns multithreading responsibilities entirely. This phase deletes `TrellisState::mutex` once config updates are driven solely through descriptor transactions.
- Document persistence/stat format guarantees once buffered fan-in lands (include descriptor schema bump notes and migration guidance).

## Immediate follow-ups — November 13, 2025
- ✅ **Multi-waiter/shutdown stress regression (completed November 13, 2025).** Added `Internal runtime multi-waiter shutdown clears waiters`, ensured `max_waiters` is populated on enable, and fixed the waiter scope guard so mirrored snapshots drain on shutdown with the feature flag enabled and disabled.
- ✅ **Feature-flag perf sampling.** Captured 15× loop timings with `PATHSPACE_TRELLIS_INTERNAL_RUNTIME={1,0}` (see `build/trellis_flag_bench.json`). The delta between modes is <0.3 % (avg iteration cost 17.73 s with mirrors vs. 17.68 s without).
- ✅ **Phase D migration.** Legacy ready-queue, waiter, cursor, shutdown, trace, and latest-snapshot bookkeeping moved into the runtime descriptor; `PATHSPACE_TRELLIS_INTERNAL_RUNTIME=0` now emits a warning but keeps mirrors enabled.
- ✅ **Documentation/tooling sync (completed November 14, 2025).** `AI_ARCHITECTURE.md`, `AI_Debugging_Playbook.md`, `Plan_Overview.md`, and this plan now describe descriptor-driven helpers, warn-only flag behaviour, and the buffered fan-in roadmap.
- ✅ **Buffered fan-in roadmap (completed November 14, 2025).** Descriptor-backed milestones (BF-1 → BF-4) captured with schema updates, validation cadence, and tooling expectations.
- ✅ **Back-pressure regression.** `Back-pressure limit caps simultaneous waiters per source` is back to green after the waiter snapshot fixes; feature-flag loops exercised both flag states without triggering the timeout.

### Work breakdown (Phase D wrap-up)
1. ✅ **Descriptor ownership.** Ready queue ordering, buffered counts, waiters, cursor, shutdown flags, traces, and latest snapshots now mutate the runtime descriptor; mirrors stay in sync through descriptor writes.
2. ✅ **Legacy state removal.** `TrellisState` stores only mode/policy/sources/max_waiters, and shutdown drains waiters/flags via descriptor helpers.
3. ✅ **Loop parity.** Existing queue/latest doctests keep descriptor state aligned while the 15× loop runs.
4. 🔄 **Docs/testing alignment.** Update architecture/debugging guides, note the warning-only flag behavior, and extend buffered fan-in planning around descriptor-backed helpers.

## Shutdown note — November 14, 2025
- Queue/latest functionality, stats mirrors, and back-pressure limits remain implemented and exercised by the 15× loop; descriptor-backed traces continue to gate regressions.
- Buffered fan-in features stay deferred until Phase BF-1 kicks off; follow the roadmap above to stage descriptor schema changes without breaking persistence.
- Keep the persistence/stat schemas stable (`TrellisPersistedConfig`, `/_system/trellis/state/<hash>/backpressure/max_waiters`, `/_system/trellis/state/<hash>/stats`, `stats/buffered_ready`) until buffered fan-in ships, then document migration steps alongside the descriptor additions.
