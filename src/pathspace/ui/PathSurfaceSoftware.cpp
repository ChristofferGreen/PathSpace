#include <pathspace/ui/PathSurfaceSoftware.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#endif

namespace SP::UI {

namespace {

constexpr std::size_t kBytesPerPixel = 4u;
constexpr std::uint64_t kNsPerMs = 1'000'000;

auto clamp_non_negative(int value) -> int {
    return value < 0 ? 0 : value;
}

auto frame_bytes_for(Builders::SurfaceDesc const& desc) -> std::size_t {
    auto const width = clamp_non_negative(desc.size_px.width);
    auto const height = clamp_non_negative(desc.size_px.height);
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * kBytesPerPixel;
}

auto stride_for(Builders::SurfaceDesc const& desc) -> std::size_t {
    auto const width = clamp_non_negative(desc.size_px.width);
    return static_cast<std::size_t>(width) * kBytesPerPixel;
}

auto to_ns(double render_ms) -> std::uint64_t {
    if (render_ms <= 0.0) {
        return 0;
    }
    auto const clamped = std::max(render_ms, 0.0);
    return static_cast<std::uint64_t>(std::llround(clamped * static_cast<double>(kNsPerMs)));
}

auto to_ms(std::uint64_t render_ns) -> double {
    if (render_ns == 0) {
        return 0.0;
    }
    return static_cast<double>(render_ns) / static_cast<double>(kNsPerMs);
}

#if defined(__APPLE__)
auto align_to(std::int32_t value, std::int32_t alignment) -> std::int32_t {
    if (alignment <= 0) {
        return value;
    }
    auto remainder = value % alignment;
    if (remainder == 0) {
        return value;
    }
    return value + (alignment - remainder);
}

auto make_cf_number(std::int32_t value) -> CFNumberRef {
    return CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &value);
}

auto make_iosurface(int width, int height) -> IOSurfaceRef {
    if (width <= 0 || height <= 0) {
        return nullptr;
    }

    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault,
        0,
        &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!dict) {
        return nullptr;
    }

    auto set_number = [&](CFStringRef key, std::int32_t numberValue) {
        CFNumberRef number = make_cf_number(numberValue);
        if (!number) {
            return;
        }
        CFDictionarySetValue(dict, key, number);
        CFRelease(number);
    };

    set_number(kIOSurfaceWidth, width);
    set_number(kIOSurfaceHeight, height);
    set_number(kIOSurfaceBytesPerElement, static_cast<std::int32_t>(kBytesPerPixel));
    auto row_bytes = width * static_cast<int>(kBytesPerPixel);
    row_bytes = align_to(row_bytes, 16);
    set_number(kIOSurfaceBytesPerRow, row_bytes);
    set_number(kIOSurfaceElementWidth, 1);
    set_number(kIOSurfaceElementHeight, 1);
    set_number(kIOSurfacePixelFormat, static_cast<std::int32_t>(kCVPixelFormatType_32BGRA));

    IOSurfaceRef surface = IOSurfaceCreate(dict);
    CFRelease(dict);
    return surface;
}

auto iosurface_span_size(IOSurfaceRef surface, int height) -> std::size_t {
    if (!surface || height <= 0) {
        return 0;
    }
    auto row_bytes = IOSurfaceGetBytesPerRow(surface);
    return row_bytes * static_cast<std::size_t>(height);
}

void zero_iosurface(IOSurfaceRef surface, int height) {
    if (!surface || height <= 0) {
        return;
    }
    if (IOSurfaceLock(surface, 0, nullptr) != kIOReturnSuccess) {
        return;
    }
    auto* base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(surface));
    auto const bytes = iosurface_span_size(surface, height);
    if (base && bytes > 0) {
        std::memset(base, 0, bytes);
    }
    IOSurfaceUnlock(surface, 0, nullptr);
}
#endif

} // namespace

#if defined(__APPLE__)
PathSurfaceSoftware::SharedIOSurface::SharedIOSurface(IOSurfaceRef surface,
                                                     int width,
                                                     int height,
                                                     std::size_t row_bytes)
    : surface_(surface)
    , width_(width)
    , height_(height)
    , row_bytes_(row_bytes) {
    if (surface_) {
        CFRetain(surface_);
    }
}

