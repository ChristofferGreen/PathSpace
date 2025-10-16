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
    auto const start_time = request.now;
    stats.mode = policy.mode;
    stats.auto_render_on_present = policy.auto_render_on_present;
    stats.vsync_aligned = policy.vsync_align;
    stats.frame = surface.latest_frame_info();

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
        auto finish = std::chrono::steady_clock::now();
        stats.present_ms = std::chrono::duration<double, std::milli>(finish - start_time).count();
        return stats;
    }

    auto const row_stride = surface.row_stride_bytes();

    auto copy_progressive_tiles = [&](bool mark_present) -> bool {
        if (!surface.has_progressive() || request.dirty_tiles.empty()) {
            return false;
        }
        auto& progressive = surface.progressive_buffer();
        std::vector<std::uint8_t> tile_storage;
        std::size_t copied = 0;

        for (auto tile_index : request.dirty_tiles) {
            try {
                auto dims = progressive.tile_dimensions(tile_index);
                if (dims.width <= 0 || dims.height <= 0) {
                    continue;
                }
                ++stats.progressive_rects_coalesced;
                auto const tile_bytes = static_cast<std::size_t>(dims.width)
                                        * static_cast<std::size_t>(dims.height) * kBytesPerPixel;
                tile_storage.resize(tile_bytes);
                auto attempt_copy = [&](bool /*second_attempt*/) {
                    return progressive.copy_tile(tile_index, tile_storage);
                };
                auto tile_copy = attempt_copy(false);
                if (!tile_copy) {
                    ++stats.progressive_skip_seq_odd;
                    tile_copy = attempt_copy(true);
                    if (tile_copy) {
                        ++stats.progressive_recopy_after_seq_change;
                    }
                }
                if (!tile_copy) {
                    continue;
                }
                auto const row_pitch = static_cast<std::size_t>(dims.width) * kBytesPerPixel;
                for (int row = 0; row < dims.height; ++row) {
                    auto const* src = tile_storage.data() + static_cast<std::size_t>(row) * row_pitch;
                    auto* dst = request.framebuffer.data()
                                + (static_cast<std::size_t>(dims.y + row) * row_stride)
                                + static_cast<std::size_t>(dims.x) * kBytesPerPixel;
                    std::memcpy(dst, src, row_pitch);
                }
                stats.used_progressive = true;
                stats.frame.revision = std::max(stats.frame.revision, tile_copy->epoch);
                ++copied;
            } catch (std::exception const& ex) {
                stats.error = ex.what();
            } catch (...) {
                stats.error = "unexpected exception during progressive copy";
            }
        }

        if (copied > 0) {
            stats.progressive_tiles_copied += copied;
            if (mark_present) {
                stats.presented = true;
                stats.skipped = false;
            }
            return true;
        }
        return false;
    };

    if (surface.has_buffered()) {
        auto copy = surface.copy_buffered_frame(request.framebuffer);
        if (copy) {
            stats.presented = true;
            stats.buffered_frame_consumed = true;
            stats.frame = copy->info;
            (void)copy_progressive_tiles(false);
            auto finish = std::chrono::steady_clock::now();
            stats.present_ms = std::chrono::duration<double, std::milli>(finish - start_time).count();
            return stats;
        }
    }

    if (policy.mode == PresentMode::AlwaysFresh) {
        stats.skipped = true;
        auto finish = std::chrono::steady_clock::now();
        stats.present_ms = std::chrono::duration<double, std::milli>(finish - start_time).count();
        return stats;
    }

    auto copied = copy_progressive_tiles(true);
    if (!copied) {
        stats.skipped = true;
    }
    auto finish = std::chrono::steady_clock::now();
    stats.present_ms = std::chrono::duration<double, std::milli>(finish - start_time).count();
    return stats;
}

} // namespace SP::UI
