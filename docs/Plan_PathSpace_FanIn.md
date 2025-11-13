# PathSpace Trellis (Fan-In Draft)

_Last updated: November 12, 2025 (Phase C spike blocked)_

> **Status (November 12, 2025):** Queue-mode traces now log explicit `serve_queue.result` entries (success, empty, error), and latest-mode wait paths emit both `wait_latest.*` results and matching `notify.ready` entries. Targeted tests (`Queue mode blocks until data arrives`, `Latest mode trace captures priority wake path`, `Latest mode priority polls secondary sources promptly`, `Latest mode priority wakes every source`) now pass in single-shot runs after widening the final wait slice and recording the readiness events. Phase A of the internal `PathSpace` migration is in place (trellises now bootstrap a private `PathSpace` mounted under `/_system/trellis/internal/*`, ready for runtime bookkeeping in later phases).
> **Status (November 12, 2025 — evening):** Queue-mode readiness and trace buffers now dual-write to the embedded `PathSpace` under `/_system/trellis/internal/state/<hash>/runtime/*` while the legacy `TrellisState` containers remain authoritative. A feature flag (`PATHSPACE_TRELLIS_INTERNAL_RUNTIME=0`) disables mirroring if regressions appear. New tests assert the internal runtime queue/count and trace snapshots stay aligned with the backing space.
> **Status (November 12, 2025 — night):** Phase C spike (waiter/cursor/shutdown mirroring) was prototyped but not landed. Latest-mode regressions (`Latest mode priority wakes every source`, `Latest mode priority polls secondary sources promptly`, legacy config migration) surfaced during the 15× loop, so the code changes were rolled back. Internal mirroring of waiters/cursor/shutdown remains pending (legacy mutex fields are still authoritative).

> **Priority focus (November 12, 2025):** Migrate trellis bookkeeping to an internal `PathSpace` so readiness queues, stats, and persistence reuse core wait/notify semantics without bespoke mutexes.

## Highest priority (as of November 12, 2025 — night)

- **Phase C — Waiter accounting and cursor migration (blocked).** The first implementation pass exposed latest-mode ordering regressions and intermittent legacy-config crashes during the looped suite. Rework is required before re-attempting the mirror: diagnose the latest-mode wake ordering, restore test parity, and only then reintroduce the internal runtime nodes for waiters/cursor/shutdown.

### Migration roadmap (drafted November 12, 2025 — Codex)

We will land the embedded-`PathSpace` migration in four incremental phases so each step keeps the suite green and isolates risk.

1. **Phase A — Bootstrap internal space (no behavioural changes).** ✅ _Completed November 12, 2025._
   - Instantiate a private `PathSpace` inside `PathSpaceTrellis`, mounting it under a reserved prefix (e.g. `/_system/trellis/internal/*`) while sharing the existing `PathSpaceContext`.
   - Expose a test-only hook to confirm the embedded space exists after construction/enables; runtime bookkeeping will migrate into this space during Phase B/C.

2. **Phase B — Ready-queue and trace storage migration.** ✅ _Completed November 12, 2025._
   - Queue-mode readiness buffers now dual-write to `/_system/trellis/internal/state/<hash>/runtime/ready/{queue,count}` while the legacy `readySources` deque stays authoritative.
   - Trace snapshots mirror into `/_system/trellis/internal/state/<hash>/runtime/trace/latest`, kept in lock-step with persisted stats.
   - Mirroring can be disabled via `PATHSPACE_TRELLIS_INTERNAL_RUNTIME=0`. Unit coverage (`Queue mode mirrors internal runtime ready queue and trace`) guards the internal paths.

