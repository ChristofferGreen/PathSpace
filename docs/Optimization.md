# Optimization Log

Tracking optimization experiments and benchmark outcomes. Only changes that show a clear, repeatable win should land in code.

## Benchmark Recipe (use this for every comparison)
Numbers are only comparable when they come from the same recipe. Do not mix Debug/ASan runs with Release runs.

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DENABLE_ADDRESS_SANITIZER=OFF
cmake --build build-release -j
./build-release/bench/PathSpaceBench --runs 10 --warmup 1 --scale 1.0
```
- Scenarios: Wide tree, Deep chain, Nested chain, Nested fanout. `--max-inserts` defaults to 10,000 (pass `0` for unlimited, memory permitting); `--engine pathspace|snapshot|array` selects the backend.
- Run the baseline and the change back to back on an idle machine; repeat 3 times and compare best-of-3 means.
- Treat differences under ~5% as noise. Single-run (`--runs 1`) numbers are directional only.
- Release (ASan OFF) runs are markedly faster than sanitized runs (the older log entries show about 1.6x), so results from `build/` are not comparable to the tables below.

## Summary of Experiments
| Date | Experiment | Outcome |
|------|------------|---------|
| 2026-01-31 | Release baseline (best-of-3, no ASan) | Reference: Wide 281.80 ms, Nested chain 58.71 ms, Nested fanout 37.03 ms (totals) |
| 2026-01-31 | `std::string_view` lookups in Leaf | Kept (small win across all scenarios) |
| 2026-01-31 | Global `Node` allocation free list | Kept (about 6% on wide tree) |
| 2026-01-31 | Thread-local `Node` free list | Rejected (mixed) |
| 2026-01-31 | Skip retarget path building | Rejected (regressed nested totals) |
| 2026-01-31 | Nested iterator construction micro-opt | Rejected (mixed) |
| 2026-01-31 | `parallel_node_hash_map` submaps 12 -> 4 | Reverted (ASan abort in `test_PathSpace_execution`) |
| 2026-11-13 | `parallel_node_hash_map` submaps 12 -> 4, with the `getOrCreateChild` shard-lock fix | Kept (instructions 20.4G -> 0.9G, RSS 453 MB -> 6.7 MB on the 400-insert run) |
| 2026-11-13 | `std::pmr` children map and `SlidingBuffer` (default resource) | Kept (no measurable cost once nodes are small; see below) |
| 2026-11-13 | Custom memory resources (pool, monotonic arena) vs default | No significant difference in the hierarchy benchmark; arena frees nothing before release |
| 2026-01-31 | Snapshot array-backed trie (bench-only) | Not adopted: reads ~100x faster, but build 3-5x slower, so total is worse |

Raw notes for each experiment follow. Entries marked "debug build" or "single run" predate the recipe above.

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

### 2026-11-13 — Node size, `std::pmr` containers and custom memory resources
- Setup: Apple Silicon, shared and heavily loaded machine (wall-clock numbers swung 10x between identical runs, swap in use). Wall time is therefore not trusted; instructions retired, cycles and peak RSS come from `/usr/bin/time -l` and agree between repeats. Every comparison below ran back to back in one session; master and the new build were each measured twice.
- Build: Release (`-DPATHSPACE_WERROR=ON`), `PathSpaceBench --runs 15 --warmup 2 --max-inserts 400` for master vs new; `--runs 10 --warmup 1` (default 10,000 inserts) for new only (master needs several GB at that size).
- **Node size.** `Node::DefaultSubmaps` is the log2 of phmap's shard count: 12 embedded 4096 submaps, `sizeof(Node)` = 458,752 bytes. With the `std::pmr` children map each submap also stores a `polymorphic_allocator` (+8 bytes per submap), 491,520 bytes per node. In the node-heavy benchmark that cost about +7% RSS and +10-20% cycles (best of 6 interleaved runs: 1.10G baseline, 1.04G `SlidingBuffer`-only pmr, 1.25G pmr children) with 3-4% fewer instructions, so the pmr children map was not acceptable at 12 submaps. Constructing and destroying 300 nodes in isolation was equal (about 10 ms either way), so the cost came from footprint, not CPU work.
- **Fix.** `DefaultSubmaps = 4` (16 shards, about 2 KB per node). The 2026-01-31 attempt aborted under ASan because `getOrCreateChild` read the iterator returned by `try_emplace` after releasing the shard lock; a concurrent insert into the same shard reallocates its slot table. With 4096 shards this was practically unreachable, with 16 it crashed roughly 1 in 50 runs of `pathspace.multithreading`. `getOrCreateChild` now takes the child pointer under the lock (`lazy_emplace_l`), covered by `core.node.concurrency`. 150 soak runs after the fix were clean.
- **Master vs new (default resource), 400 inserts:**

| | master | new |
|---|---|---|
| instructions retired | 20.44G / 20.45G | 0.91G / 0.91G |
| cycles | 6.98G / 7.16G | 0.30G / 0.27G |
| peak RSS | 453.7 MB / 451.4 MB | 6.7 MB / 6.8 MB |
| scenario p50 total (Wide, Deep, Nested chain, Nested fanout) | 9.3, 64.4, 30.3, 22.5 ms (second run 7.5, 22.9, 27.9, 17.2) | 1.0, 1.8, 0.9, 1.6 ms (second run 1.1, 1.0, 0.9, 0.7) |

- **Resources, new build, 400 inserts** (instructions / cycles / RSS): default 0.91G / 0.30G / 6.7 MB, pool 0.89G / 0.30G / 6.9 MB, monotonic 0.88G / 0.30G / 6.7 MB, counting 0.92G / 0.30G / 6.7 MB. Allocation counts through the counting resource: Wide tree 883 (0.03 MiB peak), Deep chain 1,865 (0.06 MiB), Nested chain 1,696 (0.05 MiB), Nested fanout 1,273 (0.04 MiB); the monotonic arena took one 2 MiB block from its upstream.
- **Resources, new build, default 10,000 inserts** (instructions / cycles / RSS): default 7.24G / 2.69G / 99 MB (repeat 7.24G / 2.57G / 98.8 MB), pool 7.07G / 2.58G / 101 MB, monotonic 7.02G / 2.64G / 98.6 MB, counting 7.27G / 2.62G / 99.3 MB. Deep chain makes 46,662 allocations (1.42 MiB peak) and Wide tree 10,387 (0.3 MiB). Instruction and cycle counts differ by under 3% across resources, inside the repeat-to-repeat spread, so no resource is recommended on speed alone.
- What a custom resource covers is limited to tree entries and payload buffers (see `docs/AI_Allocators.md`); POD payload lanes, `NodeData` queues and task state still use the global allocator.
- Reproduce: `./scripts/bench_allocators.sh build-release --runs 10 --warmup 1 --max-inserts 400`, and compare against a master build with `/usr/bin/time -l` for instructions and RSS.

