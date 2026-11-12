# PathSpace — Allocator Guide

How to give a `PathSpace` its own `std::pmr::memory_resource`, what that covers, and the rules the resource has to follow. Architecture summary: "Allocator integration" in `docs/AI_Architecture.md`. Measurements: `docs/Optimization.md`.

## Quick start
```cpp
#include <pathspace/PathSpace.hpp>
#include <pathspace/core/LockedMemoryResource.hpp>
#include <memory_resource>

// Declare the resource before the space: it is destroyed after it.
std::pmr::monotonic_buffer_resource arena{1 << 20};   // grows from new/delete when the block is used up
SP::LockedMemoryResource            shared{&arena};   // PathSpace allocates from concurrent operations
SP::PathSpace                       space{shared};

space.insert("/sensors/a", 42);
auto value = space.read<int>("/sensors/a");
```
`space.memoryResource()` returns the resource the space allocates from. A space built the usual way (`PathSpace space;`) allocates from `std::pmr::get_default_resource()`, which is `new`/`delete` unless the process changed it.

The same resource can be attached to a shared context for spaces that share one: `context->setMemoryResource(&res)` (null is rejected with `InvalidError`), then `PathSpace{context, prefix}`. Spaces read the context's resource when they are constructed.

## What allocates from the resource
- The children map of every node (map entries and path component names).
- Child nodes created below the root get the same resource as their parent.
- The serialized payload buffer (`SlidingBuffer`) of every node.
- Copies: `PathSpace(PathSpace const&)`, `clone()` and the nested spaces a copy creates use the source's resource. Assigning (`a = b`) keeps `a`'s own resource.

Not covered (global allocator): the `Node` objects themselves (about 2 KB each), the POD fast-path lanes used for trivially copyable values such as `int`, the per-node type/task/future queues, task control blocks, the wait registry and temporary buffers used while serializing. A custom resource therefore bounds the dominant tree and payload storage, not every byte the process allocates.

## Rules for the resource
1. **Lifetime.** The caller owns the resource. It must outlive the space and everything allocated from it, including copies and clones. Declare it before the space (or keep it in a longer-lived scope). The context stores a raw pointer only.
2. **Thread safety.** Insert/read/take run concurrently and allocate from several threads at once. The resource must be thread-safe. `std::pmr::monotonic_buffer_resource` and `std::pmr::unsynchronized_pool_resource` are not: wrap them in `SP::LockedMemoryResource` (serializes with one mutex) or use `std::pmr::synchronized_pool_resource`. Skip the wrapper only for a space that one thread uses from construction to destruction.
3. **Not null.** The constructor takes a reference and the context rejects null.
4. **Fixed at construction.** A space's tree keeps the resource it was built with. Changing a context's resource later only affects spaces constructed afterwards. A space mounted as a nested space (`insert("/mount", std::make_unique<PathSpace>(res))`) keeps its own resource; give nested spaces the same resource explicitly when you want one arena for the whole tree.
5. **Failure.** If the resource throws (`std::bad_alloc`, an exhausted `null_memory_resource` upstream) the exception propagates out of the operation that allocated. Back arenas with a growing upstream unless the size is known.

## Choosing a resource
| Resource | Fits | Watch out for |
|---|---|---|
| default (`new`/`delete`) | General use | None; one virtual call per allocation |
| `synchronized_pool_resource` | Many small nodes freed and reused | Pool internals take locks; measure under your thread count |
| `monotonic_buffer_resource` + `LockedMemoryResource` | Build a tree, use it, drop it as a unit (per-frame or per-request spaces) | Memory is only returned when the arena is released; `take`, `clear` and erased nodes do not give memory back |

## Verifying allocation behaviour
`tests/unit/CountingMemoryResource.hpp` counts allocations, deallocations and live bytes with atomics. Typical assertion: build a space on it, run the workload, destroy the space, and check `liveAllocations() == 0`. The tests in `tests/unit/pathspace/test_PathSpaceAllocator.cpp` show stress, concurrency and arena-outlives-space patterns.

## Benchmarks
Measured in the hierarchy benchmark, default, pool and monotonic resources are within a few percent of each other in instructions and cycles; pick a resource for lifetime and locality reasons (release everything at once, bound or count memory), not for speed. `PathSpaceBench --resource default|pool|monotonic|counting` runs the hierarchy scenarios on each resource (single-threaded, so the pool and arena are used unsynchronized); `counting` also prints allocation counts and peak bytes. See `docs/Optimization.md` for the recipe and the recorded A/B numbers.