3. **Phase C — Waiter accounting and round-robin cursor.** ⏸️ _On hold November 12, 2025._
   - ⛔ Latest-mode loop failures (`Latest mode priority wakes every source`, `Latest mode priority polls secondary sources promptly`) appeared after introducing internal mirroring; the code was reverted.
   - 🔁 Next restart: reproduce the failures with targeted doctest seeds, capture traces, and fix ordering before re-attempting mirroring. Add a focused regression to guard the wake ordering paths.
   - 📊 Once stable, re-run the mirroring spike with incremental commits, padding the loop suite to confirm no timing regressions, then proceed to perf sampling.

4. **Phase D — Legacy removal and cleanup.**
   - Delete the old `TrellisState` containers, replacing them with lightweight descriptors (mode/policy/source list/max waiters) and internal-path helpers.
   - Simplify persistence helpers to replay solely into the internal space.
   - Refresh documentation (`docs/AI_Architecture.md`, this plan) and update tooling references to the new runtime paths.

**Validation cadence**
- After each phase: `cmake --build build -j` followed by `./scripts/compile.sh --clean --test --loop=15 --release`.
- Add focused doctest invocations for any new stress cases introduced in Phases B/C.

**Risks & mitigations**
- *Runtime drift between legacy and internal state (Phases B/C).* Mitigate with dual-write assertions in tests until the legacy structures are removed.
- *Performance regressions from internal `PathSpace` overhead.* Profile queue-mode hot paths during Phase B; fall back via feature flag if we observe measurable regressions.
- *Persistence schema compatibility.* Maintain existing stats/config nodes; internal runtime path is private and excluded from persisted state to avoid upgrade churn.

> **Previous snapshot (November 11, 2025):** `PathSpaceTrellis` ships with queue/latest fan-in, persisted configuration reloads, live stats under `_system/trellis/state/*/stats`, and per-source back-pressure limits. Latest mode performs non-destructive reads across all configured sources, honours round-robin and priority policies, and persistence keeps trellis configs + counters across restarts. Buffered fan-in remains in Deferred work.

## Goal
Provide a lightweight "fan-in" overlay that exposes a single public path backed by multiple concrete source paths. Consumers read/take from the trellis path and receive whichever source produces the next payload (including executions). The trellis is implemented as a `PathSpaceBase` subclass that forwards most operations to an underlying `PathSpace` but intercepts control commands and reads for managed paths.

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
- `TrellisState` (first landing) keeps:
  ```cpp
  struct TrellisState {
      enum class Mode { Queue, Latest };
      enum class Policy { RoundRobin, Priority };

      Mode mode{Mode::Queue};
      Policy policy{Policy::RoundRobin};
      std::vector<std::string> sources; // canonical absolute paths
      std::size_t roundRobinCursor{0};
      bool shuttingDown{false};
      mutable std::mutex mutex;
      std::size_t maxWaitersPerSource{0};
      std::unordered_map<std::string, std::size_t> activeWaiters;
      std::deque<std::string> readySources;              // buffered fan-in notifications
      std::unordered_set<std::string> readySourceSet;    // dedupe helper for readySources
      std::deque<TrellisTraceEvent> trace;
  };
  ```
