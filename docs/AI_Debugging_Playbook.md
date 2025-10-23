# Handoff Notice

> **Handoff note (October 19, 2025):** These troubleshooting steps remain accurate but assume the prior tooling defaults. Confirm any helper script changes in `docs/AI_Onboarding_Next.md` before relying on this playbook.
> **Update (October 21, 2025):** Presenter telemetry now mirrors into window diagnostics sinks (`windows/<win>/diagnostics/metrics/live/views/<view>/present/*`); see §3 for usage.

# PathSpace — Debugging Playbook

> **Context update (October 18, 2025):** The test harness now captures detailed logs for every failure via `scripts/run-test-with-logs.sh`. Use this playbook to triage regressions, inspect diagnostics, and close the loop with the mandated 15× test runs.
> **Diagnostics refresh (October 19, 2025):** Dirty-rect hints are coalesced server-side, and the extended damage/fingerprint/progressive metrics are available when `PATHSPACE_UI_DAMAGE_METRICS=1`. Keep the flag unset during perf runs—the software encode pass is still single-threaded today, so we’re focusing on parallelising it next.

This guide consolidates the practical steps for investigating failures across unit tests, UI targets, and runtime metrics. Pair it with the architecture docs (`docs/AI_Architecture.md`, `docs/Plan_SceneGraph_Renderer.md`) when you need deeper background.

## 1. Reproducing and Capturing Failures

### 1.1 Standard looped test run

```bash
./scripts/compile.sh --test --loop=15 --per-test-timeout=20
```

- Runs both `PathSpaceTests` and `PathSpaceUITests` each iteration.
- Logs land under `build/test-logs/` using the pattern `<test>_loop<iteration>of<total>_<timestamp>.log` if a failure occurs.
- `PATHSPACE_LOG` defaults to `1` in the helper so tagged logging is enabled when an error surfaces; adjust via `--env PATHSPACE_LOG=0` if you need silence.
- Want to exercise the Metal presenter path locally? Append `--enable-metal-tests` (macOS only) so the helper sets `PATHSPACE_UI_METAL=ON` during configuration and runs the suites with `PATHSPACE_ENABLE_METAL_UPLOADS=1`.

### 1.2 Single test reproduction

Use doctest filters with the wrapper when isolating a case:

```bash
./scripts/run-test-with-logs.sh \
  --label PathSpaceTests.read \
  --log-dir build/test-logs \
  --timeout 20 \
  -- \
  ./build/tests/PathSpaceTests --test-case "PathSpace read blocking"
```

Environment knobs (all respected by the wrapper and the logger):

| Variable | Purpose |
| --- | --- |
| `PATHSPACE_LOG` | Enable/disable log emission (truthy values: `1`, `true`, `on`, etc.). |
| `PATHSPACE_LOG_ENABLE_TAGS` | Comma-separated allowlist (e.g., `TEST,WAIT`). |
| `PATHSPACE_LOG_SKIP_TAGS` | Comma-separated blocklist. |
| `PATHSPACE_TEST_TIMEOUT` | Millisecond poll interval for blocking tests (default `1`). |
| `MallocNanoZone` | Set to `0` to reduce allocation overhead on macOS (default from helper). |
| `PATHSPACE_ENABLE_METAL_UPLOADS` | Opt-in Metal texture uploads during UI tests; leave unset in CI/headless runs so builders fall back to the software raster. |

## 2. Inspecting Collected Logs

1. Open the saved log file (e.g., `build/test-logs/PathSpaceTests_loop3of15_20251018-161200.log`).
2. The tail section is also echoed to stderr at the time of failure; the full file includes tagged entries (`[TEST][INFO] …`) plus doctest progress markers.
3. When ASAN/UBSAN is enabled, the helper preserves the sanitizer output verbatim. Re-run with `PATHSPACE_ENABLE_CORES=1` to generate core dumps for deeper analysis.

## 3. PathSpace Runtime Diagnostics

