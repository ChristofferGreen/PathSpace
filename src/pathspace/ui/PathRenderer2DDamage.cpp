#include "PathRenderer2DInternal.hpp"

#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/ProgressiveSurfaceBuffer.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace SP::UI::PathRenderer2DInternal {

auto DamageRect::from_bounds(PathRenderer2D::DrawableBounds const& bounds) -> DamageRect {
    return DamageRect{
        .min_x = bounds.min_x,
        .min_y = bounds.min_y,
        .max_x = bounds.max_x,
        .max_y = bounds.max_y,
    };
}

auto DamageRect::clamp(int width, int height) -> void {
    min_x = std::clamp(min_x, 0, width);
    min_y = std::clamp(min_y, 0, height);
    max_x = std::clamp(max_x, 0, width);
    max_y = std::clamp(max_y, 0, height);
}

auto DamageRect::expand(int margin, int width, int height) -> void {
    min_x = std::clamp(min_x - margin, 0, width);
    min_y = std::clamp(min_y - margin, 0, height);
    max_x = std::clamp(max_x + margin, 0, width);
    max_y = std::clamp(max_y + margin, 0, height);
}

auto DamageRect::empty() const -> bool {
    return min_x >= max_x || min_y >= max_y;
}

auto DamageRect::width() const -> int {
    return empty() ? 0 : (max_x - min_x);
}

auto DamageRect::height() const -> int {
    return empty() ? 0 : (max_y - min_y);
}

auto DamageRect::area() const -> std::uint64_t {
    if (empty()) {
        return 0;
    }
    return static_cast<std::uint64_t>(width()) * static_cast<std::uint64_t>(height());
}

auto DamageRect::merge(DamageRect const& other) -> void {
    min_x = std::min(min_x, other.min_x);
    min_y = std::min(min_y, other.min_y);
    max_x = std::max(max_x, other.max_x);
    max_y = std::max(max_y, other.max_y);
}

auto DamageRect::overlaps_or_touches(DamageRect const& other) const -> bool {
    return !(max_x <= other.min_x || other.max_x <= min_x
             || max_y <= other.min_y || other.max_y <= min_y);
}

auto DamageRect::intersects(PathRenderer2D::DrawableBounds const& bounds) const -> bool {
    if (bounds.empty() || empty()) {
        return false;
    }
    return !(bounds.max_x <= min_x || bounds.min_x >= max_x
             || bounds.max_y <= min_y || bounds.min_y >= max_y);
}

auto DamageRect::intersects(TileDimensions const& tile) const -> bool {
    if (empty() || tile.width <= 0 || tile.height <= 0) {
        return false;
    }
    auto tile_max_x = tile.x + tile.width;
    auto tile_max_y = tile.y + tile.height;
    return !(tile_max_x <= min_x || tile.x >= max_x
             || tile_max_y <= min_y || tile.y >= max_y);
}

auto DamageRect::intersect(DamageRect const& other) const -> DamageRect {
    DamageRect result{};
    result.min_x = std::max(min_x, other.min_x);
    result.min_y = std::max(min_y, other.min_y);
    result.max_x = std::min(max_x, other.max_x);
    result.max_y = std::min(max_y, other.max_y);
    if (result.empty()) {
        return DamageRect{};
    }
    return result;
}

void DamageRegion::set_full(int width, int height) {
    full_surface_ = true;
    rects_.clear();
    rects_.push_back(DamageRect{
        .min_x = 0,
        .min_y = 0,
        .max_x = width,
        .max_y = height,
    });
}

void DamageRegion::add(PathRenderer2D::DrawableBounds const& bounds,
                       int width,
                       int height,
                       int margin) {
    if (full_surface_ || bounds.empty()) {
        return;
    }
    DamageRect rect = DamageRect::from_bounds(bounds);
    rect.expand(margin, width, height);
    rect.clamp(width, height);
    if (rect.empty()) {
        return;
    }
    rects_.push_back(rect);
}

