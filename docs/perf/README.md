# Pixel Noise Baselines

This directory stores captured performance baselines for the pixel noise example
(`examples/pixel_noise_example.cpp`). Each baseline is recorded with
`scripts/capture_pixel_noise_baseline.sh`, which invokes the example in
headless mode, enforces the standard budgets (≥25 FPS, ≤20 ms average
present/render, ≤40 ms present-call after the shaped text rollout) and writes a JSON payload with the captured metrics.

## Recording a Baseline

```bash
./scripts/capture_pixel_noise_baseline.sh
```

By default the script saves the output to
`docs/perf/pixel_noise_baseline.json`. Adjust `OUTPUT_PATH`, `FRAMES`,
`WIDTH`, `HEIGHT`, or the budget environment variables to capture alternative
profiles.

For the Metal2D baseline, export `PATHSPACE_ENABLE_METAL_UPLOADS=1`,
set `OUTPUT_PATH=docs/perf/pixel_noise_metal_baseline.json`, and pass
`--backend=metal` so the example exercises GPU uploads while recording the run.

The JSON payload includes:

- Command parameters used for the run (surface size, seed, budgets, frame count)
- Aggregate timing (average FPS, render/present/present-call timings)
- Progressive tiling summaries (tiles updated/dirty/skipped/copied, worker counts)
- Residency metrics mirrored from `diagnostics/metrics/residency/*`
- Last error fields surfaced via `Diagnostics::ReadTargetMetrics`

## Usage

- Commit updated baselines when renderer or presenter changes result in stable,
  intended metric shifts. Mention the change in the PR description along with
  the captured command line.
- When investigating regressions, compare the latest JSON against prior commits
  and re-run the script locally to confirm improvements.

## Capturing a Frame Grab

Use the pixel noise example’s PNG writer to capture a representative frame that
matches the current baselines:

```bash
./build/pixel_noise_example --headless --frames=1 \
  --write-frame=docs/images/perf/pixel_noise.png
```

The `--write-frame` flag enables framebuffer capture automatically and writes
an RGBA PNG using the current surface size. Commit refreshed images alongside
baseline JSON updates so perf regressions include both metrics and visuals.

# Renderer FPS Traces

Enable benchmark targets during configure (`-DBUILD_PATHSPACE_BENCHMARKS=ON`)
and capture PathRenderer2D traces for the standard canvases:

```bash
./scripts/capture_renderer_fps_traces.py --pretty
```

The script writes `docs/perf/renderer_fps_traces.json`. Latest capture
(2025-12-10, post-FontManager shaping) recorded:

- 1280x720: full repaint ~103 FPS, incremental ~1423 FPS.
- 3840x2160: full repaint ~13.6 FPS, incremental ~712 FPS.

Use these baselines to flag regressions after shaping or renderer changes.

# Widget Pipeline Benchmark

Build with `-DBUILD_PATHSPACE_BENCHMARKS=ON` and run the declarative pipeline
benchmark to track widget mutation and bucket synthesis costs:

```bash
./build/benchmarks/widget_pipeline_benchmark \
  --write-json docs/perf/widget_pipeline_benchmark.json --verbose
```

The JSON records declarative throughput and paint GPU upload timing. Latest
capture (2025-12-10) measured bucketAvgMs ~0.118 ms, bucketBytesPerIter
~11.7 KB, dirtyWidgetsPerSec ~6.8k, and paintGpuLastUploadNs ~3.6 ms. Keep the
file under `docs/perf/` so future runs can diff against this baseline.
