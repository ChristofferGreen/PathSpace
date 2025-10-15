#include <pathspace/ui/PathWindowView.hpp>

#include <algorithm>
#include <cstring>
#include <exception>

namespace SP::UI {

namespace {

constexpr std::size_t kBytesPerPixel = 4u;

} // namespace

auto PathWindowView::present(PathSurfaceSoftware& surface,
                             PresentPolicy const& policy,
                             PresentRequest const& request) -> PresentStats {
    PresentStats stats{};

    auto wait_budget = request.vsync_deadline - request.now;
    if (wait_budget < std::chrono::steady_clock::duration::zero()) {
        wait_budget = std::chrono::steady_clock::duration::zero();
    }
    stats.wait_budget_ms =
        std::chrono::duration<double, std::milli>(wait_budget).count();

    auto const required_bytes = surface.frame_bytes();
    if (required_bytes > 0 && request.framebuffer.size() < required_bytes) {
        stats.skipped = true;
        stats.error = "framebuffer span too small for surface dimensions";
        return stats;
    }

    if (surface.has_buffered()) {
        auto copy = surface.copy_buffered_frame(request.framebuffer);
        if (copy) {
            stats.presented = true;
            stats.buffered_frame_consumed = true;
            stats.frame = copy->info;
            return stats;
        }
    }

    if (policy.mode == PresentMode::AlwaysFresh) {
        stats.skipped = true;
        return stats;
    }

    if (!surface.has_progressive() || request.dirty_tiles.empty()) {
        stats.skipped = true;
        return stats;
    }

    auto& progressive = surface.progressive_buffer();
    auto const row_stride = surface.row_stride_bytes();
    std::vector<std::uint8_t> tile_storage;

    for (auto tile_index : request.dirty_tiles) {
        try {
            auto dims = progressive.tile_dimensions(tile_index);
            auto const tile_bytes = static_cast<std::size_t>(dims.width) * dims.height * kBytesPerPixel;
            if (tile_bytes == 0) {
                continue;
            }
            tile_storage.resize(tile_bytes);
            auto tile_copy = progressive.copy_tile(tile_index, tile_storage);
            if (!tile_copy) {
                continue;
            }
            stats.used_progressive = true;
            ++stats.progressive_tiles_copied;
            stats.frame.revision = std::max(stats.frame.revision, tile_copy->epoch);

            for (int row = 0; row < dims.height; ++row) {
                auto const* src = tile_storage.data()
                                  + static_cast<std::size_t>(row) * static_cast<std::size_t>(dims.width) * kBytesPerPixel;
                auto* dst = request.framebuffer.data()
                            + (static_cast<std::size_t>(dims.y + row) * row_stride)
                            + static_cast<std::size_t>(dims.x) * kBytesPerPixel;
                std::memcpy(dst, src, static_cast<std::size_t>(dims.width) * kBytesPerPixel);
            }
        } catch (std::exception const& ex) {
            stats.error = ex.what();
            continue;
        } catch (...) {
            stats.error = "unexpected exception during progressive copy";
            continue;
        }
    }

    if (stats.progressive_tiles_copied > 0) {
        stats.presented = true;
    } else {
        stats.skipped = true;
    }
    return stats;
}

} // namespace SP::UI