- **Structured errors:** Renderer/presenter components publish `PathSpaceError` payloads under `renderers/<rid>/targets/<tid>/diagnostics/errors/live`. In tests, call `Diagnostics::ReadTargetMetrics` to fetch the payload and correlate with frame indices.
- **Per-target metrics:** `renderers/<rid>/targets/<tid>/output/v1/common/*` stores the latest `frameIndex`, `revision`, `renderMs`, progressive copy counters (including `progressiveTileSize`, `progressiveWorkersUsed`, `progressiveJobs`), encode fan-out stats (`encodeWorkersUsed`, `encodeJobs`), backend telemetry (`backendKind`, `usedMetalTexture`), GPU timings (`gpuEncodeMs`, `gpuPresentMs`), present policy outcomes, and—when `PATHSPACE_UI_DAMAGE_METRICS=1` is set—damage/fingerprint telemetry (`damageRectangles`, `damageCoverageRatio`, `fingerprint*`, `progressiveTiles*`). Use `PathWindowView` doctest helpers or the builders’ diagnostics reader to dump these values.
- **Window diagnostics sinks:** Presenter metrics mirror into `windows/<win>/diagnostics/metrics/live/views/<view>/present/*`, including frame indices, present/render timings, progressive counters, last error strings, and timestamps, so dashboards can monitor windows without crawling every renderer target. With `PATHSPACE_UI_DAMAGE_METRICS=1` the mirror also carries the tile-damage diagnostics (`progressiveTiles{Dirty,Total,Skipped}`, `progressiveTilesUpdated`, `progressiveBytesCopied`, encode worker/job counts) alongside the core progressive copy telemetry.
- **Scene dirty state:** `scenes/<sid>/diagnostics/dirty/state` and `scenes/<sid>/diagnostics/dirty/queue` expose layout/build notifications. The `Scene::MarkDirty` doctests show how to wait on these paths without polling.
- **HTML/IO logs:** Application surfaces write to `<app>/io/log/{error,warn,info,debug}`. The global mirrors live at `/system/io/std{out,err}`; see `docs/AI_Paths.md` §2 for the canonical layout.

## 4. Workflow Checklist After a Failure

1. **Collect artifacts**
   - Inspect the saved log file(s) under `build/test-logs/`.
   - Preserve any core dumps or sanitizer traces (enable with `PATHSPACE_ENABLE_CORES=1` if needed).
2. **Correlate with diagnostics**
   - Query `diagnostics/errors/live` and `output/v1/common/*` for affected targets.
   - For scene issues, inspect `diagnostics/dirty/*` to confirm dirty markers and queues behave as expected.
3. **Isolate the regression**
   - Re-run the failing test with doctest filters and specific tags enabled (`PATHSPACE_LOG_ENABLE_TAGS=TEST,UI`).
   - Use `--loop=3` on the helper to confirm the fix eliminates intermittent races before scaling back to the mandated 15.
4. **Document findings**
   - Update the relevant plan doc (`docs/Plan_SceneGraph_Implementation.md` or task entry) with repro steps, log references, and next actions.

## 5. Tooling Quick Reference

