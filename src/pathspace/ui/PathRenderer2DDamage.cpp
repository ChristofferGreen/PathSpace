#include "PathRenderer2DInternal.hpp"
#include "PathRenderer2DDetail.hpp"

#include <pathspace/ui/PathSurfaceSoftware.hpp>
#include <pathspace/ui/ProgressiveSurfaceBuffer.hpp>

#include <parallel_hashmap/phmap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <exception>
#include <optional>
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

namespace {

auto tile_rect_from_index(std::uint32_t index,
                          int width,
                          int height,
                          int tile_size_px) -> DamageRect {
    if (tile_size_px <= 0) {
        return {};
    }
    int const tiles_x = (width + tile_size_px - 1) / tile_size_px;
    if (tiles_x <= 0) {
        return {};
    }
    int const ty = static_cast<int>(index / static_cast<std::uint32_t>(tiles_x));
    int const tx = static_cast<int>(index % static_cast<std::uint32_t>(tiles_x));
    int const min_x = std::clamp(tx * tile_size_px, 0, width);
    int const min_y = std::clamp(ty * tile_size_px, 0, height);
    int const max_x = std::clamp(min_x + tile_size_px, 0, width);
    int const max_y = std::clamp(min_y + tile_size_px, 0, height);
    return DamageRect{min_x, min_y, max_x, max_y};
}

template <typename Fn>
void enumerate_tile_indices(DamageRect const& rect,
                            int width,
                            int height,
                            int tile_size_px,
                            Fn&& fn) {
    if (tile_size_px <= 0 || rect.empty()) {
        return;
    }
    int tiles_x = (width + tile_size_px - 1) / tile_size_px;
    int tiles_y = (height + tile_size_px - 1) / tile_size_px;
    if (tiles_x <= 0 || tiles_y <= 0) {
        return;
    }

    int min_x = std::clamp(rect.min_x, 0, width);
    int min_y = std::clamp(rect.min_y, 0, height);
    int max_x = std::clamp(rect.max_x, 0, width);
    int max_y = std::clamp(rect.max_y, 0, height);
    if (min_x >= max_x || min_y >= max_y) {
        return;
    }

    int start_tx = min_x / tile_size_px;
    int start_ty = min_y / tile_size_px;
    int end_tx = (std::max(max_x - 1, min_x) / tile_size_px);
    int end_ty = (std::max(max_y - 1, min_y) / tile_size_px);

    start_tx = std::clamp(start_tx, 0, tiles_x - 1);
    start_ty = std::clamp(start_ty, 0, tiles_y - 1);
    end_tx = std::clamp(end_tx, 0, tiles_x - 1);
    end_ty = std::clamp(end_ty, 0, tiles_y - 1);

    for (int ty = start_ty; ty <= end_ty; ++ty) {
        for (int tx = start_tx; tx <= end_tx; ++tx) {
            auto index = static_cast<std::uint32_t>(ty) * static_cast<std::uint32_t>(tiles_x)
                         + static_cast<std::uint32_t>(tx);
            fn(index);
        }
    }
}

auto make_hint_rect(SP::UI::Runtime::DirtyRectHint const& hint,
                    int width,
                    int height) -> DamageRect {
    DamageRect rect{};
    rect.min_x = static_cast<int>(std::floor(hint.min_x));
    rect.min_y = static_cast<int>(std::floor(hint.min_y));
    rect.max_x = static_cast<int>(std::ceil(hint.max_x));
    rect.max_y = static_cast<int>(std::ceil(hint.max_y));
    rect.clamp(width, height);
    return rect;
}

auto bounds_equal(PathRenderer2D::DrawableBounds const& lhs,
                  PathRenderer2D::DrawableBounds const& rhs) -> bool {
    return lhs.min_x == rhs.min_x && lhs.min_y == rhs.min_y && lhs.max_x == rhs.max_x
           && lhs.max_y == rhs.max_y;
}

} // namespace

