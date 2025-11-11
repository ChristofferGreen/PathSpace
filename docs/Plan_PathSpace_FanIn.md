# PathSpace Trellis (Fan-In Draft)

_Last updated: November 11, 2025_

> **Status (November 11, 2025):** `PathSpaceTrellis` now ships with both queue and latest fan-in for concrete sources. Latest mode performs non-destructive reads across all configured sources, honours the round-robin and priority policies, and is covered by fresh unit tests. Remaining follow-ups focus on persistence, metrics, and back-pressure (see Deferred work).

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
  The trellis validates the payload, sets up state, registers notification hooks on each source, and keeps bookkeeping (optionally mirroring under `_system/trellis/state/<id>/config`).
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
  };
  ```
- Queue mode performs non-blocking fan-out across the configured sources; if nothing is ready and the caller requested blocking semantics, `PathSpaceTrellis` delegates the wait to the backing `PathSpace` using that space's native timeout/notify machinery.
- Latest mode performs a non-destructive sweep across the configured sources following the active policy. Round-robin rotates the selection cursor whenever a source produces data so subsequent reads surface other producers without clearing their backing queues.

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
- Blocking callers rely on the backing `PathSpace` wait/notify loop; the trellis does not yet maintain its own waiter list.
- On disable, the trellis marks the state as shutting down and notifies through the shared `PathSpaceContext` so outstanding waits observe `Error::Shutdown`.
- Future iterations can re-introduce explicit waiter queues when we add buffered fan-in or metrics.

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

## Deferred work
- Persist trellis configs (reload on restart).
- Expose metrics (`/system/trellis/<name>/stats/*`).
- Optional glob-based source discovery.
- Back-pressure controls (per-source queue limits) and the richer per-source buffering/waiter infrastructure outlined in the original design.

## Shutdown note — November 11, 2025
- Latest mode (priority + round-robin) is live and covered by tests; trellis takes remain non-destructive so source queues stay intact.
- Next focus: persist trellis configs across restarts, surface `/system/trellis/<name>/stats/*`, and design back-pressure controls before enabling buffered fan-in.
- When resuming, start by shaping the persistence story (see Deferred work) and confirm whether metrics need to fan into existing residency dashboards.
