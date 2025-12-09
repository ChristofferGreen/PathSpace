#include <pathspace/ui/PathWindowView.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <IOSurface/IOSurface.h>
#endif

namespace SP::UI {

namespace {

constexpr std::size_t kBytesPerPixel = 4u;

} // namespace

auto PathWindowView::present(PathSurfaceSoftware& surface,
                             PresentPolicy const& policy,
                             PresentRequest const& request) -> PresentStats {
    PresentStats stats{};
    stats.used_metal_texture = false;
    auto const start_time = request.now;
    stats.mode = policy.mode;
    stats.auto_render_on_present = policy.auto_render_on_present;
    stats.vsync_aligned = request.vsync_align;
    stats.frame = surface.latest_frame_info();

    auto wait_budget = request.vsync_deadline - request.now;
    if (wait_budget < std::chrono::steady_clock::duration::zero()) {
        wait_budget = std::chrono::steady_clock::duration::zero();
    }
    stats.wait_budget_ms =
        std::chrono::duration<double, std::milli>(wait_budget).count();

    auto const required_bytes = surface.frame_bytes();
    auto const row_stride = surface.row_stride_bytes();
    std::vector<std::uint8_t> scratch_framebuffer;
    bool allow_iosurface = request.allow_iosurface_sharing && SupportsIOSurfaceSharing();

    auto copy_progressive_tiles = [&](std::uint8_t* framebuffer_base,
                                      std::size_t framebuffer_stride,
                                      bool mark_present) -> bool {
        if (!surface.has_progressive() || request.dirty_tiles.empty()
            || framebuffer_base == nullptr || framebuffer_stride == 0) {
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
                auto attempt_copy = [&](int max_retries) {
                    for (int retry = 0; retry <= max_retries; ++retry) {
                        auto copy = progressive.copy_tile(tile_index, tile_storage);
                        if (copy) {
                            return copy;
                        }
                        if (retry < max_retries) {
                            std::this_thread::sleep_for(std::chrono::microseconds{10});
                        }
                    }
                    return std::optional<TileCopyResult>{};
                };
                auto tile_copy = attempt_copy(0);
                if (!tile_copy) {
                    ++stats.progressive_skip_seq_odd;
                    tile_copy = attempt_copy(4);
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
                    auto* dst = framebuffer_base
                                + (static_cast<std::size_t>(dims.y + row) * framebuffer_stride)
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

#if defined(__APPLE__)
    IOSurfaceRef iosurface_ref = nullptr;
    bool iosurface_locked = false;
    std::uint8_t* iosurface_base = nullptr;
    std::size_t iosurface_stride = row_stride;
    std::optional<PathSurfaceSoftware::SharedIOSurface> shared_surface;
    std::uint32_t iosurface_lock_mode = kIOSurfaceLockAvoidSync;

    if (allow_iosurface) {
        auto front_handle = surface.front_iosurface();
        if (front_handle && front_handle->valid()) {
            iosurface_ref = front_handle->surface();
            if (iosurface_ref) {
                if (IOSurfaceLock(iosurface_ref, iosurface_lock_mode, nullptr) != kIOReturnSuccess) {
                    iosurface_lock_mode = 0;
                    if (IOSurfaceLock(iosurface_ref, iosurface_lock_mode, nullptr) != kIOReturnSuccess) {
                        iosurface_ref = nullptr;
                    }
                }
                if (iosurface_ref) {
                    iosurface_locked = true;
                    iosurface_base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(iosurface_ref));
                    if (iosurface_base) {
                        iosurface_stride = IOSurfaceGetBytesPerRow(iosurface_ref);
                        shared_surface = std::move(front_handle);
                    } else {
                        IOSurfaceUnlock(iosurface_ref, iosurface_lock_mode, nullptr);
                        iosurface_locked = false;
                        iosurface_ref = nullptr;
                    }
                }
            }
        }
    }

    struct IOSurfaceLockGuard {
        IOSurfaceRef surface = nullptr;
        bool locked = false;
        std::uint32_t lock_mode = kIOSurfaceLockAvoidSync;
        ~IOSurfaceLockGuard() {
            if (locked && surface) {
                IOSurfaceUnlock(surface, lock_mode, nullptr);
            }
        }
    } iosurface_guard{iosurface_ref, iosurface_locked, iosurface_lock_mode};
#endif

#if defined(__APPLE__)
    if (shared_surface && iosurface_base) {
        stats.iosurface = *shared_surface;
        stats.used_iosurface = true;
        stats.presented = true;
        stats.buffered_frame_consumed = false;
        stats.frame = surface.latest_frame_info();
        (void)copy_progressive_tiles(iosurface_base, iosurface_stride, false);
        auto finish = std::chrono::steady_clock::now();
        stats.present_ms = std::chrono::duration<double, std::milli>(finish - start_time).count();
        return stats;
    }
#endif

    if (surface.has_buffered()) {
        std::span<std::uint8_t> framebuffer_span = request.framebuffer;
        if (required_bytes > 0 && framebuffer_span.size() < required_bytes) {
            scratch_framebuffer.resize(required_bytes);
            framebuffer_span = std::span<std::uint8_t>{scratch_framebuffer.data(), scratch_framebuffer.size()};
        }
        if (required_bytes > 0 && framebuffer_span.size() < required_bytes) {
            stats.skipped = true;
            stats.error = "framebuffer span too small for surface dimensions";
            auto finish = std::chrono::steady_clock::now();
            stats.present_ms = std::chrono::duration<double, std::milli>(finish - start_time).count();
            return stats;
        }

        auto copy = surface.copy_buffered_frame(framebuffer_span);
        if (copy) {
            stats.presented = true;
            stats.buffered_frame_consumed = true;
            stats.frame = copy->info;
            (void)copy_progressive_tiles(framebuffer_span.data(), row_stride, false);
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

    bool copied_progressive = false;
    if (!request.framebuffer.empty()) {
        copied_progressive = copy_progressive_tiles(request.framebuffer.data(), row_stride, true);
    }
    if (!copied_progressive) {
        stats.skipped = true;
    }
    auto finish = std::chrono::steady_clock::now();
    stats.present_ms = std::chrono::duration<double, std::milli>(finish - start_time).count();
    return stats;
}

} // namespace SP::UI