auto compute_damage(DamageComputationOptions const& options,
                    PathRenderer2D::DrawableStateMap const& previous_states,
                    PathRenderer2D::DrawableStateMap const& current_states,
                    std::span<SP::UI::Runtime::DirtyRectHint const> dirty_rect_hints)
    -> DamageComputationResult {
    DamageComputationResult result{};
    int const width = std::max(options.width, 0);
    int const height = std::max(options.height, 0);
    int const tile_size_px = std::max(options.tile_size_px, 1);

    auto collect_hint_rectangles = [&]() {
        std::vector<std::uint32_t> hint_tile_indices;
        hint_tile_indices.reserve(dirty_rect_hints.size());
        for (auto const& hint : dirty_rect_hints) {
            auto rect = make_hint_rect(hint, width, height);
            if (rect.empty()) {
                continue;
            }
            enumerate_tile_indices(rect, width, height, tile_size_px, [&](std::uint32_t index) {
                hint_tile_indices.push_back(index);
            });
        }
        if (hint_tile_indices.empty()) {
            return;
        }
        std::sort(hint_tile_indices.begin(), hint_tile_indices.end());
        hint_tile_indices.erase(std::unique(hint_tile_indices.begin(), hint_tile_indices.end()),
                                hint_tile_indices.end());
        result.hint_rectangles.reserve(hint_tile_indices.size());
        for (auto index : hint_tile_indices) {
            auto rect = tile_rect_from_index(index, width, height, tile_size_px);
            if (!rect.empty()) {
                result.hint_rectangles.push_back(rect);
            }
        }
    };

    collect_hint_rectangles();

    DamageRegion damage;
    auto& stats = result.statistics;

    result.full_repaint = options.force_full_repaint || options.missing_bounds;
    if (result.full_repaint) {
        damage.set_full(width, height);
        if (options.collect_damage_metrics) {
            stats.fingerprint_removed = static_cast<std::uint64_t>(previous_states.size());
            if (previous_states.empty()) {
                stats.fingerprint_new = static_cast<std::uint64_t>(current_states.size());
            } else {
                stats.fingerprint_changed = static_cast<std::uint64_t>(current_states.size());
            }
        }
    } else {
        phmap::flat_hash_map<std::uint64_t, std::vector<std::uint64_t>> previous_by_fingerprint;
        previous_by_fingerprint.reserve(previous_states.size());
        for (auto const& [prev_id, prev_state] : previous_states) {
            previous_by_fingerprint[prev_state.fingerprint].push_back(prev_id);
        }

        phmap::flat_hash_set<std::uint64_t> consumed_previous_ids;
        consumed_previous_ids.reserve(previous_states.size());

        auto add_bounds = [&](PathRenderer2D::DrawableBounds const& bounds) {
            if (!bounds.empty()) {
                damage.add(bounds, width, height, 1);
            }
        };

        for (auto const& [id, current_state] : current_states) {
            auto prev_it = previous_states.find(id);
            if (prev_it != previous_states.end()) {
                consumed_previous_ids.insert(id);
                auto const& prev_state = prev_it->second;
                bool fingerprint_changed_now = current_state.fingerprint != prev_state.fingerprint;
                bool bounds_changed = !bounds_equal(current_state.bounds, prev_state.bounds);
                if (fingerprint_changed_now || bounds_changed) {
                    add_bounds(current_state.bounds);
                    add_bounds(prev_state.bounds);
                    if (options.collect_damage_metrics) {
                        ++stats.fingerprint_changed;
                    }
                } else if (options.collect_damage_metrics) {
                    ++stats.fingerprint_matches_exact;
                }
                continue;
            }

            PathRenderer2D::DrawableState const* matched_prev = nullptr;
            if (current_state.fingerprint != 0) {
                auto map_it = previous_by_fingerprint.find(current_state.fingerprint);
                if (map_it != previous_by_fingerprint.end()) {
                    auto& candidates = map_it->second;
                    std::optional<std::size_t> best_index;
                    for (std::size_t idx = 0; idx < candidates.size(); ++idx) {
                        auto candidate_id = candidates[idx];
                        if (consumed_previous_ids.contains(candidate_id)) {
                            continue;
                        }
                        if (current_states.find(candidate_id) != current_states.end()) {
                            continue;
                        }
                        auto prev_found = previous_states.find(candidate_id);
                        if (prev_found == previous_states.end()) {
                            continue;
                        }
                        if (!matched_prev) {
                            matched_prev = &prev_found->second;
                            best_index = idx;
                        }
                        if (bounds_equal(current_state.bounds, prev_found->second.bounds)) {
                            matched_prev = &prev_found->second;
                            best_index = idx;
                            break;
                        }
                    }
                    if (best_index.has_value() && matched_prev) {
                        consumed_previous_ids.insert(candidates[*best_index]);
                        candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(*best_index));
                    } else {
                        matched_prev = nullptr;
                    }
                }
            }

            if (matched_prev) {
                bool fingerprint_changed_now = current_state.fingerprint != matched_prev->fingerprint;
                bool bounds_changed = !bounds_equal(current_state.bounds, matched_prev->bounds);
                if (fingerprint_changed_now || bounds_changed) {
                    add_bounds(current_state.bounds);
                    add_bounds(matched_prev->bounds);
                    if (options.collect_damage_metrics) {
                        ++stats.fingerprint_changed;
                    }
                } else if (options.collect_damage_metrics) {
                    ++stats.fingerprint_matches_remap;
                }
            } else {
                add_bounds(current_state.bounds);
                if (options.collect_damage_metrics) {
                    ++stats.fingerprint_new;
                }
            }
        }

        for (auto const& [prev_id, prev_state] : previous_states) {
            if (!consumed_previous_ids.contains(prev_id)) {
                add_bounds(prev_state.bounds);
                if (options.collect_damage_metrics) {
                    ++stats.fingerprint_removed;
                }
            }
        }
    }

    damage.finalize(width, height);

    if (!result.hint_rectangles.empty()) {
        auto hint_span = std::span<DamageRect const>(result.hint_rectangles);
        if (damage.empty()) {
            damage.replace_with_rects(hint_span, width, height);
        } else {
            damage.restrict_to(hint_span);
        }
    }

    auto& damage_tile_hints = result.damage_tiles;
    damage_tile_hints.clear();
    if (!damage.empty()) {
        std::vector<std::uint32_t> damage_tile_indices;
        std::vector<DamageRect> damage_tile_rects;
        if (tile_size_px > 1) {
            for (auto const& rect : damage.rectangles()) {
                enumerate_tile_indices(rect, width, height, tile_size_px, [&](std::uint32_t index) {
                    damage_tile_indices.push_back(index);
                });
            }
        }

        if (!damage_tile_indices.empty()) {
            std::sort(damage_tile_indices.begin(), damage_tile_indices.end());
            damage_tile_indices.erase(std::unique(damage_tile_indices.begin(), damage_tile_indices.end()),
                                      damage_tile_indices.end());
            damage_tile_rects.reserve(damage_tile_indices.size());
            damage_tile_hints.reserve(damage_tile_indices.size());
            for (auto index : damage_tile_indices) {
                auto rect = tile_rect_from_index(index, width, height, tile_size_px);
                if (rect.empty()) {
                    continue;
                }
                damage_tile_rects.push_back(rect);
                SP::UI::Runtime::DirtyRectHint hint{};
                hint.min_x = static_cast<float>(rect.min_x);
                hint.min_y = static_cast<float>(rect.min_y);
                hint.max_x = static_cast<float>(rect.max_x);
                hint.max_y = static_cast<float>(rect.max_y);
                damage_tile_hints.push_back(hint);
            }
            if (!damage_tile_rects.empty()) {
                damage.replace_with_rects(damage_tile_rects, width, height);
            } else {
                damage_tile_hints.clear();
            }
        }

        if (damage_tile_hints.empty()) {
            auto rects = damage.rectangles();
            damage_tile_hints.reserve(rects.size());
            for (auto const& rect : rects) {
                SP::UI::Runtime::DirtyRectHint hint{};
                hint.min_x = static_cast<float>(rect.min_x);
                hint.min_y = static_cast<float>(rect.min_y);
                hint.max_x = static_cast<float>(rect.max_x);
                hint.max_y = static_cast<float>(rect.max_y);
                damage_tile_hints.push_back(hint);
            }
        }
    }

    if (options.collect_damage_metrics) {
        stats.damage_rect_count = static_cast<std::uint64_t>(damage.rectangles().size());
        stats.damage_coverage_ratio = damage.coverage_ratio(width, height);
    }

    result.damage = std::move(damage);
    return result;
}

} // namespace SP::UI::PathRenderer2DInternal
