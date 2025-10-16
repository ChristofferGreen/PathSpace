# Surface Ray Cache Plan (Draft)

> **Context update (October 15, 2025):** Ray cache milestones are evaluated within the “Atlas” AI context launched for this cycle; align earlier terminology with Atlas standards.

> **Status:** Working draft. Describes the ray-query + sample cache approach that replaces the earlier “microtriangle” tessellation idea. We will defer implementation until the baseline “set pixel color” renderer path is stable.

## Why This Direction

- Tessellating every drawable into microtriangles is expensive, adds serialization overhead, and duplicates geometric data we already have (SDFs, explicit meshes, tetrahedral surfaces).
- Ray intersections against the authoritative surface representation can directly produce the shading samples we need per pixel. A spatial/temporal cache lets us reuse those samples across frames until content changes.
- When we run out of frame budget, we can gracefully fall back to the nearest cached sample—even if it’s far away—so the image progressively converges rather than stalling or popping.

## Objectives

1. Provide a deterministic ray-query interface over the scene’s surface representation (SDF, mesh, tet surfaces).
2. Maintain a progressive pixel cache keyed by screen-space (and optionally history reprojection) to reuse samples when geometry/material epochs are unchanged.
3. Allow early exits when budgets are hit: pull the closest cached sample (even if distant) so each frame can complete within time constraints, improving quality over time.
4. Keep diagnostics hooked to `RenderSettings::microtri_rt` (renamed? TBD) so pacing knobs (budget, rays per pixel, seeds) still apply.

## High-Level Pipeline

### 1. Baseline Renderer (Phase Zero)
- Finish the simple “set pixel color” path that paints pixels directly from the drawable SoA (no ray queries). This is the correctness harness for input wiring, present policy, and diagnostics.
- Once stable: introduce the ray cache as an opt-in path guarded behind renderer flags.

### Renderable Acceleration Input Contract

Every renderable mounted under the renderer target must supply geometry for the ray pipeline via a lightweight API. Suggested C++ shape (names illustrative):

```cpp
struct AccelInput {
  enum class GeometryType { TriangleMesh, TetraMesh };

  GeometryType type;
  uint32_t     surface_handle;    // stable id for cache tagging
  EpochStamp   geometry_epoch;
  EpochStamp   material_epoch;
  Bound3f      bounds;            // world-space AABB

  struct TriangleMesh {
    std::shared_ptr<const std::vector<float3>> positions;
    std::shared_ptr<const std::vector<float3>> normals;      // optional
    std::shared_ptr<const std::vector<uint32_t>> indices;    // triangles (3 per)
    std::shared_ptr<const std::vector<uint32_t>> materialIds;// optional per-face
  } tri;

  struct TetraMesh {
    std::shared_ptr<const std::vector<float3>> positions;
    std::shared_ptr<const std::vector<uint32_t>> indices;    // 4 per tetra
    std::shared_ptr<const std::vector<uint32_t>> materialIds;
  } tet;
};

struct Renderable {
  Expected<AccelInput> get_acceleration_input(RendererContext& ctx);
};
```

Implementation notes:
- Renderables may internally author via SDF/procedural means but must convert to triangles or tetrahedra before returning.
- Geometry pointers are reference-counted so renderer caches can reuse existing buffers across frames.
- `geometry_epoch` / `material_epoch` bump whenever underlying data changes; the renderer uses them to rebuild acceleration structures and invalidate cached pixels.
- The renderer builds two acceleration paths: triangles feed the GPU TLAS/BLAS, tetra meshes feed the CPU tet-walk. Renderables can populate whichever structures they support.
- If geometry isn’t ready (e.g., asset streaming), return an error; renderer skips it that frame and logs the event.

Renderer responsibilities:
1. Call `get_acceleration_input` at frame start (or whenever epochs change).
2. Compare epochs to cached entries; rebuild CPU/GPU acceleration structures as needed.
3. Tag cached samples with `surface_handle` for later invalidations.
4. Keep per-pixel loops entirely in renderer-managed memory—PathSpace is touched only during the fetch phase.

### 2. Surface Intersection Layer
- Expose a scene-level API: `IntersectRay(ray, options)` → `{hit?, position_world, normal_world, material_handle, epoch_stamp}`.
- Implementors may reuse existing SDF evaluators or tetrahedral acceleration structures.
- Ensure returned `epoch_stamp` lets the cache detect when a sample is stale because geometry/material changed.
- **Canonical representation (lazy):** normalize all renderable surfaces to **tetrahedral volumes** on first request. The first time a widget (e.g., a button) is asked to render, it is prompted for a tetra mesh representing its current SDF/geometry. The mesh is cached alongside the surface. When the widget animates (e.g., click ripple), it can supply updated meshes each frame for the animation window and optionally keep a small cache of the representations it cycles through. This keeps the intersection kernel uniform without forcing up-front conversion for unused surfaces.
- **Initial sources of tetrahedra:**
  - UI widgets: start with a small hand-authored library of tetra meshes (rect panels, rounded pills, spheres, cylinders).
  - SDF primitives: implement SDF→tetra conversion by surveying existing literature (isosurface meshing / dual contouring variants) and picking a conservative scheme for the first set of primitives (boxes, spheres, capsules, etc.).
  - Future work (e.g., planetary surfaces): seed with a single large tetrahedron and progressively subdivide based on camera location. That refinement system sits on top of the same tetrahedral representation but will be developed later once the core ray cache is in place.

