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

### 2026-01-31 — Deep scenario run (partial)
- Command: `./build/bench/PathSpaceBench --runs 10 --warmup 1 --scale 1.0`
- Result: Process terminated with signal 9 after completing Wide tree; remaining scenarios not executed.
  - Wide tree: build 422.65 ms, read 24.34 ms, total 446.99 ms

### 2026-01-31 — Release benchmark single run (build-release)
- Build: `cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release`
- Run: `./build-release/bench/PathSpaceBench --runs 1 --warmup 1 --scale 1.0`
- Results (single run):
  - Wide tree: build 186.40 ms, read 9.13 ms, total 195.53 ms
  - Nested chain: build 37.34 ms, read 2.52 ms, total 39.86 ms
  - Nested fanout: build 27.09 ms, read 1.48 ms, total 28.57 ms

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

### 2026-01-31 — Skip retarget path building when no retargets
- Change: Avoid building `mountBase` and rebasing when nested insert returns no retargets.
- Release benchmark comparison (best-of-3 means):
  - Baseline (with node pool): 
    - Wide tree: build 249.60 ms, read 14.03 ms, total 263.67 ms
    - Nested chain: build 52.65 ms, read 3.31 ms, total 55.96 ms
    - Nested fanout: build 33.27 ms, read 1.94 ms, total 35.23 ms
  - After change:
    - Wide tree: build 250.00 ms, read 12.20 ms, total 262.19 ms
    - Nested chain: build 54.90 ms, read 3.42 ms, total 58.34 ms
    - Nested fanout: build 34.32 ms, read 2.10 ms, total 36.41 ms
- Result: Regression on nested chain/fanout totals. Not kept.

### 2026-01-31 — Thread-local node free list
- Change: Add a small per-thread free list for `Node` allocations to reduce global pool contention.
- Release benchmark comparison (best-of-3 means):
  - Baseline (global pool):
    - Wide tree: build 249.60 ms, read 14.03 ms, total 263.67 ms
    - Nested chain: build 52.65 ms, read 3.31 ms, total 55.96 ms
    - Nested fanout: build 33.27 ms, read 1.94 ms, total 35.23 ms
  - After change:
    - Wide tree: build 254.69 ms, read 13.01 ms, total 268.35 ms
    - Nested chain: build 53.01 ms, read 3.33 ms, total 56.33 ms
    - Nested fanout: build 32.61 ms, read 1.88 ms, total 34.54 ms
- Result: Mixed with regressions in wide tree and nested chain totals. Not kept.

### 2026-01-31 — Reduce `parallel_node_hash_map` submaps to 4
- Change: Lower `Node::DefaultSubmaps` from 12 → 4 to reduce map overhead.
- Result: ASAN abort in `test_PathSpace_execution` (container overflow). Reverted.

### 2026-01-31 — Array-backed trie prototype (bench-only)
- Change: Added a bench-only `--engine array` option that builds a flattened array trie (temp map build + sorted edge arrays).
- Notes: Prototype flattens nested spaces into a single path prefix; it is not a full nested-space model, so results are directional only. `--engine snapshot` is the apples-to-apples variant built from a real PathSpace visit.
- Run (debug build): `./build/bench/PathSpaceBench --runs 1 --warmup 1 --scale 0.5 --engine array`
- Comparison run: `./build/bench/PathSpaceBench --runs 1 --warmup 1 --scale 0.5 --engine pathspace`
- Results (array, scale 0.5):
  - Wide tree: build 0.02 ms, read 0.00 ms, total 0.02 ms
  - Deep chain: build 0.07 ms, read 0.01 ms, total 0.08 ms
  - Nested chain: build 0.02 ms, read 0.00 ms, total 0.02 ms
  - Nested fanout: build 0.03 ms, read 0.00 ms, total 0.03 ms
- Results (pathspace, scale 0.5):
  - Wide tree: build 1.77 ms, read 0.03 ms, total 1.80 ms
  - Deep chain: build 7.25 ms, read 0.22 ms, total 7.46 ms
  - Nested chain: build 1.76 ms, read 0.02 ms, total 1.79 ms
  - Nested fanout: build 2.82 ms, read 0.06 ms, total 2.88 ms
- Result: Array engine was significantly faster at this scale, but the workload is tiny and the semantics differ; needs a fairer comparison at scale 1.0 (current debug runs at scale 1.0 terminated with signal 9).

### 2026-01-31 — Snapshot array-backed trie (PathSpace visit)
- Change: Added `--engine snapshot` to build a read-only array trie by visiting the actual PathSpace (nested spaces included), then serve reads from the array snapshot.
- Run (build-release): `./build-release/bench/PathSpaceBench --runs 1 --warmup 1 --scale 0.5 --engine snapshot`
- Comparison run (build-release): `./build-release/bench/PathSpaceBench --runs 1 --warmup 1 --scale 0.5 --engine pathspace`
- Results (snapshot, scale 0.5, build-release):
  - Wide tree: build 2.40 ms, read 0.00 ms, total 2.40 ms
  - Deep chain: build 11.77 ms, read 0.00 ms, total 11.77 ms
  - Nested chain: build 2.67 ms, read 0.00 ms, total 2.67 ms
  - Nested fanout: build 3.78 ms, read 0.00 ms, total 3.78 ms
- Results (pathspace, scale 0.5, build-release):
  - Wide tree: build 0.69 ms, read 0.01 ms, total 0.71 ms
  - Deep chain: build 3.18 ms, read 0.09 ms, total 3.27 ms
  - Nested chain: build 0.82 ms, read 0.01 ms, total 0.82 ms
  - Nested fanout: build 1.15 ms, read 0.02 ms, total 1.17 ms
- Result: Snapshot array is much faster on reads but adds significant build overhead at this scale; scale 1.0 runs in build-release currently terminate with signal 9, so we still need a larger-scale comparison.

### 2026-01-31 — Benchmark safety caps
- Change: Added `--max-inserts` (default 10,000; pass `0` for unlimited) and read-path limiting so scale 1.0 runs do not exhaust memory.
- Rationale: The Deep chain scenario can generate millions of values at scale 1.0; the cap keeps the benchmark runnable while still exercising the traversal code paths.

### 2026-01-31 — Snapshot array vs PathSpace (scale 1.0 with caps)
- Run (build-release): `./build-release/bench/PathSpaceBench --runs 1 --warmup 1 --scale 1.0 --engine pathspace`
- Run (build-release): `./build-release/bench/PathSpaceBench --runs 1 --warmup 1 --scale 1.0 --engine snapshot`
- Note: Uses default `--max-inserts 10000` safety cap.
- Results (pathspace, scale 1.0, build-release):
  - Wide tree: build 178.50 ms, read 11.22 ms, total 189.72 ms
  - Deep chain: build 1225.99 ms, read 131.24 ms, total 1357.23 ms
  - Nested chain: build 56.15 ms, read 3.39 ms, total 59.54 ms
  - Nested fanout: build 35.18 ms, read 2.01 ms, total 37.19 ms
- Results (snapshot, scale 1.0, build-release):
  - Wide tree: build 557.35 ms, read 0.35 ms, total 557.70 ms
  - Deep chain: build 6406.80 ms, read 1.51 ms, total 6408.31 ms
  - Nested chain: build 205.07 ms, read 0.06 ms, total 205.13 ms
  - Nested fanout: build 104.86 ms, read 0.04 ms, total 104.90 ms
- Result: Snapshot array has dramatically faster reads but substantially higher build time; total time is worse with the current snapshot build path at this cap.
