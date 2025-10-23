# Pixel Noise Baselines

This directory stores captured performance baselines for the pixel noise example
(`examples/pixel_noise_example.cpp`). Each baseline is recorded with
`scripts/capture_pixel_noise_baseline.sh`, which invokes the example in
headless mode, enforces the standard budgets (≥50 FPS, ≤20 ms average
present/render) and writes a JSON payload with the captured metrics.

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
