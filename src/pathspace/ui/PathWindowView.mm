#include <pathspace/ui/PathWindowView.hpp>

#include <algorithm>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <Foundation/Foundation.h>
#include <Metal/Metal.h>
#include <QuartzCore/CAMetalLayer.h>
#include <dispatch/dispatch.h>
#include <IOSurface/IOSurface.h>
#endif

namespace SP::UI {

namespace {

constexpr std::size_t kBytesPerPixel = 4u;

#if defined(__APPLE__)
struct MetalPresenterState {
    CAMetalLayer* layer = nil;
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> command_queue = nil;
    double contents_scale = 1.0;
};

std::mutex gMetalPresenterMutex;
MetalPresenterState gMetalPresenterState{};

auto copy_presenter_state() -> MetalPresenterState {
    std::lock_guard<std::mutex> lock(gMetalPresenterMutex);
    return gMetalPresenterState;
}

auto command_buffer_status_string(MTLCommandBufferStatus status) -> char const* {
    switch (status) {
    case MTLCommandBufferStatusNotEnqueued:
        return "not enqueued";
    case MTLCommandBufferStatusEnqueued:
        return "enqueued";
    case MTLCommandBufferStatusCommitted:
        return "committed";
    case MTLCommandBufferStatusScheduled:
        return "scheduled";
    case MTLCommandBufferStatusCompleted:
        return "completed";
    case MTLCommandBufferStatusError:
        return "error";
    }
    return "unknown";
}
#endif

} // namespace

#if defined(__APPLE__)
void PathWindowView::ConfigureMetalPresenter(MetalPresenterConfig const& config) {
    std::lock_guard<std::mutex> lock(gMetalPresenterMutex);
    gMetalPresenterState.layer = (__bridge CAMetalLayer*)config.layer;
    gMetalPresenterState.device = (__bridge id<MTLDevice>)config.device;
    gMetalPresenterState.command_queue = (__bridge id<MTLCommandQueue>)config.command_queue;
    gMetalPresenterState.contents_scale = config.contents_scale > 0.0 ? config.contents_scale : 1.0;
}

void PathWindowView::ResetMetalPresenter() {
    std::lock_guard<std::mutex> lock(gMetalPresenterMutex);
    gMetalPresenterState = {};
}

static auto present_metal_texture(PathSurfaceSoftware& surface,
                                  PathWindowView::PresentStats& stats,
                                  PathWindowView::PresentRequest const& request) -> bool {
    PathSurfaceMetal::TextureInfo texture{};
    bool has_texture = false;
#if PATHSPACE_UI_METAL
    if (request.metal_surface != nullptr) {
        texture = request.metal_surface->acquire_texture();
        if (texture.texture != nullptr) {
            has_texture = true;
            stats.frame.frame_index = texture.frame_index;
            stats.frame.revision = texture.revision;
        }
    }
#endif
    if (!has_texture && request.has_metal_texture && request.metal_texture.texture != nullptr) {
        texture = request.metal_texture;
        has_texture = true;
        stats.frame.frame_index = texture.frame_index;
        stats.frame.revision = texture.revision;
    }
    if (!has_texture) {
        return false;
    }

    auto state = copy_presenter_state();
    if (state.layer == nil) {
        stats.error = "CAMetalLayer unavailable";
        return false;
    }
    if (state.command_queue == nil) {
        stats.error = "Metal command queue unavailable";
        return false;
    }

    auto const& desc = surface.desc();
    int width_px = request.surface_width_px > 0 ? request.surface_width_px : std::max(desc.size_px.width, 0);
    int height_px = request.surface_height_px > 0 ? request.surface_height_px : std::max(desc.size_px.height, 0);
    if (width_px <= 0 || height_px <= 0) {
        return false;
    }

    __block bool success = false;
    __block std::string error_message;
    __block double encode_ms = 0.0;
    __block double present_ms = 0.0;
    __block MTLCommandBufferStatus command_status = MTLCommandBufferStatusNotEnqueued;

    auto present_block = ^{
        @autoreleasepool {
            CAMetalLayer* layer = state.layer;
            if (!layer) {
                error_message = "CAMetalLayer unavailable";
                return;
            }
            id<MTLCommandQueue> queue = state.command_queue;
            if (!queue) {
                error_message = "Metal command queue unavailable";
                return;
            }
            id<MTLTexture> source = (__bridge id<MTLTexture>)texture.texture;
            if (!source) {
                error_message = "Metal texture unavailable";
                return;
            }

            CGFloat scale = state.contents_scale > 0.0 ? static_cast<CGFloat>(state.contents_scale) : (CGFloat)1.0;
            layer.contentsScale = scale;
            layer.drawableSize = CGSizeMake(static_cast<CGFloat>(width_px), static_cast<CGFloat>(height_px));

            id<CAMetalDrawable> drawable = [layer nextDrawable];
            if (!drawable) {
                error_message = "failed to acquire CAMetalDrawable";
                return;
            }

            id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
            if (!commandBuffer) {
                error_message = "failed to create Metal command buffer";
                return;
            }

            auto encode_start = std::chrono::steady_clock::now();
            id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
            if (!blit) {
                error_message = "failed to create Metal blit encoder";
                return;
            }
            MTLSize size = MTLSizeMake(width_px, height_px, 1);
            [blit copyFromTexture:source
                       sourceSlice:0
                        sourceLevel:0
                       sourceOrigin:MTLOriginMake(0, 0, 0)
                         sourceSize:size
                         toTexture:drawable.texture
                  destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            auto encode_finish = std::chrono::steady_clock::now();
            encode_ms = std::chrono::duration<double, std::milli>(encode_finish - encode_start).count();

            auto present_start = std::chrono::steady_clock::now();
            [commandBuffer presentDrawable:drawable];
            [commandBuffer commit];
            [commandBuffer waitUntilScheduled];
            auto present_finish = std::chrono::steady_clock::now();
            present_ms = std::chrono::duration<double, std::milli>(present_finish - present_start).count();
            [commandBuffer waitUntilCompleted];
            command_status = commandBuffer.status;
            if (command_status != MTLCommandBufferStatusCompleted) {
                NSError* command_error = commandBuffer.error;
                if (command_error) {
                    auto const* localized = [[command_error localizedDescription] UTF8String];
                    if (localized) {
                        error_message = std::string("Metal command buffer error: ") + localized;
                    } else {
                        error_message = "Metal command buffer error";
                    }
                } else {
                    error_message = std::string("Metal command buffer completed with status ")
                                    + command_buffer_status_string(command_status);
                }
                return;
            }
            success = true;
        }
    };

    auto call_start = std::chrono::steady_clock::now();
    if ([NSThread isMainThread]) {
        present_block();
    } else {
        dispatch_sync(dispatch_get_main_queue(), present_block);
    }
    auto call_finish = std::chrono::steady_clock::now();

    if (success) {
        stats.presented = true;
        stats.used_metal_texture = true;
        stats.buffered_frame_consumed = false;
        stats.gpu_encode_ms = encode_ms;
        stats.gpu_present_ms = present_ms;
        stats.backend_kind = "Metal2D";
        stats.present_ms = std::chrono::duration<double, std::milli>(call_finish - call_start).count();
        stats.skipped = false;
        stats.error.clear();
    } else if (!error_message.empty()) {
        stats.error = std::move(error_message);
    }

    return success;
}
#endif

auto PathWindowView::present(PathSurfaceSoftware& surface,
                             PresentPolicy const& policy,
                             PresentRequest const& request) -> PresentStats {
    PresentStats stats{};
    stats.used_metal_texture = false;
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

#if defined(__APPLE__)
    if (present_metal_texture(surface, stats, request)) {
        return stats;
    }
#endif

    auto const required_bytes = surface.frame_bytes();
    auto const row_stride = surface.row_stride_bytes();

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

    if (request.allow_iosurface_sharing) {
        auto front_handle = surface.front_iosurface();
        if (front_handle && front_handle->valid()) {
            iosurface_ref = front_handle->surface();
            if (iosurface_ref
                && IOSurfaceLock(iosurface_ref, kIOSurfaceLockAvoidSync, nullptr) == kIOReturnSuccess) {
                iosurface_locked = true;
                iosurface_base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(iosurface_ref));
                if (iosurface_base) {
                    iosurface_stride = IOSurfaceGetBytesPerRow(iosurface_ref);
                    shared_surface = std::move(front_handle);
                } else {
                    IOSurfaceUnlock(iosurface_ref, kIOSurfaceLockAvoidSync, nullptr);
                    iosurface_locked = false;
                    iosurface_ref = nullptr;
                }
            }
        }
    }

    struct IOSurfaceLockGuard {
        IOSurfaceRef surface = nullptr;
        bool locked = false;
        ~IOSurfaceLockGuard() {
            if (locked && surface) {
                IOSurfaceUnlock(surface, kIOSurfaceLockAvoidSync, nullptr);
            }
        }
    } iosurface_guard{iosurface_ref, iosurface_locked};
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
        if (required_bytes > 0 && request.framebuffer.size() < required_bytes) {
            stats.skipped = true;
            stats.error = "framebuffer span too small for surface dimensions";
            auto finish = std::chrono::steady_clock::now();
            stats.present_ms = std::chrono::duration<double, std::milli>(finish - start_time).count();
            return stats;
        }

        auto copy = surface.copy_buffered_frame(request.framebuffer);
        if (copy) {
            stats.presented = true;
            stats.buffered_frame_consumed = true;
            stats.frame = copy->info;
            (void)copy_progressive_tiles(request.framebuffer.data(), row_stride, false);
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