PathSurfaceSoftware::SharedIOSurface::SharedIOSurface(SharedIOSurface const& other)
    : surface_(other.surface_)
    , width_(other.width_)
    , height_(other.height_)
    , row_bytes_(other.row_bytes_) {
    if (surface_) {
        CFRetain(surface_);
    }
}

auto PathSurfaceSoftware::SharedIOSurface::operator=(SharedIOSurface const& other)
    -> SharedIOSurface& {
    if (this == &other) {
        return *this;
    }
    if (surface_) {
        CFRelease(surface_);
    }
    surface_ = other.surface_;
    width_ = other.width_;
    height_ = other.height_;
    row_bytes_ = other.row_bytes_;
    if (surface_) {
        CFRetain(surface_);
    }
    return *this;
}

PathSurfaceSoftware::SharedIOSurface::SharedIOSurface(SharedIOSurface&& other) noexcept
    : surface_(other.surface_)
    , width_(other.width_)
    , height_(other.height_)
    , row_bytes_(other.row_bytes_) {
    other.surface_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
    other.row_bytes_ = 0;
}

auto PathSurfaceSoftware::SharedIOSurface::operator=(SharedIOSurface&& other) noexcept
    -> SharedIOSurface& {
    if (this == &other) {
        return *this;
    }
    if (surface_) {
        CFRelease(surface_);
    }
    surface_ = other.surface_;
    width_ = other.width_;
    height_ = other.height_;
    row_bytes_ = other.row_bytes_;
    other.surface_ = nullptr;
    other.width_ = 0;
    other.height_ = 0;
    other.row_bytes_ = 0;
    return *this;
}

PathSurfaceSoftware::SharedIOSurface::~SharedIOSurface() {
    if (surface_) {
        CFRelease(surface_);
    }
}

auto PathSurfaceSoftware::SharedIOSurface::retain_for_external_use() const -> IOSurfaceRef {
    if (surface_) {
        CFRetain(surface_);
    }
    return surface_;
}

PathSurfaceSoftware::IOSurfaceHolder::IOSurfaceHolder(IOSurfaceRef surface)
    : surface_(surface) {}

PathSurfaceSoftware::IOSurfaceHolder::IOSurfaceHolder(IOSurfaceHolder&& other) noexcept
    : surface_(other.surface_) {
    other.surface_ = nullptr;
}

auto PathSurfaceSoftware::IOSurfaceHolder::operator=(IOSurfaceHolder&& other) noexcept
    -> IOSurfaceHolder& {
    if (this == &other) {
        return *this;
    }
    reset();
    surface_ = other.surface_;
    other.surface_ = nullptr;
    return *this;
}

PathSurfaceSoftware::IOSurfaceHolder::~IOSurfaceHolder() {
    reset();
}

void PathSurfaceSoftware::IOSurfaceHolder::reset(IOSurfaceRef surface) {
    if (surface_) {
        CFRelease(surface_);
    }
    surface_ = surface;
}

void PathSurfaceSoftware::IOSurfaceHolder::swap(IOSurfaceHolder& other) noexcept {
    std::swap(surface_, other.surface_);
}
#endif

PathSurfaceSoftware::PathSurfaceSoftware(Builders::SurfaceDesc desc)
    : PathSurfaceSoftware(std::move(desc), Options{}) {}

PathSurfaceSoftware::PathSurfaceSoftware(Builders::SurfaceDesc desc, Options options)
    : desc_(std::move(desc))
    , options_(options) {
    options_.progressive_tile_size_px = std::max(64, options_.progressive_tile_size_px);
    configured_progressive_tile_size_px_ = options_.progressive_tile_size_px;
    reallocate_buffers();
    reset_progressive();
}

void PathSurfaceSoftware::resize(Builders::SurfaceDesc const& desc) {
    desc_ = desc;
    options_.progressive_tile_size_px = std::max(64, options_.progressive_tile_size_px);
    reallocate_buffers();
    reset_progressive();
    configured_progressive_tile_size_px_ = std::max(1, options_.progressive_tile_size_px);
    staging_dirty_ = false;
    buffered_epoch_.store(0, std::memory_order_release);
    buffered_frame_index_.store(0, std::memory_order_release);
    buffered_revision_.store(0, std::memory_order_release);
    buffered_render_ns_.store(0, std::memory_order_release);
}

