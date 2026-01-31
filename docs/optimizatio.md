# Optimization Log

Tracking optimization experiments and benchmark outcomes. Only changes that show a clear, repeatable win should land in code.

## Bench Harness
- Command: `./build/bench/PathSpaceBench --runs 10 --warmup 1 --scale 1.0`
- Scenarios: Wide tree, Nested chain, Nested fanout

## Experiments

### 2026-01-31 — Release benchmark baseline (no ASAN)
- Build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DENABLE_ADDRESS_SANITIZER=OFF`
- Run: `./build-release/bench/PathSpaceBench --runs 10 --warmup 1 --scale 1.0`
- Best-of-3 mean results:
  - Wide tree: build 266.17 ms, read 15.63 ms, total 281.80 ms
  - Nested chain: build 55.19 ms, read 3.52 ms, total 58.71 ms
  - Nested fanout: build 34.95 ms, read 2.07 ms, total 37.03 ms

### 2026-01-31 — Nested iterator construction micro-opt
- Change: Avoid intermediate `std::string` when routing into nested spaces by constructing `Iterator` storage directly.
- Result: Mixed. Wide tree read improved in one run, but nested fanout regressed and overall totals were flat to worse. Not kept.
- Status: Reverted.

### 2026-01-31 — Avoid baseName string allocations in Leaf
- Change: Use `std::string_view` for path component lookups and update `append_index_suffix` to accept `std::string_view`.
- Baseline (revert run):
  - Wide tree: build 429.20 ms, read 31.82 ms, total 461.02 ms
  - Nested chain: build 90.88 ms, read 5.84 ms, total 96.72 ms
  - Nested fanout: build 57.63 ms, read 3.28 ms, total 60.92 ms
- After change:
  - Wide tree: build 427.75 ms, read 29.46 ms, total 457.21 ms
  - Nested chain: build 89.53 ms, read 5.74 ms, total 95.26 ms
  - Nested fanout: build 57.37 ms, read 3.07 ms, total 60.44 ms
- Result: Consistent small win across all scenarios in this run. Keep.

### 2026-01-31 — Node allocation pool (global free list)
- Change: Add a simple global free list for `Node` allocations via `operator new/delete`.
- Release benchmark comparison (best-of-3 means):
  - Baseline:
    - Wide tree: build 266.17 ms, read 15.63 ms, total 281.80 ms
    - Nested chain: build 55.19 ms, read 3.52 ms, total 58.71 ms
    - Nested fanout: build 34.95 ms, read 2.07 ms, total 37.03 ms
  - After change:
    - Wide tree: build 249.60 ms, read 14.03 ms, total 263.67 ms
    - Nested chain: build 52.65 ms, read 3.31 ms, total 55.96 ms
    - Nested fanout: build 33.27 ms, read 1.94 ms, total 35.23 ms
- Result: Clear win across build/read/total in Release. Keep.