void DamageRegion::add_rect(DamageRect rect, int width, int height) {
    if (full_surface_) {
        return;
    }
    rect.clamp(width, height);
    if (rect.empty()) {
        return;
    }
    rects_.push_back(rect);
}

void DamageRegion::finalize(int width, int height) {
    if (full_surface_) {
        if (!rects_.empty()) {
            rects_.front().clamp(width, height);
        }
        return;
    }
    for (auto& rect : rects_) {
        rect.clamp(width, height);
    }
    rects_.erase(std::remove_if(rects_.begin(),
                                rects_.end(),
                                [](DamageRect const& rect) { return rect.empty(); }),
                 rects_.end());
    merge_overlaps();
}

auto DamageRegion::empty() const -> bool {
    return rects_.empty();
}

auto DamageRegion::intersects(PathRenderer2D::DrawableBounds const& bounds) const -> bool {
    if (bounds.empty()) {
        return false;
    }
    for (auto const& rect : rects_) {
        if (rect.intersects(bounds)) {
            return true;
        }
    }
    return false;
}

auto DamageRegion::intersects(TileDimensions const& tile) const -> bool {
    for (auto const& rect : rects_) {
        if (rect.intersects(tile)) {
            return true;
        }
    }
    return false;
}

auto DamageRegion::rectangles() const -> std::span<DamageRect const> {
    return rects_;
}

auto DamageRegion::area() const -> std::uint64_t {
    std::uint64_t total = 0;
    for (auto const& rect : rects_) {
        total += rect.area();
    }
    return total;
}

auto DamageRegion::coverage_ratio(int width, int height) const -> double {
    if (rects_.empty()) {
        return 0.0;
    }
    if (width <= 0 || height <= 0) {
        return 0.0;
    }
    auto surface = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    if (surface == 0) {
        return 0.0;
    }
    auto damaged = area();
    if (damaged == 0) {
        return 0.0;
    }
    return static_cast<double>(damaged) / static_cast<double>(surface);
}

void DamageRegion::collect_progressive_tiles(ProgressiveSurfaceBuffer const& buffer,
                                             std::vector<std::size_t>& out) const {
    if (rects_.empty()) {
        return;
    }
    auto const tile_count = buffer.tile_count();
    if (tile_count == 0) {
        return;
    }
    auto const tiles_x = buffer.tiles_x();
    auto const tiles_y = buffer.tiles_y();
    auto const tile_size = std::max(buffer.tile_size(), 1);
    std::vector<std::uint8_t> seen(tile_count, 0);
    auto push_tile = [&](int tx, int ty) {
        if (tx < 0 || ty < 0 || tx >= tiles_x || ty >= tiles_y) {
            return;
        }
        auto index = static_cast<std::size_t>(ty) * static_cast<std::size_t>(tiles_x)
                     + static_cast<std::size_t>(tx);
        if (index >= tile_count || seen[index]) {
            return;
        }
        seen[index] = 1;
        out.push_back(index);
    };

    for (auto const& rect : rects_) {
        if (rect.empty()) {
            continue;
        }
        int const min_x = rect.min_x;
        int const min_y = rect.min_y;
        int const max_x = rect.max_x;
        int const max_y = rect.max_y;

        int min_tx = min_x / tile_size;
        int min_ty = min_y / tile_size;
        int max_tx = (std::max(max_x - 1, min_x) / tile_size);
        int max_ty = (std::max(max_y - 1, min_y) / tile_size);

        min_tx = std::max(min_tx, 0);
        min_ty = std::max(min_ty, 0);
        max_tx = std::min(max_tx, tiles_x - 1);
        max_ty = std::min(max_ty, tiles_y - 1);

        for (int ty = min_ty; ty <= max_ty; ++ty) {
            for (int tx = min_tx; tx <= max_tx; ++tx) {
                push_tile(tx, ty);
            }
        }
    }
}