### 3. Pixel Sample Cache

Per pixel (or per screen-space tile) store:
- `color` / `lighting` result
- `position_world`, `normal_world`
- `material_handle`
- `timestamp` or `epoch_stamp`
- Optional confidence/variance measures

Lookup strategy:
- Try the exact pixel key first.
- If missing or stale, search nearby pixels (screen-space tile lists) for a valid sample within tolerance; fall back to world-space proximity queries when needed.
- Fallback: when frame budget is exhausted, return the nearest cached sample even if it’s far; mark the pixel as “needs refinement”.

### Shared Lookup Data Structures

To keep CPU and hardware RT paths aligned, store cache metadata in GPU-friendly buffers that the CPU also maps:
- **Sample atlas:** contiguous array of `SampleEntry { float2 pixel_coord; float3 world_pos; float3 normal; float3 color; uint32 surface_handle; uint32 epochs; }` kept in a Struct-of-Arrays layout. CPU code views it as vectors; GPU shaders access via SSBO/StorageBuffer.
- **Screen-space tile grid:** 2D grid (e.g., 16×16 pixel tiles). Each tile stores `(start_offset, count)` into the sample atlas via a second buffer. CPU uses it for quick neighborhood search; GPU compute shaders do the same for bounce jobs or reprojection.
- **World-space hash (optional):** for large parallax, maintain a hashed voxel table keyed by quantized world position to locate nearest cached sample. Implemented with the same flat buffers so GPU kernels can reuse it.

Updates are double-buffered: while the renderer populates buffer A, GPU bounce jobs consume buffer B; swap at frame end. This keeps data structures identical across CPU and GPU implementations and avoids bespoke caches for each path.

### Cache Invalidation

- Every surface contributor (SDF node, mesh, tet surface, material layer) must expose an epoch counter or change token.
- When a surface updates geometry or material parameters, it publishes an invalidation record (e.g., via PathSpace notify) keyed by its surface handle. The cache subscribes and invalidates all entries tagged with that handle.
- If a surface’s deformation affects only a region, optional bounding volumes can narrow the invalidation scope; otherwise we invalidate conservatively.
- Materials with large parameter deltas can invalidate their samples, while minor tweaks (e.g., temporal noise) may update the epoch without full eviction if the renderer chooses.
- **Transport proposal:** surfaces write `SurfaceInvalidation` structs to `renderers/<rid>/cache/invalidate` (a PathSpace queue). Each record carries `{ surface_handle, geometry_epoch, material_epoch, optional_bounds, reason }`. The cache daemon drains this queue, bumps its internal epoch table, and tags affected pixels for eviction. Surfaces also expose a `surface_handle` value under their scene node so the cache can tag new samples with the correct handle.
- **Partial updates:** when `optional_bounds` is present, the cache only evicts samples whose cached `position_world` lies inside the bounding volume; otherwise, the entire surface is invalidated. Surfaces without precise bounds can omit the field and accept coarser invalidation.

### 4. Progressive Refinement Loop

Per frame:
1. Determine budget (max rays) from renderer settings.
2. Visit pixels in a progressive sequence to produce natural updates:
   - Start with a deterministic blue-noise or Morton-order pattern so every frame touches a sparse stipple across the screen.
   - On subsequent frames, rotate/offset the pattern so coverage fills in uniformly.
   - Within each tile, optionally prioritize high-error / high-variance samples first.
3. For each selected pixel:
   - If cached sample is valid and meets quality bar → reuse (no ray).
   - Else if budget remains → shoot ray, shade, cache result.
   - Else → reuse nearest cached sample (even far away) so the frame still completes.
4. Write pixel to framebuffer. Mark stats for diagnostics (e.g., reused vs. resampled counts).

Over time, frames converge as budgets allow more rays; stale cache entries are evicted whenever source epochs change. The blue-noise stipple ordering keeps visual noise pleasant and ensures the image refines evenly instead of sweeping from one corner.

### Hybrid CPU/GPU Strategy

