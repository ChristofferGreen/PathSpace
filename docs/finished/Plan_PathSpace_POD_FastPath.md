# Plan — PathSpace POD Fast Path

## Goal
Make `insert`/`read`/`take` on trivially copyable data (e.g., `space.insert("/ints", 5)`) lock-free on the payload hot path, exposing span-based reads for GPU uploads, while keeping FIFO correctness and current APIs intact.

## Iteration 1 (baseline) ✅
- **Payload split:** Add `podPayload` alongside `Node::data`. A node can be in one of two states: POD fast-path (has `podPayload<T>`) or generic (has `NodeData`). No automatic upgrade yet.
- **Type gate:** POD fast path is only for fundamental/trivially copyable `T`. Any non-POD insert into a POD node returns `NotSupported`; a POD insert into a generic node also returns `NotSupported`. Fast path is enabled by default; no toggle required.
- **Data structure:** `PodPayload<T>` holds an append-only, FIFO ring/segmented buffer with CAS head/tail; no per-op mutex. Backed by `std::atomic` indices and contiguous storage to allow `std::span`.
- **Insert:** If node empty, CAS-install `podPayload<T>` and push. If existing `podPayload<T>` matches `T`, push. If type mismatch or generic present, fail fast with `NotSupported`.
- **Read/take (existing APIs):** For POD nodes, deserialize from `PodPayload<T>`; for generic nodes, current `NodeData` logic. No mixing, so path stays deterministic.
- **Span callback (read only):** Overload `read(path, Callable)` where `Callable` accepts `std::span<const T>`. Works only on POD nodes; otherwise returns `NotSupported`. No mutable span, no pop via span.
- **Ordering:** Single queue per node; FIFO preserved for inserts/takes. Span read snapshots the current prefix; does not pop.
- **Notifications:** On successful POD push/pop, still call `context_->notify` (same paths as today).
- **Errors (first iteration):**
  - `NotSupported`: path is generic or type mismatch or non-POD insert into POD node.
  - `InvalidType`: callable signature doesn’t match stored POD type.
  - `Busy`: reserved for later exclusivity paths.

## Tests ✅
1. **POD enqueue/dequeue:** insert int xN, then take<int> xN → ordered values, queue empty.
2. **Span read happy path:** insert ints, `read(span)` sees ordered data; repeat to ensure non-destructive.
3. **Type mismatch:** insert int, then insert float at same path → `NotSupported`, ints remain readable/takeable.
4. **Non-POD rejection:** first insert of struct returns `NotSupported`.
5. **Span guard:** after a non-POD insert on another path, ensure POD path still works; on a generic path, span read returns `NotSupported`.
6. **Concurrency smoke:** multi-threaded `insert<int>` + span reads; assert no lost/duplicated values (bounded run).

## Iteration 2 (current status) ✅
- **Automatic upgrade to generic:** On the first non-POD insert *or* first mismatched POD type, the node freezes the `PodPayload`, migrates all queued values into `NodeData` under `payloadMutex`, resets the fast-path handle, then appends the incoming value. FIFO order is preserved.
- **Type mixing policy:** Mixing POD types no longer errors; the first mismatch triggers the upgrade above. Subsequent inserts/reads go through `NodeData` for deterministic behavior.
- **Span semantics:** Span read remains read-only; no pop or mutable spans exposed in the public API.
- **Wait/notify tuning:** Notifications are now gated on active waiters to reduce redundant wakeups.

### Tests for Iteration 2
- Upgrade path: int → string triggers migration; FIFO preserved across the mixed queue.
- Type mixing: int → float upgrades and keeps ordering.
- Span read: existing read-only span callback still supported; no bulk-pop/mutable variants.
- Concurrency smoke: unchanged from iteration 1; still passes with new span paths.

## Iteration 3 (future) 🧭
_(no open items — add if/when new needs emerge)_

## Non-goals (now)
- Changing public API names (`read_mut`, etc.).
- Altering existing NodeData semantics or glob behavior beyond fast-path gate.
