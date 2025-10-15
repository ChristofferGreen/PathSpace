#include <pathspace/ui/ProgressiveSurfaceBuffer.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace SP::UI {

namespace {

constexpr std::size_t kBytesPerPixel = 4u;

auto clamp_positive(int value) -> int {
    if (value < 0) {
        throw std::invalid_argument("dimensions must be non-negative");
    }
    return value;
}

auto validate_tile_size(int tile, int width, int height) -> int {
    if (tile <= 0) {
        throw std::invalid_argument("tile size must be positive");
    }
    if (tile > std::max(width, height) && (width > 0 && height > 0)) {
        return std::max(width, height);
    }
    return tile;
}

} // namespace

ProgressiveSurfaceBuffer::ProgressiveSurfaceBuffer(int width_px, int height_px, int tile_size_px)
    : width_px_(clamp_positive(width_px))
    , height_px_(clamp_positive(height_px))
    , tile_size_px_(validate_tile_size(tile_size_px, width_px_, height_px_)) {
    if (width_px_ == 0 || height_px_ == 0) {
        tiles_x_ = 0;
        tiles_y_ = 0;
    } else {
        tiles_x_ = (width_px_ + tile_size_px_ - 1) / tile_size_px_;
        tiles_y_ = (height_px_ + tile_size_px_ - 1) / tile_size_px_;
    }
    auto const tile_count = static_cast<std::size_t>(tiles_x_) * static_cast<std::size_t>(tiles_y_);
    pixels_.resize(static_cast<std::size_t>(width_px_) * static_cast<std::size_t>(height_px_) * kBytesPerPixel);
    metadata_.resize(tile_count);
}

auto ProgressiveSurfaceBuffer::tile_dimensions(std::size_t tile_index) const -> TileDimensions {
    return tile_rect(tile_index);
}

ProgressiveSurfaceBuffer::TileWriter::TileWriter(ProgressiveSurfaceBuffer& buffer,
                                                 std::size_t tile_index,
                                                 TilePass pass)
    : buffer_(&buffer)
    , tile_index_(tile_index) {
    buffer.ensure_tile_index(tile_index_);
    auto& meta = buffer.metadata_[tile_index_];
    auto const previous = meta.seq.fetch_add(1u, std::memory_order_acq_rel);
    if (previous & 1u) {
        meta.seq.fetch_sub(1u, std::memory_order_acq_rel);
        throw std::logic_error("tile already in-flight");
    }
    if (pass == TilePass::OpaqueInProgress || pass == TilePass::AlphaInProgress) {
        meta.pass.store(static_cast<std::uint32_t>(pass), std::memory_order_release);
    } else if (pass != TilePass::None) {
        meta.seq.fetch_sub(1u, std::memory_order_acq_rel);
        throw std::invalid_argument("tile writes must begin with in-progress pass or None");
    }
    active_ = true;
}

ProgressiveSurfaceBuffer::TileWriter::TileWriter(TileWriter&& other) noexcept {
    *this = std::move(other);
}

auto ProgressiveSurfaceBuffer::TileWriter::operator=(TileWriter&& other) noexcept -> TileWriter& {
    if (this == &other) {
        return *this;
    }
    if (active_) {
        abort();
    }
    buffer_ = other.buffer_;
    tile_index_ = other.tile_index_;
    active_ = other.active_;
    other.buffer_ = nullptr;
    other.tile_index_ = 0;
    other.active_ = false;
    return *this;
}

ProgressiveSurfaceBuffer::TileWriter::~TileWriter() {
    if (active_) {
        abort();
    }
}

auto ProgressiveSurfaceBuffer::TileWriter::pixels() -> TilePixels {
    if (!buffer_) {
        return {};
    }
    auto const dims = buffer_->tile_rect(tile_index_);
    auto const offset = buffer_->byte_offset(dims);
    auto* base = buffer_->pixels_.data() + offset;
    return TilePixels{
        .data = base,
        .stride_bytes = buffer_->stride_bytes(),
        .dims = dims,
    };
}