void DamageRegion::restrict_to(std::span<DamageRect const> limits) {
    if (full_surface_ || limits.empty()) {
        return;
    }
    std::vector<DamageRect> reduced;
    reduced.reserve(rects_.size());
    for (auto const& rect : rects_) {
        bool intersected = false;
        for (auto const& limit : limits) {
            if (limit.empty()) {
                continue;
            }
            auto intersection = rect.intersect(limit);
            if (!intersection.empty()) {
                reduced.push_back(intersection);
                intersected = true;
            }
        }
        if (!intersected) {
            reduced.push_back(rect);
        }
    }
    rects_.swap(reduced);
    merge_overlaps();
}

void DamageRegion::replace_with_rects(std::span<DamageRect const> rects,
                                      int width,
                                      int height) {
    rects_.clear();
    full_surface_ = false;
    rects_.reserve(rects.size());
    for (auto rect : rects) {
        rect.clamp(width, height);
        if (!rect.empty()) {
            rects_.push_back(rect);
        }
    }
    merge_overlaps();
}

void DamageRegion::merge_overlaps() {
    for (std::size_t i = 0; i < rects_.size(); ++i) {
        auto& base = rects_[i];
        std::size_t j = i + 1;
        while (j < rects_.size()) {
            if (base.overlaps_or_touches(rects_[j])) {
                base.merge(rects_[j]);
                rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(j));
            } else {
                ++j;
            }
        }
    }
}

namespace {

auto copy_single_tile(std::size_t tile_index,
                      ProgressiveTileCopyContext const& ctx) -> std::uint64_t {
    auto dims = ctx.buffer.tile_dimensions(tile_index);
    if (dims.width <= 0 || dims.height <= 0) {
        return 0;
    }
    auto writer = ctx.surface.begin_progressive_tile(tile_index, TilePass::OpaqueInProgress);
    auto tile_pixels = writer.pixels();
    auto const row_pitch = static_cast<std::size_t>(dims.width) * 4u;
    auto const tile_rows = std::max(dims.height, 0);
    for (int row = 0; row < tile_rows; ++row) {
        auto const src_offset = (static_cast<std::size_t>(dims.y + row) * ctx.row_stride_bytes)
                                + static_cast<std::size_t>(dims.x) * 4u;
        auto const dst_offset = static_cast<std::size_t>(row) * tile_pixels.stride_bytes;
        std::memcpy(tile_pixels.data + dst_offset,
                    ctx.staging.data() + src_offset,
                    row_pitch);
    }
    writer.commit(TilePass::AlphaDone, ctx.revision);
    return row_pitch * static_cast<std::uint64_t>(std::max(tile_rows, 0));
}

} // namespace