| Task | Command |
| --- | --- |
| Run both suites once with logs | `./scripts/compile.sh --test` |
| Run both suites 15× with 20 s timeout | `./scripts/compile.sh --test --loop=15 --per-test-timeout=20` |
| Run suites once with Metal presenter enabled (macOS) | `./scripts/compile.sh --enable-metal-tests --test` |
| Run a single suite via CTest | `ctest --output-on-failure -R PathSpaceUITests` |
| Re-run failed tests only | `ctest --rerun-failed --output-on-failure` |
| Verify HTML adapter command stream | `ctest -R HtmlCanvasVerify --output-on-failure` |
| Verify HSAT asset inspection tooling | `ctest -R HtmlAssetInspect --output-on-failure` |
| Inspect HSAT payload contents manually | `./build/pathspace_hsat_inspect --input <payload.hsat>` |
| Tail latest failure log | `ls -t build/test-logs | head -1 | xargs -I{} tail -n 80 build/test-logs/{}` |
| Inspect renderer metrics path | `build/tests/PathSpaceUITests --test-case Diagnostics::ReadTargetMetrics` |
| Benchmark damage/fingerprint metrics | `./build/benchmarks/path_renderer2d_benchmark --metrics [--canvas=WIDTHxHEIGHT]` |
| Capture pixel-noise baseline JSON | `./scripts/capture_pixel_noise_baseline.sh` |
| Check pixel-noise run against budgets | `python3 scripts/check_pixel_noise_baseline.py --build-dir build` |
| Capture pixel-noise PNG frame | `./build/pixel_noise_example --headless --frames=1 --write-frame=docs/images/perf/pixel_noise.png` |

### 5.1 Pixel Noise Perf Harness Baselines

- Use `./scripts/capture_pixel_noise_baseline.sh` after rebuilding (`cmake --build build -j`) to refresh `docs/perf/pixel_noise_baseline.json`.
- Pass `--backend=metal` (and export `PATHSPACE_ENABLE_METAL_UPLOADS=1`) to capture the Metal2D variant in `docs/perf/pixel_noise_metal_baseline.json`; commit both baselines together when budgets shift.
- The helper launches `pixel_noise_example` headless with the standard perf budgets (≥50 FPS, ≤20 ms render/present) and records the run via `--write-baseline=<path>`.
- `python3 scripts/check_pixel_noise_baseline.py --build-dir build` reruns the harness with the recorded parameters, writes a temporary metrics snapshot, and fails if the averaged frame times exceed the stored budgets or if FPS dips below the baseline threshold. The script respects the baseline’s `backendKind`, forwarding the matching `--backend` flag and enabling Metal uploads automatically when needed. `PixelNoisePerfHarness` (software) and `PixelNoisePerfHarnessMetal` (Metal, PATHSPACE_UI_METAL builds) in CTest now go through the same script so regressions surface during the 15× loop.
- Inspect the resulting JSON for:
  - `summary.*` — aggregate FPS and timing averages used to confirm budgets.
  - `tileStats.*` — progressive tiling activity (tiles updated/dirty/skipped/copied, worker/job counts) for spotting regression hot spots.
  - `residency.*` — mirrored metrics from `diagnostics/metrics/residency/*` so residency budget drifts surface during perf reviews.
- Pair the JSON with the console summary emitted by the example (`pixel_noise_example: summary …`) when reporting numbers in PRs.
- When updating visuals, run `./build/pixel_noise_example --headless --frames=1 --write-frame=docs/images/perf/pixel_noise.png` to refresh the canonical PNG shown in docs; the flag enables framebuffer capture automatically.
- Keep older baselines committed when behaviour intentionally shifts; diffing JSON across commits highlights the magnitude of a regression or improvement.

## 6. Closing the Loop

Always finish a debugging session by:

1. Re-running the full loop: `./scripts/compile.sh --test --loop=15 --per-test-timeout=20`
2. Verifying no new logs were emitted (`find build/test-logs -type f -mmin -5` should be empty on success).
3. Recording the outcome (pass/fail, relevant paths, log snippets) in the working note or PR description for traceability.

With the shared test runner and this playbook, every failure leaves behind actionable artifacts, keeping the UI/renderer hardening efforts measurable and repeatable.

## 7. Current Blockers (October 20, 2025)

- None. The HTML asset hydration failure was cleared on October 20, 2025 by introducing a dedicated `Html::Asset` serialization codec (`include/pathspace/ui/HtmlSerialization.hpp`) and a regression test (`Html::Asset vectors survive PathSpace round-trip`). Reconfirm with `ctest --test-dir build --output-on-failure -R "Html::Asset"` or the full `PathSpaceUITests` loop when touching the renderer stack.
