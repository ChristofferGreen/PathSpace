#pragma once

#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/PathSurfaceMetal.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace SP::UI {

enum class PathWindowPresentMode {
    AlwaysFresh,
    PreferLatestCompleteWithBudget,
    AlwaysLatestComplete,
};

struct PathWindowPresentPolicy {
    PathWindowPresentMode mode = PathWindowPresentMode::PreferLatestCompleteWithBudget;
    std::chrono::milliseconds staleness_budget{8};
    std::uint32_t max_age_frames = 1;
    std::chrono::milliseconds frame_timeout{20};
    bool vsync_align = true;
    bool auto_render_on_present = true;
    bool capture_framebuffer = false;
    double staleness_budget_ms_value = 8.0;
    double frame_timeout_ms_value = 20.0;
};

struct PathWindowPresentRequest {
    std::chrono::steady_clock::time_point now{};
    std::chrono::steady_clock::time_point vsync_deadline{};
    bool vsync_align = true;
    std::span<std::uint8_t> framebuffer{};
    std::span<std::size_t const> dirty_tiles{};
    int surface_width_px = 0;
    int surface_height_px = 0;
    bool has_metal_texture = false;
#if PATHSPACE_UI_METAL
    PathSurfaceMetal* metal_surface = nullptr;
#endif
    PathSurfaceMetal::TextureInfo metal_texture{};
#if defined(__APPLE__)
    bool allow_iosurface_sharing = false;
#endif
};

struct PathWindowPresentStats {
    bool presented = false;
    bool skipped = false;
    bool buffered_frame_consumed = false;
    bool used_progressive = false;
    bool used_metal_texture = false;
    bool vsync_aligned = true;
    bool auto_render_on_present = true;
    bool stale = false;
    PathWindowPresentMode mode = PathWindowPresentMode::PreferLatestCompleteWithBudget;
    PathSurfaceSoftware::FrameInfo frame{};
    double wait_budget_ms = 0.0;
    double damage_ms = 0.0;
    double encode_ms = 0.0;
    double progressive_copy_ms = 0.0;
    double publish_ms = 0.0;
    double present_ms = 0.0;
    double gpu_encode_ms = 0.0;
    double gpu_present_ms = 0.0;
    double frame_age_ms = 0.0;
    std::uint64_t frame_age_frames = 0;
    std::uint64_t drawable_count = 0;
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
    std::uint32_t tile_width_px = 0;
    std::uint32_t tile_height_px = 0;
    bool tiled_renderer_used = false;
    double encode_worker_stall_ms_total = 0.0;
    double encode_worker_stall_ms_max = 0.0;
    std::uint64_t encode_worker_stall_workers = 0;
    std::uint64_t progressive_tiles_dirty = 0;
    std::uint64_t progressive_tiles_total = 0;
    std::uint64_t progressive_tiles_skipped = 0;
    bool progressive_tile_diagnostics_enabled = false;
    std::size_t progressive_tiles_copied = 0;
    std::size_t progressive_rects_coalesced = 0;
    std::size_t progressive_skip_seq_odd = 0;
    std::size_t progressive_recopy_after_seq_change = 0;
    std::string error;
    std::string backend_kind;
#if defined(__APPLE__)
    bool used_iosurface = false;
    std::optional<PathSurfaceSoftware::SharedIOSurface> iosurface;
#endif
};

class PathWindowView {
public:
    using PresentMode = PathWindowPresentMode;
    using PresentPolicy = PathWindowPresentPolicy;
    using PresentRequest = PathWindowPresentRequest;
    using PresentStats = PathWindowPresentStats;

#if defined(__APPLE__)
    struct MetalPresenterConfig {
        void* layer = nullptr;
        void* device = nullptr;
        void* command_queue = nullptr;
        double contents_scale = 1.0;
    };

    static void ConfigureMetalPresenter(MetalPresenterConfig const& config);
    static void ResetMetalPresenter();
#endif

    [[nodiscard]] auto present(PathSurfaceSoftware& surface,
                               PresentPolicy const& policy,
                               PresentRequest const& request) -> PresentStats;

    static bool SupportsIOSurfaceSharing();

#if defined(__APPLE__) && PATHSPACE_UI_METAL
    [[nodiscard]] auto present(PathSurfaceMetal& surface,
                               PresentPolicy const& policy,
                               PresentRequest const& request) -> PresentStats;
#endif
};

inline bool PathWindowView::SupportsIOSurfaceSharing() {
#if defined(__APPLE__)
    static std::optional<bool> cached;
    if (cached.has_value()) {
        return *cached;
    }
    Runtime::SurfaceDesc desc{};
    desc.size_px.width = 2;
    desc.size_px.height = 2;
    desc.pixel_format = Runtime::PixelFormat::RGBA8Unorm_sRGB;
    desc.color_space = Runtime::ColorSpace::sRGB;
    desc.premultiplied_alpha = true;

    PathSurfaceSoftware surface{desc};
    auto stage = surface.staging_span();
    if (stage.empty()) {
        cached = false;
        return *cached;
    }
    stage[0] = 0xFF;
    surface.publish_buffered_frame({
        .frame_index = 1,
        .revision = 1,
        .render_ms = 0.1,
    });
    cached = surface.front_iosurface().has_value();
    return *cached;
#else
    return false;
#endif
}

} // namespace SP::UI