void ProgressiveSurfaceBuffer::TileWriter::commit(TilePass completed_pass, std::uint64_t epoch) {
    if (!buffer_ || !active_) {
        return;
    }
    if (completed_pass != TilePass::OpaqueDone && completed_pass != TilePass::AlphaDone) {
        abort();
        throw std::invalid_argument("tile commit requires OpaqueDone or AlphaDone");
    }
    auto& meta = buffer_->metadata_[tile_index_];
    std::atomic_thread_fence(std::memory_order_release);
    meta.pass.store(static_cast<std::uint32_t>(completed_pass), std::memory_order_release);
    if (completed_pass == TilePass::AlphaDone) {
        meta.epoch.store(epoch, std::memory_order_release);
    }
    meta.seq.fetch_add(1u, std::memory_order_acq_rel);
    active_ = false;
}

void ProgressiveSurfaceBuffer::TileWriter::abort() {
    if (!buffer_ || !active_) {
        return;
    }
    auto& meta = buffer_->metadata_[tile_index_];
    meta.pass.store(static_cast<std::uint32_t>(TilePass::None), std::memory_order_release);
    meta.seq.fetch_add(1u, std::memory_order_acq_rel);
    active_ = false;
}

auto ProgressiveSurfaceBuffer::begin_tile_write(std::size_t tile_index, TilePass pass) -> TileWriter {
    return TileWriter{*this, tile_index, pass};
}

auto ProgressiveSurfaceBuffer::copy_tile(std::size_t tile_index,
                                         std::span<std::uint8_t> destination) const
    -> std::optional<TileCopyResult> {
    ensure_tile_index(tile_index);
    auto const dims = tile_rect(tile_index);
    auto const row_pitch = static_cast<std::size_t>(dims.width) * kBytesPerPixel;
    auto const required_bytes = static_cast<std::size_t>(dims.height) * row_pitch;
    if (destination.size() < required_bytes) {
        return std::nullopt;
    }

    auto const& meta = metadata_[tile_index];
    auto const pre_seq = meta.seq.load(std::memory_order_acquire);
    if (pre_seq & 1u) {
        return std::nullopt;
    }

    auto const offset = byte_offset(dims);
    auto const* base = pixels_.data() + offset;
    for (int row = 0; row < dims.height; ++row) {
        auto const src_offset = static_cast<std::size_t>(row) * stride_bytes();
        auto const dst_offset = static_cast<std::size_t>(row) * row_pitch;
        std::memcpy(destination.data() + dst_offset, base + src_offset, row_pitch);
    }

    auto const post_seq = meta.seq.load(std::memory_order_acquire);
    if ((post_seq & 1u) != 0u || post_seq != pre_seq) {
        return std::nullopt;
    }

    auto const pass_value = static_cast<TilePass>(meta.pass.load(std::memory_order_acquire));
    auto const epoch_value = meta.epoch.load(std::memory_order_acquire);

    return TileCopyResult{
        .pass = pass_value,
        .epoch = epoch_value,
    };
}

auto ProgressiveSurfaceBuffer::ensure_tile_index(std::size_t tile_index) const -> void {
    if (tile_index >= metadata_.size()) {
        throw std::out_of_range("tile index out of range");
    }
}

auto ProgressiveSurfaceBuffer::tile_rect(std::size_t tile_index) const -> TileDimensions {
    ensure_tile_index(tile_index);
    int const tx = static_cast<int>(tile_index % static_cast<std::size_t>(tiles_x_));
    int const ty = static_cast<int>(tile_index / static_cast<std::size_t>(tiles_x_));

    int const x0 = tx * tile_size_px_;
    int const y0 = ty * tile_size_px_;
    int const w = std::min(tile_size_px_, width_px_ - x0);
    int const h = std::min(tile_size_px_, height_px_ - y0);

    return TileDimensions{
        .x = x0,
        .y = y0,
        .width = std::max(w, 0),
        .height = std::max(h, 0),
    };
}

auto ProgressiveSurfaceBuffer::byte_offset(TileDimensions const& dims) const -> std::size_t {
    return (static_cast<std::size_t>(dims.y) * static_cast<std::size_t>(width_px_) + static_cast<std::size_t>(dims.x))
           * kBytesPerPixel;
}

} // namespace SP::UI
