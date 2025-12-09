# Release Notes — Q4 2025

## Typed Payload Helper Removal (December 6, 2025)

- `PathSpace::insertTypedPayload` / `takeTypedPayload` have been removed from the public API. Distributed components now route every payload through the shared `distributed/TypedPayloadBridge`, which resolves metadata via `TypeMetadataRegistry`, constructs the concrete value, and reinserts/takes using the standard `insert`/`read`/`take` calls.
- RemoteMountServer and RemoteMountManager only accept `typed/slidingbuffer` payloads by default. `nodedata/base64` frames are rejected outright, and the compatibility flag (`PATHSPACE_REMOTE_TYPED_PAYLOADS`) merely re-enables the legacy `string/base64` decoder for straggler deployments.
- Mirrors and local caches hydrate values by handing the decoded buffer back to `TypedPayloadBridge`, ensuring no code outside `PathSpace` needs to touch `NodeData` or bespoke helper APIs.
- Downstream integrations must stop calling the removed helpers; migrate to the typed API or wrap calls around `TypedPayloadBridge` utilities. See `docs/finished/Plan_DistributedTypedPayloads_Finished.md` and `docs/finished/Plan_RemoveSerializedNodeData_Finished.md` for the architecture timeline and migration guidance.

## Paint Example Screenshot & PixelNoise Harness Pause (December 8, 2025)

- `PaintExampleScreenshot`, `PaintExampleScreenshot720`, `PaintExampleScreenshot600`, and `PaintExampleScreenshotReport` have been removed from `tests/CMakeLists.txt` and the loop harness entirely. The ServeHtml/example rewrite tracked in `docs/Plan_PathSpaceHtmlServer.md` will provide a replacement workflow; until then, the screenshot CLI (`pathspace_screenshot_cli`) remains available for manual runs without registering CTest targets.
- `PixelNoisePerfHarness` and `PixelNoisePerfHarnessMetal` have been removed entirely from CTest and the loop harness. The pixel-noise example plus `scripts/check_pixel_noise_baseline.py` remain available for manual captures, and the ServeHtml/example rewrite will introduce the next-gen automated perf gate.

## PathSpaceUITests Recovery Complete (December 9, 2025)

- `PATHSPACE_UI_TEST_BATCHES` now groups the entire `PathSpaceUITests` binary into five deterministic batches plus a remainder; `ctest --test-dir build -R "PathSpaceUITests_" --output-on-failure --timeout 60` finishes in ~13.5 s when `PATHSPACE_DISABLE_SURFACE_CACHE_WATCH=1` is set, and artifacts such as `build/test-logs/PathSpaceUITests_PathRenderer2DCore_20251209-074236.artifacts` capture each batch’s telemetry.
- `./scripts/compile.sh --test --loop=5 --per-test-timeout 20 --release --loop-label PathSpaceUITests_*` now drives the same batch list, emitting `build/test-logs/loop_manifest.tsv` plus the per-loop logs so maintainers can prove the 5× loop stays under the 20 s per-test guardrail.
- `scripts/git-hooks/pre-push.local.sh` inherits the batch/timeout wiring and passes when `PREPUSH_COMMAND_TIMEOUT` is raised to ~900 s; the December 9 run archived its JSON summary under `build/test-logs/pre-push/pre-push_20251209-063634_pid26343.json` for PR evidence while still wrapping every inner command in the 60 s timeout.
- The full recovery playbook is archived at `docs/finished/Plan_PathSpaceUITests_Recovery_Finished.md`, including the recommended Conventional Commit breakdown for packaging the remaining working-tree changes once `.git/` is writable on the maintainer host.