auto PathSurfaceSoftware::progressive_buffer() -> ProgressiveSurfaceBuffer& {
    if (!progressive_) {
        throw std::runtime_error("progressive buffer disabled");
    }
    return *progressive_;
}

auto PathSurfaceSoftware::progressive_buffer() const -> ProgressiveSurfaceBuffer const& {
    if (!progressive_) {
        throw std::runtime_error("progressive buffer disabled");
    }
    return *progressive_;
}

auto PathSurfaceSoftware::progressive_tile_size() const -> int {
    if (progressive_) {
        return progressive_->tile_size();
    }
    return std::max(64, options_.progressive_tile_size_px);
}

void PathSurfaceSoftware::ensure_progressive_tile_size(int tile_size_px) {
    if (!options_.enable_progressive) {
        return;
    }
    auto const clamped = std::max(64, tile_size_px);
    if (configured_progressive_tile_size_px_ == clamped
        && progressive_
        && progressive_->tile_size() == clamped) {
        return;
    }
    options_.progressive_tile_size_px = clamped;
    configured_progressive_tile_size_px_ = clamped;
    reset_progressive();
}

auto PathSurfaceSoftware::begin_progressive_tile(std::size_t tile_index, TilePass pass)
    -> ProgressiveSurfaceBuffer::TileWriter {
    return progressive_buffer().begin_tile_write(tile_index, pass);
}

auto PathSurfaceSoftware::staging_span() -> std::span<std::uint8_t> {
    if (!has_buffered()) {
        return {};
    }
#if defined(__APPLE__)
    auto* surface = staging_surface_.get();
    if (!surface) {
        return {};
    }
    auto const height = clamp_non_negative(desc_.size_px.height);
    if (height <= 0) {
        return {};
    }
    if (staging_locked_) {
        IOSurfaceUnlock(surface, kIOSurfaceLockAvoidSync, nullptr);
        staging_locked_ = false;
    }
    if (IOSurfaceLock(surface, kIOSurfaceLockAvoidSync, nullptr) != kIOReturnSuccess) {
        return {};
    }
    staging_locked_ = true;
    auto* base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(surface));
    auto const bytes = iosurface_span_size(surface, height);
    if (!base || bytes == 0) {
        IOSurfaceUnlock(surface, kIOSurfaceLockAvoidSync, nullptr);
        staging_locked_ = false;
        return {};
    }
    if (staging_sync_pending_) {
        auto const stride = static_cast<std::size_t>(IOSurfaceGetBytesPerRow(surface));
        if (copy_front_into_locked_staging(base,
                                           stride,
                                           desc_.size_px.width,
                                           desc_.size_px.height)) {
            clear_staging_sync();
        }
    }
    staging_dirty_ = true;
    return std::span<std::uint8_t>{base, bytes};
#else
    if (staging_sync_pending_) {
        if (!front_.empty()) {
            staging_ = front_;
        }
        clear_staging_sync();
    }
    staging_dirty_ = true;
    return std::span<std::uint8_t>{staging_.data(), staging_.size()};
#endif
}

void PathSurfaceSoftware::publish_buffered_frame(FrameInfo info) {
    if (has_buffered()) {
        if (!staging_dirty_) {
            record_frame_info(info);
            return;
        }

#if defined(__APPLE__)
        if (staging_locked_ && staging_surface_.get()) {
            IOSurfaceUnlock(staging_surface_.get(), kIOSurfaceLockAvoidSync, nullptr);
            staging_locked_ = false;
        }
        front_surface_.swap(staging_surface_);
#else
        front_.swap(staging_);
#endif
        staging_dirty_ = false;
    }

    record_frame_info(info);
    mark_staging_sync_needed();
}

void PathSurfaceSoftware::discard_staging() {
#if defined(__APPLE__)
    if (staging_locked_ && staging_surface_.get()) {
        IOSurfaceUnlock(staging_surface_.get(), kIOSurfaceLockAvoidSync, nullptr);
        staging_locked_ = false;
    }
#endif
    staging_dirty_ = false;
}

void PathSurfaceSoftware::mark_staging_sync_needed() {
    staging_sync_pending_ = has_buffered();
}

void PathSurfaceSoftware::clear_staging_sync() {
    staging_sync_pending_ = false;
}

