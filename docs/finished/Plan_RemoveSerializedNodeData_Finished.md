# Plan — Remove Serialized NodeData APIs

> **Drafted:** December 6, 2025  
> **Owner:** PathSpace maintainer pairing  
> **Status:** Completed — NodeData helpers removed December 6, 2025

## Background

The `PathSpace::insertSerializedNodeData` and `PathSpace::takeSerializedNodeData` helpers were introduced as a stopgap so distributed mounts, mirrors, and tooling could manipulate raw `NodeData` snapshots. With the new "Typed Distributed Payloads" plan in motion, these helpers are redundant and actively violate the requirement that nothing outside `PathSpaceBase` should touch `NodeData`.

## Objective

Remove both helpers from the public API and migrate every caller to the typed transport flow, ensuring no external component depends on `NodeData`.

## Completion Summary — December 6, 2025

- Added a typed payload transport built on the public `insert`/`take` API (distributed components resolve metadata via `TypeMetadataRegistry` and the new bridge utilities), keeping `NodeData` fully encapsulated inside `PathSpace`.
- `RemoteMountServer` and `RemoteMountManager` now emit and consume only typed sliding-buffer payloads; legacy `nodedata/base64` frames are rejected with an `InvalidType` error and no longer appear in metrics or docs.
- Mirrors, snapshots, and documentation were updated to reflect the typed-only transport, and `docs/Memory.md` records the removal for future onboarding reference.
- Validation remains covered by the standard `scripts/compile.sh --loop` pre-push gate; no ad-hoc serialized helper references remain when running `rg insertSerializedNodeData` / `rg takeSerializedNodeData`.
- `docs/ReleaseNotes_Q4_2025.md` now flags the helper removal so downstream deployments see the change in the published upgrade notes.

## Scope

1. **API changes**
   - Delete `insertSerializedNodeData` / `takeSerializedNodeData` declarations from `PathSpace.hpp` and their implementations.
   - Remove any friend declarations or includes that existed solely for those helpers (`PathSpaceInternalAccess`, etc.).

2. **Runtime refactors**
   - Update `RemoteMountServer` to call typed `insert`/`take` once the typed payload protocol lands.
   - Update `RemoteMountManager` mirror, insert, take, and cache logic to rely solely on typed serialization callbacks (`InputMetadata`).
   - Ensure notification streaming, snapshots, and diagnostics no longer mention `nodedata/base64`.

3. **Documentation**
   - Clean up `docs/Memory.md`, finished plans, and onboarding docs that reference the serialized helpers.
   - Cross-link this removal plan with `docs/Plan_DistributedTypedPayloads.md` so future readers know the dependency order (typed payloads first, API removal second).

4. **Testing**
   - Extend distributed unit tests to cover the typed flow and confirm no code path reaches for the legacy helpers.
   - Add regression cases that attempt to call the removed APIs (ensuring compilation fails) so downstream projects notice immediately.

## Sequencing

1. **Prerequisite:** Deliver the typed payload protocol (per the companion plan). Without the new encoding, remote mounts have no alternative way to move user-defined types.
2. **Soft deprecation (completed December 6, 2025):**
   - Serialized helpers are deprecated in headers and emit runtime warnings upon their first invocation per process.
   - Migration guidance now points callers to the typed remote payload transport documented in `docs/finished/Plan_DistributedTypedPayloads_Finished.md` and `docs/finished/Plan_Distributed_PathSpace_Finished.md`.
3. **Removal:** delete the helpers, update build files, and remove any include guards or tests referencing them.
4. **Post-removal validation:** run the mandated compile/test loops and ensure no binary references the removed symbols (`nm | rg insertSerializedNodeData`).

## Risks & Mitigations

- **Downstream integrations:** some out-of-tree tools may still link against the helpers. Mitigate by flagging the change in release notes and offering the deprecation window.
- **Protocol skew:** removing the helpers before typed payloads ship would brick remote mounts. This plan explicitly depends on completing the typed payload overhaul first.
- **Performance regressions:** typed serialization might introduce extra copies compared to raw `NodeData`. Benchmark remote mount insert/take loops after the migration and optimize `InputMetadata` paths if needed.

## Exit Criteria

1. No references to `insertSerializedNodeData` or `takeSerializedNodeData` remain in the repository.
2. All distributed code paths rely on typed payload encoding.
3. Documentation and release notes clearly describe the removal and the new alternative.
4. Full PathSpace test suite (including distributed tests and ServeHtml flows) passes under the mandated loop settings.