- **Ray generation (primary hits):** run in software on the CPU to minimize latency and keep per-pixel cache logic close to the presenter. CPU-side ray marching traverses the tetrahedral TLAS/BLAS directly, producing the initial surface sample for each pixel.
- **Lighting bounces:** once a primary hit is cached, offload secondary rays / global illumination to hardware RT when available (Metal/Vulkan RT). The cached sample stores the request; a GPU worker integrates lighting asynchronously and updates the cache entry. Systems without hardware RT fall back to the CPU path.
- Budgets differentiate between CPU primary rays (`budget.primary_rays_per_frame`) and GPU bounce workloads (`budget.rt_bounces_per_frame`), letting us keep UI interaction responsive while progressively refining lighting.

### CPU↔GPU Coordination

1. **Startup detection:** at application init, probe Vulkan/Metal capabilities and deployment policy once; store `gpu_rt_available` / `gpu_rt_allowed` flags in the renderer context. No per-frame probing is required.
2. **Frame start:** renderer latches `RenderSettings::ray_cache`, clears per-frame queues, and reads the cached flags. If hardware RT is both available and allowed, mark the frame GPU-enabled; otherwise stay CPU-only.
3. **CPU pass:** ray cache walks tiles, issues primary rays, writes/updates `SampleEntry` records in buffer A. For samples needing bounce lighting, it pushes lightweight requests `{sample_index, surface_handle, material_id}` into an RT dispatch queue (SSBO).
4. **GPU dispatch (if RT permitted):** after the CPU pass completes (or mid-frame via async compute), GPU consumes the queue, traces secondary rays, writes lighting contributions into buffer B.
5. **Merge:** CPU checks completion flags on buffer B (or uses fences). Completed entries merge back into buffer A before present; in-progress entries keep prior lighting so the main thread never stalls.
6. **Fallback / policy:** when hardware RT isn’t supported/allowed, the CPU drains the queue itself, following identical control flow.
7. **Timeout handling:** if the GPU misses the frame budget, the presenter uses existing lighting values; pending requests roll over to the next frame.

## Post Processing / Tone Mapping

- After the ray cache (and any overlay pixels) produce the HDR buffer, run a configurable tone-mapping pipeline (default ACES/filmic) before handing pixels to the presenter.
- Keep the tone-mapper pluggable so UI renderers can choose simpler gamma curves if desired.
- The overlay buffer from the direct pixel path is treated as display-space and composited *after* tone mapping so overlays remain untouched.

## Direct Pixel Overlay Path

Some renderables/tools may bypass the ray cache entirely and write pixels directly. Provide a dedicated PathSpace overlay queue per renderer:
- `renderers/<rid>/overlay/pixels/in` — queue of `{ x:int, y:int, rgba:float4 }` updates. On each frame the presenter drains this queue, writes into an overlay buffer, then blits it over the main framebuffer before present.
- `renderers/<rid>/overlay/pixels/delete` — queue of `{ x:int, y:int }` removals (or bounding boxes). Presenter clears those entries from the overlay buffer.

Overlay buffer notes:
- Fixed size matching the target surface (cleared each frame unless persisted by producer).
- Producers are responsible for resubmitting persistent pixels every frame; otherwise they drift out.
- Cap queue length / per-frame consumption to protect responsiveness.

### Shader Authoring Roadmap

1. **Phase 0 – Direct C API.** Implement shaders as simple C functions compiled into the renderer (`ShadeSurface(hit, params)`). This is sufficient for the initial software-only path.
2. **Phase 1 – Portable Shader Language (PSL).** Introduce a GLSL-like DSL. Parse once and emit both GLSL (compiled to SPIR-V for GPU) and C++/LLVM IR (for CPU). Share reflection metadata so bindings stay in sync.
3. **Phase 2 – Runtime authoring.** Allow hot-loading PSL shaders at runtime, compiling them to both targets so user-authored materials work in software-only deployments as well.

## Shading IR Specification (Draft)

Goal: define one portable representation so both CPU and GPU evaluate identical BRDF/BTDF logic. Materials compile into this IR once; the CPU interpreter and GPU codegen share bytecode.

### Core Concepts
- **Module:** immutable blob containing:
  - metadata (`version`, `material_name`, `hash`)
  - tables of constants, textures/samplers, string ids
  - entry functions (`surface`, `volume`, optional `emissive`)
- **Function:** sequence of SSA instructions forming a DAG; terminated by `Return color3` (surface) or `Return volume_sample`.
- **Value types:** `float`, `float2/3/4`, `int`, `bool`, `material_param` (indexed uniform), `texture2D`, `textureCube`.
- **Instruction set (initial):**
  - arithmetic: `add`, `sub`, `mul`, `mad`, `div`, `sqrt`, `rsqrt`
  - vector ops: `dot`, `cross`, `normalize`, `reflect`, `refract`
  - BRDF helpers: `ggx_D`, `ggx_G`, `ggx_F`, Disney diffuse
  - utility: `clamp`, `saturate`, `mix`, `step`
  - texture/sample: `sample_texture2D(coord, sampler)` (explicit LOD optional)
  - control: `select(pred, a, b)` (no general branching in v1)