- _Current-state note:_ This structure reflects the mutex-backed implementation that shipped through November 12, 2025. The migration plan above replaces these in-memory fields with records stored inside an embedded `PathSpace`, eliminating the bespoke locking while preserving the on-disk schema.
- Queue mode performs non-blocking fan-out across the configured sources; if nothing is ready and the caller requested blocking semantics, `PathSpaceTrellis` delegates the wait to the backing `PathSpace` using that space's native timeout/notify machinery.
- Latest mode performs a non-destructive sweep across the configured sources following the active policy. Round-robin rotates the selection cursor whenever a source produces data so subsequent reads surface other producers without clearing their backing queues.
- Trellis configs persist automatically: enabling a trellis stores `TrellisPersistedConfig` under `/_system/trellis/state/<hash>/config`; new `PathSpaceTrellis` instances reload the configs on construction. Back-pressure limits live alongside the config under `/_system/trellis/state/<hash>/config/max_waiters`.
- Trellis stats mirror live counters under `/_system/trellis/state/<hash>/stats` (`TrellisStats` with mode, policy, sources, servedCount, waitCount, errorCount, backpressureCount, lastSource, lastErrorCode, lastUpdateNs). Stats update after each serve, record waits keyed off blocking reads, increment `backpressureCount` when the waiter cap is hit, and reset the last error on success. Buffered readiness is exposed separately via `/_system/trellis/state/<hash>/stats/buffered_ready`.
- Latest-mode traces persist to `/_system/trellis/state/<hash>/stats/latest_trace` as a bounded (`64` entries) `TrellisTraceSnapshot`. Each `TrellisTraceEvent` records a nanosecond timestamp plus a human-readable message (register, wait/block, notify, result) so priority wake paths can be inspected without the temporary `std::cout` logging.

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
- Blocking callers still rely on the backing `PathSpace` wait/notify loop, but the trellis now tracks active waiters per source so it can enforce the optional `maxWaitersPerSource` cap (returning `Error::CapacityExceeded` when the limit is reached).
- On disable, the trellis marks the state as shutting down and notifies through the shared `PathSpaceContext` so outstanding waits observe `Error::Shutdown`.
- Future iterations can build on the waiter tracking to add buffered fan-in or richer queue visibility.
- Latest-mode priority waits limit each per-source blocking slice to 20 ms so the polling loop revisits secondary sources within the caller’s timeout budget; trace snapshots record the resulting wait/notify cycle.

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
- Internal runtime waiter/cursor/shutdown mirrors — _pending; ensure new regression covers this once mirroring lands._

## Deferred work
- Optional glob-based source discovery.
- Buffered fan-in: richer per-source buffering/waiter infrastructure and queue visibility built atop the current back-pressure hooks.
- Document persistence/stat format guarantees once buffered fan-in lands.

## Immediate follow-ups — November 12, 2025 (Phase C on hold)
- **Latest-mode regression triage.** Reproduce the loop failures (`Latest mode priority wakes every source`, `Latest mode priority polls secondary sources promptly`) with tracing enabled, capture the wake ordering, and fix the priority hand-off so both sources surface in the trace output again.
- **Legacy config crash.** Investigate the `Legacy trellis config migrates to backpressure path` SIGTERM seen in the loop (likely from the rolled-back spike) to confirm no latent shutdown issues remain before the next attempt.
- **Feature-flag safety net.** Ensure `PATHSPACE_TRELLIS_INTERNAL_RUNTIME=0` shortcuts the new paths before re-landing mirroring so the spike can be isolated without impacting production usage.
- **Loop discipline.** Keep using `./scripts/compile.sh --clean --test --loop=15 --release` after each trellis change while buffered fan-in work continues.

### Work breakdown (Phase C restart checklist)
1. **Stabilize latest-mode priority wake path.** Add focused doctest coverage that asserts alternating sources appear in traces and results, preventing regressions.
2. **Reintroduce waiter/cursor/shutdown mirroring incrementally.** Land dual-write helpers behind the feature flag, then layer in dedicated tests before turning the flag on by default.
3. **Concurrency stress coverage.** Add the planned multi-waiter/shutdown regression to exercise mirrored state once the core wake ordering is stable.
4. **Legacy field deprecation plan.** After mirroring is proven, document the cutover steps for deleting mutex-backed fields and persisting the internal runtime nodes.

## Shutdown note — November 12, 2025
- Queue/latest functionality, stats mirrors, and back-pressure limits remain implemented, but the latest-mode priority timeout must be resolved before buffered fan-in resumes.
- Buffered fan-in stays blocked until the trellis latest-mode test passes reliably in the looped suite.
- Keep the persistence/stat schemas stable (`TrellisPersistedConfig`, `/_system/trellis/state/<hash>/backpressure/max_waiters`, `/_system/trellis/state/<hash>/stats`, `stats/buffered_ready`) while refining the notification + wait logic.