auto copy_progressive_tiles(std::span<std::size_t const> tile_indices,
                            ProgressiveTileCopyContext const& ctx) -> ProgressiveTileCopyStats {
    ProgressiveTileCopyStats stats{};
    if (tile_indices.empty()) {
        return stats;
    }

    auto const hardware = std::max(1u, std::thread::hardware_concurrency());
    std::size_t worker_count = std::min<std::size_t>(tile_indices.size(),
                                                     static_cast<std::size_t>(hardware));
    constexpr std::size_t kMinTilesPerWorker = 16;
    if (worker_count <= 1 || (tile_indices.size() / worker_count) < kMinTilesPerWorker) {
        stats.workers_used = tile_indices.empty() ? 0 : 1;
        for (auto tile_index : tile_indices) {
            stats.bytes_copied += copy_single_tile(tile_index, ctx);
            ++stats.tiles_updated;
        }
        return stats;
    }

    std::atomic<std::size_t> next{0};
    std::atomic<std::uint64_t> copied_bytes{0};
    std::atomic<std::uint64_t> tiles_done{0};
    std::exception_ptr error;
    std::mutex error_mutex;

    auto worker = [&]() {
        try {
            while (true) {
                auto idx = next.fetch_add(1, std::memory_order_relaxed);
                if (idx >= tile_indices.size()) {
                    break;
                }
                auto tile_index = tile_indices[idx];
                copied_bytes.fetch_add(copy_single_tile(tile_index, ctx), std::memory_order_relaxed);
                tiles_done.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            std::lock_guard<std::mutex> lock{error_mutex};
            if (!error) {
                error = std::current_exception();
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    if (error) {
        std::rethrow_exception(error);
    }

    stats.workers_used = worker_count;
    stats.bytes_copied = copied_bytes.load(std::memory_order_relaxed);
    stats.tiles_updated = tiles_done.load(std::memory_order_relaxed);
    return stats;
}

auto choose_progressive_tile_size(int width,
                                  int height,
                                  DamageRegion const& damage,
                                  bool full_repaint,
                                  PathSurfaceSoftware const& surface) -> int {
    if (!surface.has_progressive()) {
        return surface.progressive_tile_size();
    }
    auto clamp_dimension = [](int value) {
        return std::max(value, 1);
    };
    width = clamp_dimension(width);
    height = clamp_dimension(height);

    auto tiles_for = [&](int candidate) -> std::uint64_t {
        auto tiles_x = static_cast<std::uint64_t>((width + candidate - 1) / candidate);
        auto tiles_y = static_cast<std::uint64_t>((height + candidate - 1) / candidate);
        return tiles_x * tiles_y;
    };

    int base_size = std::max(64, surface.progressive_tile_size());
    double coverage = damage.coverage_ratio(width, height);
    constexpr std::uint64_t kMaxTiles = 4096;

    int tile_size = base_size;

    auto clamp_to_step = [](int value) {
        constexpr int kStep = 32;
        if (value % kStep == 0) {
            return value;
        }
        return ((value / kStep) + 1) * kStep;
    };

    auto adapt_for_localized_damage = [&]() {
        if (full_repaint || coverage >= 0.05) {
            return;
        }
        auto rects = damage.rectangles();
        if (rects.empty()) {
            return;
        }
        DamageRect bounds = rects.front();
        for (std::size_t i = 1; i < rects.size(); ++i) {
            bounds.merge(rects[i]);
        }
        auto span_w = std::max(1, bounds.max_x - bounds.min_x);
        auto span_h = std::max(1, bounds.max_y - bounds.min_y);
        auto longest = std::max(span_w, span_h);
        auto shortest = std::min(span_w, span_h);

        auto shrink_to = [&](int desired) {
            desired = std::max(32, desired);
            tile_size = std::min(tile_size, clamp_to_step(desired));
        };

        if (longest <= 192 && shortest <= 128) {
            shrink_to(tile_size / 2);
        }
        if (longest <= 128 && shortest <= 96) {
            shrink_to(48);
        }
        if (longest <= 96 && shortest <= 64) {
            shrink_to(32);
        }
    };

    auto reduce_if_small_damage = [&]() {
        if (coverage <= 0.0 || coverage >= 0.10 || full_repaint) {
            return;
        }
        tile_size = std::min(tile_size, 64);
    };

    auto widen_for_large_surfaces = [&]() {
        if (!(full_repaint || coverage > 0.5)) {
            return;
        }
        auto tiles = tiles_for(tile_size);
        while (tiles > kMaxTiles && tile_size < 256) {
            tile_size = clamp_to_step(tile_size + 32);
            tiles = tiles_for(tile_size);
        }
    };

    auto widen_for_extreme_dimensions = [&]() {
        auto longest = std::max(width, height);
        if (longest >= 6144 && tile_size < 128) {
            tile_size = clamp_to_step(128);
        }
    };

    auto ensure_minimum_concurrency = [&]() {
        auto tiles = tiles_for(tile_size);
        if (!(full_repaint || coverage > 0.5)) {
            return;
        }
        auto hardware = std::max(1u, std::thread::hardware_concurrency());
        std::uint64_t min_tiles_target = std::max<std::uint64_t>(hardware * 8u, 96u);
        while (tiles < min_tiles_target && tile_size > 64) {
            tile_size = std::max(64, tile_size - 32);
            tiles = tiles_for(tile_size);
        }
    };

    widen_for_extreme_dimensions();
    widen_for_large_surfaces();
    ensure_minimum_concurrency();
    adapt_for_localized_damage();
    reduce_if_small_damage();

    return tile_size;
}

} // namespace SP::UI::PathRenderer2DInternal