- **Uniform parameters:** stored in a packed UBO structure; IR references them via indices so both CPU and GPU share layout.

### Execution Modes
- **CPU interpreter:** walks SSA instructions; implemented as a simple register VM. Optional LLVM JIT later.
- **GPU codegen:** translate SSA to SPIR-V / MSL / HLSL. Limited control flow keeps generated shaders branch-friendly.

### Material Authoring Flow
1. Author writes material in high-level form (node graph / JSON) specifying baseColor, metallic, roughness, transmission, normal maps, emissive.
2. Offline compiler lowers to IR module (normalizes params, allocates textures/samplers, bakes constants).
3. Runtime loads the module, uploads textures/uniforms, and attaches a `MaterialHandle` referencing the module + uniform block.
4. CPU shading: interpreter executes `surface(hitContext, uniforms)` to produce BRDF/BTDF outputs.
5. GPU shading: RT bounce kernels execute generated shader with the same context/uniforms.

### Extension Hooks
- Register custom instruction libraries (clear coat, sheen) as long as both CPU/GPU backends implement them.
- Module `version` ensures backward compatibility; renderer rejects newer versions if unsupported.

### Validation
- Unit tests execute every instruction via CPU interpreter and GPU shader, comparing outputs within epsilon.
- Golden scene (wood + glass button) rendered via CPU-only and GPU-accelerated paths to confirm parity.

## Data & Diagnostics

### PathSpace output schema
Record per-frame stats under the renderer target so tooling can query them consistently:
- `output/v1/common/ray_cache/budget` — `{ primary_rays_used:uint32, refinement_pixels_processed:uint32, rt_bounces_used:uint32 }`
- `output/v1/common/ray_cache/cache` — `{ pixels_reused:uint32, pixels_resampled:uint32, search_radius_px:float }`
- `output/v1/common/ray_cache/queue` — `{ pending:int32, gpu_enabled:bool, last_dispatch_ms:float }`
- `output/v1/common/ray_cache/status` — `{ fallback_reason:string, last_error:string }`

### Optional debug attachments
- `output/v1/debug/ray_cache/distance` — screen-space texture storing normalized distance to nearest cached sample.
- `output/v1/debug/ray_cache/variance` — variance/quality map to visualize progressive refinement.

### Logging
- Emit structured lines to `<app>/io/log/info` with tag `ray_cache` whenever mode switches (GPU RT enable/disable, cache reset). Include budget usage and any fallback reason for quick triage.

## Settings Mapping

Rename `RenderSettings::microtri_rt` to `RenderSettings::ray_cache` (short alias `RC`). The renderer owns these knobs: builders/tests seed defaults, but runtime code supplies budgets. Proposed layout:

```cpp
struct RayCacheSettings {
  struct Budget {
    uint32_t primary_rays_per_frame = 0;      // CPU ray generation budget
    uint32_t refinement_pixels_per_frame = 0; // pixels eligible for fresh shading
    uint32_t rt_bounces_per_frame = 0;        // GPU lighting jobs allowed per frame
  } budget;

  struct Path {
    uint32_t max_bounces = 1;
    uint32_t rr_start_bounce = 1;
  } path;

  struct Cache {
    float search_radius_px = 1.5f;        // screen-space radius when reusing cached samples
    bool  invalidate_on_epoch_change = true;
  } cache;

  uint64_t seed = 0;                       // deterministic sample ordering
};
```

Flow:
- Renderer latches `RenderSettings::ray_cache` once per frame (same rule as other settings).
- UI helpers expose convenience setters (`Surface::ConfigureRayCache`, etc.) but do not mutate at render time.
- Snapshot builder ignores these values—they are runtime-only knobs.
- Diagnostics mirror the effective settings under `output/v1/common/ray_cache/*` for visibility.

## Open Questions

- **Cache search metric:** pure screen-space distance vs. world-space proximity (or hybrid).
- **Variance tracking:** do we need per-sample variance to decide priority?
- **History reprojection:** incorporate velocity buffers to reproject previous frame samples?
- **Memory budget:** how large can the cache grow; eviction policy?
- **Diagnostics UI:** best way to visualize how many pixels were re-shaded vs. reused.

---

*Next steps:* finalize the ray intersection API, rename the renderer settings to reflect this approach, and prototype the cache loop on top of the simple pixel renderer. Update this document as decisions solidify. Logged 2025‑10‑15.

## Future Work: PrimeScript

Longer-term we may adopt **PrimeScript**, a unified language that can emit C++, GLSL, and VM bytecode for both gameplay scripting and shading. See `docs/PrimeScriptPlan.md` for early thoughts. This sits beyond the current roadmap (C API → PSL) but remains a target for consolidating authoring workflows.