#if defined(__APPLE__)
bool PathSurfaceSoftware::copy_front_into_locked_staging(std::uint8_t* staging_base,
                                                         std::size_t staging_stride,
                                                         int width,
                                                         int height) {
    auto* front_surface = front_surface_.get();
    if (!front_surface || !staging_base) {
        return false;
    }
    width = clamp_non_negative(width);
    height = clamp_non_negative(height);
    if (width == 0 || height == 0) {
        return false;
    }

    std::uint32_t lock_mode = kIOSurfaceLockAvoidSync;
    if (IOSurfaceLock(front_surface, lock_mode, nullptr) != kIOReturnSuccess) {
        lock_mode = 0;
        if (IOSurfaceLock(front_surface, lock_mode, nullptr) != kIOReturnSuccess) {
            return false;
        }
    }
    auto* front_base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(front_surface));
    if (!front_base) {
        IOSurfaceUnlock(front_surface, lock_mode, nullptr);
        return false;
    }

    auto const front_stride = IOSurfaceGetBytesPerRow(front_surface);
    if (front_stride <= 0) {
        IOSurfaceUnlock(front_surface, lock_mode, nullptr);
        return false;
    }

    auto const row_bytes = static_cast<std::size_t>(width) * kBytesPerPixel;
    auto const src_stride = static_cast<std::size_t>(front_stride);
    for (int row = 0; row < height; ++row) {
        auto const src_offset = static_cast<std::size_t>(row) * src_stride;
        auto const dst_offset = static_cast<std::size_t>(row) * staging_stride;
        std::memcpy(staging_base + dst_offset, front_base + src_offset, row_bytes);
    }

    IOSurfaceUnlock(front_surface, lock_mode, nullptr);
    return true;
}
#endif

