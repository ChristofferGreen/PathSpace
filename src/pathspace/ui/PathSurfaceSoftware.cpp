#include <pathspace/ui/PathSurfaceSoftware.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

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

} // namespace

PathSurfaceSoftware::PathSurfaceSoftware(Builders::SurfaceDesc desc)
    : PathSurfaceSoftware(std::move(desc), Options{}) {}

PathSurfaceSoftware::PathSurfaceSoftware(Builders::SurfaceDesc desc, Options options)
    : desc_(std::move(desc))
    , options_(options) {
    reallocate_buffers();
    reset_progressive();
}

void PathSurfaceSoftware::resize(Builders::SurfaceDesc const& desc) {
    desc_ = desc;
    reallocate_buffers();
    reset_progressive();
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

auto PathSurfaceSoftware::begin_progressive_tile(std::size_t tile_index, TilePass pass)
    -> ProgressiveSurfaceBuffer::TileWriter {
    return progressive_buffer().begin_tile_write(tile_index, pass);
}

auto PathSurfaceSoftware::staging_span() -> std::span<std::uint8_t> {
    if (!has_buffered()) {
        return {};
    }
    staging_dirty_ = true;
    return std::span<std::uint8_t>{staging_.data(), staging_.size()};
}

void PathSurfaceSoftware::publish_buffered_frame(FrameInfo info) {
    if (!has_buffered() || !staging_dirty_) {
        return;
    }

    front_.swap(staging_);
    staging_dirty_ = false;

    buffered_frame_index_.store(info.frame_index, std::memory_order_release);
    buffered_revision_.store(info.revision, std::memory_order_release);
    buffered_render_ns_.store(to_ns(info.render_ms), std::memory_order_release);
    buffered_epoch_.fetch_add(1, std::memory_order_acq_rel);
}

void PathSurfaceSoftware::discard_staging() {
    staging_dirty_ = false;
}

auto PathSurfaceSoftware::copy_buffered_frame(std::span<std::uint8_t> destination) const
    -> std::optional<BufferedFrameCopy> {
    if (!has_buffered()) {
        return std::nullopt;
    }
    if (destination.size() < front_.size()) {
        return std::nullopt;
    }

    auto const epoch_before = buffered_epoch_.load(std::memory_order_acquire);
    if (epoch_before == 0) {
        return std::nullopt;
    }

    auto const frame_index = buffered_frame_index_.load(std::memory_order_acquire);
    auto const revision = buffered_revision_.load(std::memory_order_acquire);
    auto const render_ns = buffered_render_ns_.load(std::memory_order_acquire);

    std::memcpy(destination.data(), front_.data(), front_.size());

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
    if (options_.enable_buffered) {
        staging_.assign(frame_bytes_, 0);
        front_.assign(frame_bytes_, 0);
    } else {
        staging_.clear();
        front_.clear();
    }
}

void PathSurfaceSoftware::reset_progressive() {
    if (!options_.enable_progressive || desc_.size_px.width <= 0 || desc_.size_px.height <= 0) {
        progressive_.reset();
        return;
    }
    progressive_ = std::make_unique<ProgressiveSurfaceBuffer>(
        desc_.size_px.width,
        desc_.size_px.height,
        std::max(1, options_.progressive_tile_size_px));
}

} // namespace SP::UI
