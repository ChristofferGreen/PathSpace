# Plan — Remove `insertTypedPayload` / `takeTypedPayload`

> **Drafted:** December 6, 2025  
> **Owner:** PathSpace distributed/core maintainers  
> **Status:** Completed — helpers removed, docs/release notes updated

## Motivation

`insertTypedPayload` and `takeTypedPayload` were introduced as “typed” replacements for the legacy serialized NodeData helpers. They still bypass the public `insert/read/take` API surface and duplicate logic that already exists in `InputMetadata` (`serialize`/`deserialize`). To keep PathSpace’s API surface minimal and enforce the invariant that only the standard typed operations manipulate data, these helpers should be removed once remote mounts and mirrors rely exclusively on the typed payload protocol.

## Goals

1. Eliminate `insertTypedPayload` / `takeTypedPayload` from `PathSpace` **without introducing any new public helpers**—all functionality must route through the existing typed API (`insert`, `read`, `take`).
2. Ensure all distributed/mirroring components use the regular typed API (via `InputMetadata` callbacks) for encoding/decoding payloads.
3. Update docs/tests/finished plans (`Plan_RemoveSerializedNodeData_Finished`, `Plan_DistributedTypedPayloads_Finished`, `Memory.md`) to reflect the final state: no special helpers, just typed payload transport over the public API.

### Status — December 6, 2025

- ✅ `RemoteMountServer` + `RemoteMountManager` now call the standard `insert`/`take` APIs through a new `distributed/TypedPayloadBridge` that deserializes bytes via `InputMetadata`, instantiates the concrete value (type-erased storage backed by per-type constructors), and reserializes values on the way out. The bridge leans on `TypeMetadataRegistry` to expose runtime size/alignment plus type-specific insert/take dispatchers.
- ✅ `TypeMetadataRegistry` entries now track `TypeOperations` (size, alignment, construct/destroy, insert/take) and `InputData` automatically registers every type that flows through the public API so remote components never need to touch `PathSpace` internals again.
- ✅ `PathSpace::insertTypedPayload` / `PathSpace::takeTypedPayload` have been deleted; all remaining callers route through the typed API and NodeData stays encapsulated inside `Leaf`.
- ✅ Docs (`docs/finished/Plan_DistributedTypedPayloads_Finished.md`, `docs/finished/Plan_RemoveSerializedNodeData_Finished.md`, `docs/Memory.md`, and `docs/ReleaseNotes_Q4_2025.md`) now describe the bridge + registry changes instead of pointing readers at the removed helpers. Release notes call out the API removal so downstream teams notice during upgrades.
- ✅ Tests continue to exercise the typed-transport path via the existing distributed suites (`tests/unit/distributed/test_RemoteMountServer.cpp` / `test_RemoteMountManager.cpp`). We will extend coverage only if new edge cases (e.g., non-default-constructible types, invalid metadata) surface.

## Current Usage Snapshot

- `RemoteMountServer` and `RemoteMountManager` now rely exclusively on `distributed/TypedPayloadBridge` for remote inserts/takes; they no longer touch the deleted helpers.
- Treat any `rg "takeTypedPayload"` / `rg "insertTypedPayload"` hits outside `src/pathspace/distributed/TypedPayloadBridge.*` as regressions.

## Migration Strategy

1. **Protocol/metadata certainty**
   - Confirm the remote protocol now carries the `type_name` / schema info needed to pick the correct `InputMetadata` on both sides. (Already true per `docs/finished/Plan_DistributedTypedPayloads_Finished.md`.)
   - Ensure the `PATHSPACE_REMOTE_TYPED_PAYLOADS` flag has been rolled out and typed transport is the default everywhere.

2. **Refactor RemoteMountServer**
   - Replace calls to `insertTypedPayload` / `takeTypedPayload` with internal helper functions that:
     - Resolve `InputMetadata` by `type_name` (or fail with `InvalidType`).
     - Allocate buffers, call the metadata’s `deserialize`/`serialize` callbacks, and invoke `space.insert` / `space.take` with concrete types.
   - Move any shared utilities into a private namespace within `RemoteMountServer.cpp` so other components can reuse them without touching PathSpace internals.

3. **Refactor RemoteMountManager / mirror code**
   - Similar to the server, add local helpers that translate `ValuePayload` blobs into typed inserts/reads using `InputMetadata` resolution.
   - Ensure mirror assignments, take caches, and execution payload handling all route through the typed API.

4. **Delete the helpers**
   - Remove the declarations/definitions from `PathSpace.hpp/.cpp`.
   - Update any include directives or friend declarations that existed solely for these helpers.

5. **Documentation & plan updates**
   - Update `docs/finished/Plan_RemoveSerializedNodeData_Finished.md`, `docs/finished/Plan_DistributedTypedPayloads_Finished.md`, `docs/Memory.md`, and release notes to state that no helper APIs remain—the typed payload protocol now uses the standard API exclusively.
   - Note the change in `docs/finished/Plan_RemoveRelocateSubtree_Finished.md` if it references the helpers as interim steps.
   > **Update (December 6, 2025):** Completed via the documentation sweep that refreshed both finished plans, `docs/Memory.md`, and the new `docs/ReleaseNotes_Q4_2025.md`. `Plan_RemoveRelocateSubtree_Finished.md` never referenced the helpers, so no edits were required there.

6. **Testing**
   - Extend distributed unit tests to cover the new helper-less pathways (typed insert/take to/from remote).
   - Add regression tests that ensure invalid `type_name` or malformed payloads trigger the same errors as today.

7. **API guardrail**
   - The migration must not add replacement helpers. If callers need ergonomic wrappers, they should live entirely within the distributed components and ultimately funnel through `insert/read/take`.

## Risks & Mitigations

- **Type resolution mismatch:** If RTTI names diverge between clients and servers, typed inserts will fail. Mitigate by providing a stable type identifier mapping (e.g., optional schema IDs) or by documenting the requirement for matching toolchains.
- **Performance regressions:** The helpers currently manipulate `SlidingBuffer` directly. Replacing them with typed insert/take calls should be comparable, but profile remote insert/take throughput to ensure no regressions.
- **Hidden dependencies:** Downstream forks might call the helpers. Communicate the removal in release notes and provide guidance on using the typed protocol instead.

## Next Steps

1. Secure maintainer agreement on removing the helpers. ✅
2. Break the migration into implementation tickets (server refactor, manager refactor, helper deletion, doc updates). ✅ — server/manager refactors, helper deletion, and the documentation sweep all landed in this revision.
3. Execute, test, and announce in release notes. ✅ — finished plans, `docs/Memory.md`, and `docs/ReleaseNotes_Q4_2025.md` now describe the bridge + registry behavior; the standard compile/test loop continues to guard the distributed path.
