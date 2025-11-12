# PathSpace Trellis (Fan-In Draft)

_Last updated: November 12, 2025_

> **Status (November 12, 2025):** Latest-mode blocking still times out under the priority policy (`tests/unit/layer/test_PathSpaceTrellis.cpp`, “Latest mode blocks until data arrives”). The enable-path InvalidPathSubcomponent failure is mitigated by the new legacy cleanup, but notifications from non-primary sources never reach the waiting consumer, so the harness exits on timeout. Current focus is instrumenting waiter registration, enforcing per-source wake-ups, and validating that notifications propagate through the shared `PathSpaceContext`.

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
  };
  ```
- Queue mode performs non-blocking fan-out across the configured sources; if nothing is ready and the caller requested blocking semantics, `PathSpaceTrellis` delegates the wait to the backing `PathSpace` using that space's native timeout/notify machinery.
- Latest mode performs a non-destructive sweep across the configured sources following the active policy. Round-robin rotates the selection cursor whenever a source produces data so subsequent reads surface other producers without clearing their backing queues.
- Trellis configs persist automatically: enabling a trellis stores `TrellisPersistedConfig` under `/_system/trellis/state/<hash>/config`; new `PathSpaceTrellis` instances reload the configs on construction. Back-pressure limits live alongside the config under `/_system/trellis/state/<hash>/config/max_waiters`.
- Trellis stats mirror live counters under `/_system/trellis/state/<hash>/stats` (`TrellisStats` with mode, policy, sources, servedCount, waitCount, errorCount, backpressureCount, lastSource, lastErrorCode, lastUpdateNs). Stats update after each serve, record waits keyed off blocking reads, increment `backpressureCount` when the waiter cap is hit, and reset the last error on success. Buffered readiness is exposed separately via `/_system/trellis/state/<hash>/stats/buffered_ready`.

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

## Deferred work
- Optional glob-based source discovery.
- Buffered fan-in: richer per-source buffering/waiter infrastructure and queue visibility built atop the current back-pressure hooks.
- Document persistence/stat format guarantees once buffered fan-in lands.

## Immediate follow-ups — November 12, 2025
- **Trace latest-mode waits.** Capture waiter registration and wake paths for priority mode to confirm `PathSpaceContext::notify` sees every source and that the trellis restarts non-destructive reads after a wake.
- **Distribute blocking time across sources.** Adjust the per-source blocking window so priority mode polls each configured source within the caller’s timeout budget; ensure the shared wait loop doesn’t starve secondary sources.
- **Verify legacy cleanup post-disable.** Keep the supplemental doctest logging until the suite passes, then replace it with assertions that formally cover the removal of `/config/max_waiters` and bare `/state/<hash>` payloads.
- **Looped test discipline.** Once the priority wake path succeeds, rerun `./scripts/compile.sh --clean --test --loop=15 --release` to confirm no regressions before returning to buffered fan-in.

## Shutdown note — November 12, 2025
- Queue/latest functionality, stats mirrors, and back-pressure limits remain implemented, but the latest-mode priority timeout must be resolved before buffered fan-in resumes.
- Buffered fan-in stays blocked until the trellis latest-mode test passes reliably in the looped suite.
- Keep the persistence/stat schemas stable (`TrellisPersistedConfig`, `/_system/trellis/state/<hash>/backpressure/max_waiters`, `/_system/trellis/state/<hash>/stats`, `stats/buffered_ready`) while refining the notification + wait logic.
