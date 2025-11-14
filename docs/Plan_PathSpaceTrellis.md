# PathSpace Trellis — Redesign Notes

## Context
- Goal: expose one logical trellis path that drains values from multiple concrete source paths (fan-in) while delegating storage to an existing `PathSpaceBase`.
- Simplicity first: no descriptors, persistence, or internal mirror spaces from the abandoned implementation. We just need concurrency-safe source registration plus fair draining.
- Previous design is archived in `docs/finished/Plan_PathSpace_FanIn_Abandoned.md` for historical reference only.

## Architecture
- ✅ Usage pattern:
  ```cpp
  auto trellis = std::make_shared<PathSpaceTrellis>(space);
  space->insert<"/cursor">(trellis);
  space->insert<"/cursor/_system/enable">("/data/mouse");
  space->insert<"/cursor/_system/enable">("/data/gamepad");
  auto point = space->read<"/cursor", std::pair<int, int>>();
  ```
- ✅ `PathSpaceTrellis` inherits from `PathSpaceBase`.
- ✅ Constructor accepts `std::shared_ptr<PathSpaceBase> backing`.
- ✅ Source registry: `phmap::flat_hash_set<std::string>` storing canonicalized source paths guarded by a lightweight `std::shared_mutex` so updates and snapshots stay consistent.
- ✅ Control commands live under the trellis root:
  - `trellisRoot + "/_system/enable"` with payload `<sourcePath>` (string) adds the source after canonicalization.
  - `trellisRoot + "/_system/disable"` with payload `<sourcePath>` removes it (ignored if absent).
  - Writes to any other `_system/*` node return `Error::Code::InvalidArgument`.
- ✅ Canonicalization: commands accept absolute paths; trellis normalizes them to the canonical form used by `PathSpace` so string comparisons stay consistent.
- ✅ Data operations:
  - Writes to direct children (e.g., `/cursor/blah/taga`) bypass trellis routing and forward unchanged to the backing space—`PathSpaceTrellis` only intercepts the root path and its `_system/*` control nodes.
  - `insert` fans out copyable payloads to each registered source via `backing->insert(source, payload)`; move-only payloads go only to the first source selected by the round-robin cursor. If no sources are registered, return `Error::Code::NoObjectFound`.
  - ✅ `read` / `take` iterate sources in round-robin order (cursor held in `std::atomic<size_t>`), calling backing `read`/`take`. Blocking semantics:
    - try all sources without blocking first;
    - if still empty and caller provided `Block{timeout}`, poll each source using 1 ms slices deducted from the remaining time (granularity matches `Out::timeout`);
    - for infinite blocking, continue polling at the 1 ms slice until data arrives or `shutdown` is observed.
  - ✅ `notify` fans out to each registered source via `backing->notify(source)`.
  - ✅ `shutdown` toggles an atomic `isShutdown` flag and forwards to `backing->shutdown()`.
- ✅ Non-goals: persisted configs, stats, runtime descriptors, latest-mode semantics, extra layers (intentionally omitted).

## Validation Strategy
- ✅ `tests/unit/layer/test_PathSpaceTrellis.cpp`: current coverage includes
  1. Commands (`_system/enable` / `_system/disable` idempotency).
  2. Fan-in basics with round-robin reads.
  3. Mixed types (copyable + move-only payloads).
  4. Fan-out insert semantics (copy vs move-only).
  5. Bypass writes forwarding to backing space.
  6. Blocking read/take behaviour (poll loop exercised).
  7. Shutdown waking blocked readers.
  8. Concurrent producers/consumers stress with delivery accounting.
- ✅ Execution payload fan-in (`FutureAny` fan-out across sources).
- ✅ Notify fan-out instrumentation via recording PathSpace.
- ✅ Reconfiguration under producer load stress test.
- ⏳ Pending additions: explicit notify integration tests across external contexts (if needed).

Run the standard loop: `cmake --build build -j`, `ctest --test-dir build --output-on-failure -j --repeat-until-fail 15 --timeout 20`. Add a targeted doctest regex if we need faster iteration during development.

## Next Steps
1. Implement `PathSpaceTrellis` class/files and wire into build.
2. Flesh out the command handling + round-robin helper utilities.
3. Author the test suite above (reuse patterns from multithreading doctests).
4. Update overview/status docs once implementation lands.

## Status Tracking
- **2025-11-14** — Restarted design from scratch; plan documented here. Implementation pending.
