#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/FontAtlasCache.hpp>
#include <pathspace/ui/ImageCache.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/MaterialDescriptor.hpp>
#include <pathspace/ui/MaterialShaderKey.hpp>
#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/scenegraph/SoftwareTileRenderer.hpp>
#include <pathspace/ui/scenegraph/TileDirtyTracker.hpp>
#include <pathspace/ui/runtime/RenderSettings.hpp>
#include <pathspace/ui/runtime/SurfaceTypes.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <parallel_hashmap/phmap.h>

namespace SP::UI {

using Runtime::SurfaceDesc;
using RenderSettings = Runtime::RenderSettings;
using RendererKind = Runtime::RendererKind;
using DirtyRectHint = Runtime::DirtyRectHint;

class PathSurfaceMetal;

class PathRenderer2D {
public:
    enum class TextPipeline {
        GlyphQuads,
        Shaped,
    };

    struct DrawableBounds {
        int min_x = 0;
        int min_y = 0;
        int max_x = 0;
        int max_y = 0;

        [[nodiscard]] auto empty() const -> bool {
            return min_x >= max_x || min_y >= max_y;
        }
    };

    struct RenderParams {
        SP::ConcretePathStringView target_path;
        RenderSettings const& settings;
        PathSurfaceSoftware& surface;
        RendererKind backend_kind = RendererKind::Software2D;
        PathSurfaceMetal* metal_surface = nullptr;
    };

    struct RenderStats {
        std::uint64_t frame_index = 0;
        std::uint64_t revision = 0;
        double render_ms = 0.0;
        std::size_t drawable_count = 0;
        double damage_ms = 0.0;
        double encode_ms = 0.0;
        double progressive_copy_ms = 0.0;
        double publish_ms = 0.0;
        std::uint64_t progressive_tiles_updated = 0;
        std::uint64_t progressive_bytes_copied = 0;
        std::uint64_t progressive_tile_size = 0;
        std::uint64_t progressive_workers_used = 0;
        std::uint64_t progressive_jobs = 0;
        std::uint64_t encode_workers_used = 0;
        std::uint64_t encode_jobs = 0;
        std::uint64_t tiles_total = 0;
        std::uint64_t tiles_dirty = 0;
        std::uint64_t tiles_rendered = 0;
        std::uint64_t tile_jobs = 0;
        std::uint64_t tile_workers_used = 0;
        std::uint32_t tile_width = 0;
        std::uint32_t tile_height = 0;
        bool tiled_renderer_used = false;
        double encode_worker_stall_ms_total = 0.0;
        double encode_worker_stall_ms_max = 0.0;
        std::uint64_t encode_worker_stall_workers = 0;
        std::uint64_t progressive_tiles_dirty = 0;
        std::uint64_t progressive_tiles_total = 0;
        std::uint64_t progressive_tiles_skipped = 0;
        bool progressive_tile_diagnostics_enabled = false;
        std::uint64_t text_command_count = 0;
        std::uint64_t text_fallback_count = 0;
        TextPipeline text_pipeline = TextPipeline::GlyphQuads;
        bool text_fallback_allowed = true;
        RendererKind backend_kind = RendererKind::Software2D;
        std::uint64_t resource_cpu_bytes = 0;
        std::uint64_t resource_gpu_bytes = 0;
        std::uint64_t texture_gpu_bytes = 0;
        std::vector<DirtyRectHint> damage_tiles;
        std::vector<MaterialDescriptor> materials;
        std::vector<MaterialResourceResidency> resource_residency;
    };

    struct DrawableState {
        DrawableBounds bounds{};
        std::uint64_t fingerprint = 0;
    };

    using DrawableStateMap = phmap::flat_hash_map<std::uint64_t, DrawableState>;

    explicit PathRenderer2D(PathSpace& space);

    auto render(RenderParams params) -> SP::Expected<RenderStats>;

    auto render_bucket(RenderParams params,
                       SP::UI::Scene::DrawableBucketSnapshot const* bucket,
                       std::string const& revision_base,
                       std::uint64_t revision) -> SP::Expected<RenderStats>;

private:
    using MaterialDescriptorMap = phmap::flat_hash_map<std::uint32_t, MaterialDescriptor>;

    struct TargetState {
        SurfaceDesc desc{};
        std::array<float, 4> clear_color{0.0f, 0.0f, 0.0f, 0.0f};
        DrawableStateMap drawable_states;
        std::vector<float> linear_buffer;
        MaterialDescriptorMap material_descriptors;
        std::vector<MaterialDescriptor> material_list;
        SceneGraph::TileDirtyTracker tiled_dirty_tracker;
        std::unique_ptr<SceneGraph::SoftwareTileRenderer> tiled_renderer;
        PathSurfaceSoftware* tiled_surface = nullptr;
        std::uint64_t last_revision = 0;
        double last_approx_area_total = 0.0;
        double last_approx_area_opaque = 0.0;
        double last_approx_area_alpha = 0.0;
    };

    struct TargetCacheEntry {
        std::mutex mutex;
        TargetState state;
    };

    struct TargetCache {
        std::mutex mutex;
        std::unordered_map<std::string, std::shared_ptr<TargetCacheEntry>> entries;
    };

    PathSpace& space_;
    ImageCache image_cache_;
    FontAtlasCache font_atlas_cache_;

    auto render_with_bucket(SP::UI::Scene::DrawableBucketSnapshot const* bucket,
                            RenderParams const& params,
                            std::string const& revision_base,
                            std::uint64_t revision) -> SP::Expected<RenderStats>;

    static auto target_cache() -> TargetCache&;
};

} // namespace SP::UI