void PathSurfaceSoftware::record_frame_info(FrameInfo info) {
    buffered_frame_index_.store(info.frame_index, std::memory_order_release);
    buffered_revision_.store(info.revision, std::memory_order_release);
    buffered_render_ns_.store(to_ns(info.render_ms), std::memory_order_release);
    buffered_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

auto PathSurfaceSoftware::latest_frame_info() const -> FrameInfo {
    FrameInfo info{};
    info.frame_index = buffered_frame_index_.load(std::memory_order_acquire);
    info.revision = buffered_revision_.load(std::memory_order_acquire);
    info.render_ms = to_ms(buffered_render_ns_.load(std::memory_order_acquire));
    return info;
}

void PathSurfaceSoftware::mark_progressive_dirty(std::size_t tile_index) {
    if (!progressive_) {
        return;
    }
    progressive_dirty_tiles_.push_back(tile_index);
}

auto PathSurfaceSoftware::progressive_tile_count() const -> std::size_t {
    if (!progressive_) {
        return 0;
    }
    return progressive_->tile_count();
}

auto PathSurfaceSoftware::consume_progressive_dirty_tiles() -> std::vector<std::size_t> {
    if (progressive_dirty_tiles_.empty()) {
        return {};
    }
    std::vector<std::size_t> tiles;
    tiles.swap(progressive_dirty_tiles_);
    std::sort(tiles.begin(), tiles.end());
    tiles.erase(std::unique(tiles.begin(), tiles.end()), tiles.end());
    return tiles;
}

auto PathSurfaceSoftware::copy_buffered_frame(std::span<std::uint8_t> destination) const
    -> std::optional<BufferedFrameCopy> {
    if (!has_buffered()) {
        return std::nullopt;
    }
#if defined(__APPLE__)
    auto* surface = front_surface_.get();
    if (!surface) {
        return std::nullopt;
    }
    auto const height = clamp_non_negative(desc_.size_px.height);
    if (height <= 0) {
        return std::nullopt;
    }
    auto const required_bytes = iosurface_span_size(surface, height);
    if (destination.size() < required_bytes) {
        return std::nullopt;
    }
#else
    auto const required_bytes = front_.size();
    if (destination.size() < required_bytes) {
        return std::nullopt;
    }
#endif

    auto const epoch_before = buffered_epoch_.load(std::memory_order_acquire);
    if (epoch_before == 0) {
        return std::nullopt;
    }

    auto const frame_index = buffered_frame_index_.load(std::memory_order_acquire);
    auto const revision = buffered_revision_.load(std::memory_order_acquire);
    auto const render_ns = buffered_render_ns_.load(std::memory_order_acquire);

#if defined(__APPLE__)
    if (IOSurfaceLock(surface, kIOSurfaceLockAvoidSync, nullptr) != kIOReturnSuccess) {
        return std::nullopt;
    }
    auto* base = static_cast<std::uint8_t*>(IOSurfaceGetBaseAddress(surface));
    if (!base || required_bytes == 0) {
        IOSurfaceUnlock(surface, kIOSurfaceLockAvoidSync, nullptr);
        return std::nullopt;
    }
    std::memcpy(destination.data(), base, required_bytes);
    IOSurfaceUnlock(surface, kIOSurfaceLockAvoidSync, nullptr);
#else
    std::memcpy(destination.data(), front_.data(), required_bytes);
#endif

    auto const epoch_after = buffered_epoch_.load(std::memory_order_acquire);
    if (epoch_before != epoch_after) {
        return std::nullopt;
    }

    return BufferedFrameCopy{
        .info = FrameInfo{
            .frame_index = frame_index,
            .revision = revision,
            .render_ms = to_ms(render_ns),
        },
    };
}

void PathSurfaceSoftware::reallocate_buffers() {
    frame_bytes_ = frame_bytes_for(desc_);
    row_stride_bytes_ = stride_for(desc_);
#if defined(__APPLE__)
    auto const width = clamp_non_negative(desc_.size_px.width);
    auto const height = clamp_non_negative(desc_.size_px.height);
    if (staging_locked_ && staging_surface_.get()) {
        IOSurfaceUnlock(staging_surface_.get(), kIOSurfaceLockAvoidSync, nullptr);
        staging_locked_ = false;
    }
    staging_surface_.reset();
    front_surface_.reset();
    if (options_.enable_buffered && width > 0 && height > 0 && frame_bytes_ > 0) {
        staging_surface_.reset(make_iosurface(width, height));
        front_surface_.reset(make_iosurface(width, height));
        zero_iosurface(staging_surface_.get(), height);
        zero_iosurface(front_surface_.get(), height);
        if (auto* s = staging_surface_.get()) {
            row_stride_bytes_ = IOSurfaceGetBytesPerRow(s);
            frame_bytes_ = iosurface_span_size(s, height);
        }
    } else {
        frame_bytes_ = static_cast<std::size_t>(width)
                       * static_cast<std::size_t>(height) * kBytesPerPixel;
        row_stride_bytes_ = stride_for(desc_);
    }
#else
    if (options_.enable_buffered) {
        staging_.assign(frame_bytes_, 0);
        front_.assign(frame_bytes_, 0);
    } else {
        staging_.clear();
        front_.clear();
    }
#endif
    staging_sync_pending_ = false;
}

#if defined(__APPLE__)
auto PathSurfaceSoftware::front_iosurface() const -> std::optional<SharedIOSurface> {
    if (!options_.enable_buffered) {
        return std::nullopt;
    }
    auto const width = clamp_non_negative(desc_.size_px.width);
    auto const height = clamp_non_negative(desc_.size_px.height);
    if (width <= 0 || height <= 0) {
        return std::nullopt;
    }
    if (buffered_epoch_.load(std::memory_order_acquire) == 0) {
        return std::nullopt;
    }
    auto* surface = front_surface_.get();
    if (!surface) {
        return std::nullopt;
    }
    auto const row_bytes = IOSurfaceGetBytesPerRow(surface);
    if (row_bytes == 0) {
        return std::nullopt;
    }
    return SharedIOSurface{surface, width, height, row_bytes};
}
#endif

void PathSurfaceSoftware::reset_progressive() {
    if (!options_.enable_progressive || desc_.size_px.width <= 0 || desc_.size_px.height <= 0) {
        progressive_.reset();
        progressive_dirty_tiles_.clear();
        return;
    }
    progressive_ = std::make_unique<ProgressiveSurfaceBuffer>(
        desc_.size_px.width,
        desc_.size_px.height,
        std::max(64, options_.progressive_tile_size_px));
    progressive_dirty_tiles_.clear();
    configured_progressive_tile_size_px_ = std::max(64, options_.progressive_tile_size_px);
}

} // namespace SP::UI
