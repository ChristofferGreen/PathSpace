# Plan: Remove Scene Build Snapshots from PathSpace

## Goal
Keep PathSpace limited to shared/runtime-visible data by moving scene build snapshots (`scenes/<id>/builds/<rev>`) into the renderer’s internal cache. PathSpace should expose only lightweight metadata (`current_revision`, optional publish metrics), while presenters/screenshot/inspector paths read snapshots directly from the renderer store.

## Current problem
- `SceneLifecycle` writes per-revision build nodes into PathSpace even though only the renderer/presenter consumes them.
- Dumps are noisy (`builds/000...01/*`) and inflate regression fixtures.
- Renderer already maintains `RendererSnapshotStore`; PathSpace duplication is redundant and risks staleness.

## Desired state
- No `builds/*` under `scenes/<id>` in PathSpace.
- PathSpace retains `current_revision` (and, if useful, `last_publish_time`/metrics).
- Presenter/inspector/screenshot code pulls snapshots from `RendererSnapshotStore` (or successor) only.
- JSON dumps stay lean; tests/fixtures updated accordingly.

## Work items (✖ not started, ⏳ in progress, ✔ done)
- ✔ Inventory producers/consumers: locate all writes to `scenes/*/builds` and all reads (presenter, inspector, screenshot, exporter/tests).
- ✔ Implementation: stop writing builds to PathSpace; keep `current_revision` + metadata in `current_revision_desc`; renderer cache owns snapshot lifetime/GC.
- ⏳ Consumers: update presenter/inspector/screenshot paths to fetch from renderer cache; drop PathSpace fallbacks (confirm no hidden `builds/*` assumptions remain).
- ⏳ Tests/fixtures: refresh dump regressions to expect no `builds/*`; add regression asserting `current_revision` present without builds.
- ⏳ Docs: update `Widget_Schema_Reference.md`, related finished plans, and archive this plan once complete. (`AI_PATHS.md` scene row already updated.)
- ⏳ Validation: build, run button dump, and full 5× test loop.

## Validation checklist
- `cmake --build build -j`
- `./build/declarative_button_example --dump_json > out.json` (no `scenes/<id>/builds/*`; `current_revision` present)
- `./scripts/compile.sh --release --loop=5 --per-test-timeout 20`

## Acceptance criteria
- PathSpace tree contains no `builds/*` nodes under scenes.
- Renderer cache is the sole source of scene snapshots; presenter/screenshot paths succeed via the cache.
- Updated regressions/dumps reflect the leaner scene subtree; test loop green.
