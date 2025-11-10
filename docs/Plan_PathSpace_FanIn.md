# PathSpace Trellis (Fan-In Draft)

_Last updated: November 10, 2025_

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
  - `std::shared_ptr<PathSpace> backing_` – the real space used to fetch values.
  - `std::unordered_map<std::string, TrellisState> states_` protected by a mutex.
- `TrellisState` fields:
  ```cpp
  struct TrellisState {
      enum class Mode { Queue, Latest };
      enum class Policy { RoundRobin, Priority };

      Mode mode;
      Policy policy;

      struct SourceState {
          ConcretePath path;
          std::deque<NodeData> queue;        // used in Queue mode
          std::optional<NodeData> latest;    // used in Latest mode
          std::uint64_t rr_token = 0;        // round-robin slot
          SubscriptionHandle notification;   // cancel on disable
      };
      std::vector<SourceState> sources;

      std::deque<NodeData> merged;           // ready-to-serve outputs
      std::vector<std::promise<NodeData>> waiters;
      std::size_t rr_cursor = 0;
      bool shutting_down = false;
  };
  ```
- Notification callbacks perform a non-blocking `backing_->take`/`read` per source (depending on mode), store the result in `queue/latest`, and call `emit_ready_items()` to either satisfy waiting consumers or append to `merged`.

## Read/take behaviour
- `out()` override checks whether the requested path matches a trellis output:
  - If yes: call `read_trellis(state, meta, opts, obj, consume)`.
    - In **Queue** mode, pop from per-source queues according to policy.
    - In **Latest** mode, move the most recent value for a source and clear the slot.
    - Executions are passed through as untouched `NodeData` so consumers still see callable nodes.
    - If no payload is ready, register a waiter promise and block until a notification fills the queue.
  - If no: delegate to `backing_->out()`.

## Modes & policy
- **Mode::Queue** – each source preserves FIFO order; `Policy` decides which source to pull from next (`RoundRobin` rotates through non-empty sources; `Priority` always takes the first non-empty source).
- **Mode::Latest** – only the most recent value per source is stored; consumers always see the most up-to-date item when it becomes available.

## Validation rules
- `name` must be an absolute concrete path (not under `_system/trellis/` itself) and unused.
- `sources` must be absolute, concrete, distinct, and different from `name`.
- Reject empty source lists and unknown `mode`/`policy` strings.
- On error, return the usual `InsertReturn::errors` entry (e.g., `Error::InvalidPath`, `Error::AlreadyExists`).

## Waiters & shutdown
- Blocked readers/takers are stored as promises inside `TrellisState.waiters`.
- When new data arrives `emit_ready_items()` resolves waiters before appending to `merged`.
- On disable, set `shutting_down = true`, resolve all remaining waiters with `Error::Shutdown`, remove notification hooks, and erase the state entry.

## Usage examples
1. **Widget event bridge** – enable a trellis where `sources` are multiple widget op queues; automation waits on `/system/trellis/widgetOps/out`.
2. **Lambda launch pad** – combine execution queues under `/jobs/<id>/inbox`; worker calls `take(out_path)` to run whichever job becomes available first.
3. **Status mirror** – in `latest` mode, keep `/system/trellis/devices/telemetry` updated with the most recent status across `/devices/<id>/state`.

## Testing checklist
- Enable / disable via command inserts (success and failure cases).
- Round-robin ordering across two sources producing alternately.
- Priority mode always favors the first source.
- Latest mode replaces older values.
- Execution payload is forwarded as an execution node.
- Concurrency stress: multiple producers + a blocking consumer.
- Disable wakes blocked readers with `Error::Shutdown`.

## Deferred work
- Persist trellis configs (reload on restart).
- Expose metrics (`/system/trellis/<name>/stats/*`).
- Optional glob-based source discovery.
- Back-pressure controls (per-source queue limits).
