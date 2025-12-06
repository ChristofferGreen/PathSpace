# Release Notes — Q4 2025

## Typed Payload Helper Removal (December 6, 2025)

- `PathSpace::insertTypedPayload` / `takeTypedPayload` have been removed from the public API. Distributed components now route every payload through the shared `distributed/TypedPayloadBridge`, which resolves metadata via `TypeMetadataRegistry`, constructs the concrete value, and reinserts/takes using the standard `insert`/`read`/`take` calls.
- RemoteMountServer and RemoteMountManager only accept `typed/slidingbuffer` payloads by default. `nodedata/base64` frames are rejected outright, and the compatibility flag (`PATHSPACE_REMOTE_TYPED_PAYLOADS`) merely re-enables the legacy `string/base64` decoder for straggler deployments.
- Mirrors and local caches hydrate values by handing the decoded buffer back to `TypedPayloadBridge`, ensuring no code outside `PathSpace` needs to touch `NodeData` or bespoke helper APIs.
- Downstream integrations must stop calling the removed helpers; migrate to the typed API or wrap calls around `TypedPayloadBridge` utilities. See `docs/finished/Plan_DistributedTypedPayloads_Finished.md` and `docs/finished/Plan_RemoveSerializedNodeData_Finished.md` for the architecture timeline and migration guidance.
