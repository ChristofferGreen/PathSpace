#include <pathspace/ui/PathRenderer2D.hpp>

#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/PipelineFlags.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include "DrawableUtils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <atomic>
#include <numeric>
#include <sstream>
#include <string>
#include <span>
#include <vector>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>
#include <exception>

namespace SP::UI {
namespace {

auto make_error(std::string message, SP::Error::Code code) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

auto damage_metrics_enabled() -> bool {
    if (auto* env = std::getenv("PATHSPACE_UI_DAMAGE_METRICS")) {
        if (std::strcmp(env, "0") == 0 || std::strcmp(env, "false") == 0 || std::strcmp(env, "off") == 0) {
            return false;
        }
        return true;
    }
    return false;
}

template <typename T>
auto drain_queue(PathSpace& space, std::string const& path) -> SP::Expected<void> {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }
    return {};
}

template <typename T>
auto replace_single(PathSpace& space,
                    std::string const& path,
                    T const& value) -> SP::Expected<void> {
    if (auto cleared = drain_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        return std::unexpected(result.errors.front());
    }
    return {};
}

auto format_revision(std::uint64_t revision) -> std::string {
    std::ostringstream oss;
    oss << std::setw(16) << std::setfill('0') << revision;
    return oss.str();
}

auto fingerprint_to_hex(std::uint64_t fingerprint) -> std::string {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << std::nouppercase << fingerprint;
    return oss.str();
}

auto clamp_unit(float value) -> float {
    return std::clamp(value, 0.0f, 1.0f);
}

auto to_byte(float value) -> std::uint8_t {
    auto clamped = clamp_unit(value);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

auto set_last_error(PathSpace& space,
                    SP::ConcretePathStringView targetPath,
                    std::string const& message,
                    std::uint64_t revision = 0,
                    Builders::Diagnostics::PathSpaceError::Severity severity = Builders::Diagnostics::PathSpaceError::Severity::Recoverable,
                    int code = 3000) -> SP::Expected<void> {
    if (message.empty()) {
        return Builders::Diagnostics::ClearTargetError(space, targetPath);
    }

    Builders::Diagnostics::PathSpaceError error{};
    error.code = code;
    error.severity = severity;
    error.message = message;
    error.path = std::string(targetPath.getPath());
    error.revision = revision;
    return Builders::Diagnostics::WriteTargetError(space, targetPath, error);
}

auto color_from_drawable(std::uint64_t drawableId) -> std::array<float, 4> {
    auto r = static_cast<float>(drawableId & 0xFFu) / 255.0f;
    auto g = static_cast<float>((drawableId >> 8) & 0xFFu) / 255.0f;
    auto b = static_cast<float>((drawableId >> 16) & 0xFFu) / 255.0f;
    if (r == 0.0f && g == 0.0f && b == 0.0f) {
        r = 0.9f;
        g = 0.9f;
        b = 0.9f;
    }
    return {r, g, b, 1.0f};
}

auto pipeline_flags_for(Scene::DrawableBucketSnapshot const& bucket,
                        std::size_t drawable_index) -> std::uint32_t {
    if (drawable_index < bucket.pipeline_flags.size()) {
        return bucket.pipeline_flags[drawable_index];
    }
    return 0;
}

constexpr double kPi = 3.14159265358979323846;
constexpr float kSortEpsilon = 1e-5f;

template <typename T>
auto vector_value(std::vector<T> const& values, std::uint32_t index, T fallback) -> T {
    if (index < values.size()) {
        return values[index];
    }
    return fallback;
}

auto has_valid_bounds_box(Scene::DrawableBucketSnapshot const& bucket,
                          std::uint32_t index) -> bool {
    if (index >= bucket.bounds_boxes.size()) {
        return false;
    }
    if (index < bucket.bounds_box_valid.size()) {
        return bucket.bounds_box_valid[index] != 0;
    }
    return true;
}

auto approximate_drawable_area(Scene::DrawableBucketSnapshot const& bucket,
                               std::uint32_t index) -> double {
    if (has_valid_bounds_box(bucket, index)) {
        auto const& box = bucket.bounds_boxes[index];
        auto width = std::max(0.0f, box.max[0] - box.min[0]);
        auto height = std::max(0.0f, box.max[1] - box.min[1]);
        return static_cast<double>(width) * static_cast<double>(height);
    }
    if (index < bucket.bounds_spheres.size()) {
        auto radius = bucket.bounds_spheres[index].radius;
        if (radius > 0.0f) {
            return static_cast<double>(radius) * static_cast<double>(radius) * kPi;
        }
    }
    return 0.0;
}

struct DamageRect {
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;

    static auto from_bounds(PathRenderer2D::DrawableBounds const& bounds) -> DamageRect {
        return DamageRect{
            .min_x = bounds.min_x,
            .min_y = bounds.min_y,
            .max_x = bounds.max_x,
            .max_y = bounds.max_y,
        };
    }

    auto clamp(int width, int height) -> void {
        min_x = std::clamp(min_x, 0, width);
        min_y = std::clamp(min_y, 0, height);
        max_x = std::clamp(max_x, 0, width);
        max_y = std::clamp(max_y, 0, height);
    }

    auto expand(int margin, int width, int height) -> void {
        min_x = std::clamp(min_x - margin, 0, width);
        min_y = std::clamp(min_y - margin, 0, height);
        max_x = std::clamp(max_x + margin, 0, width);
        max_y = std::clamp(max_y + margin, 0, height);
    }

    [[nodiscard]] auto empty() const -> bool {
        return min_x >= max_x || min_y >= max_y;
    }

    [[nodiscard]] auto width() const -> int {
        return empty() ? 0 : (max_x - min_x);
    }

    [[nodiscard]] auto height() const -> int {
        return empty() ? 0 : (max_y - min_y);
    }

    [[nodiscard]] auto area() const -> std::uint64_t {
        if (empty()) {
            return 0;
        }
        return static_cast<std::uint64_t>(width()) * static_cast<std::uint64_t>(height());
    }

    auto merge(DamageRect const& other) -> void {
        min_x = std::min(min_x, other.min_x);
        min_y = std::min(min_y, other.min_y);
        max_x = std::max(max_x, other.max_x);
        max_y = std::max(max_y, other.max_y);
    }

    [[nodiscard]] auto overlaps_or_touches(DamageRect const& other) const -> bool {
        return !(max_x <= other.min_x || other.max_x <= min_x
                 || max_y <= other.min_y || other.max_y <= min_y);
    }

    [[nodiscard]] auto intersects(PathRenderer2D::DrawableBounds const& bounds) const -> bool {
        if (bounds.empty() || empty()) {
            return false;
        }
        return !(bounds.max_x <= min_x || bounds.min_x >= max_x
                 || bounds.max_y <= min_y || bounds.min_y >= max_y);
    }

    [[nodiscard]] auto intersects(TileDimensions const& tile) const -> bool {
        if (empty() || tile.width <= 0 || tile.height <= 0) {
            return false;
        }
        auto tile_max_x = tile.x + tile.width;
        auto tile_max_y = tile.y + tile.height;
        return !(tile_max_x <= min_x || tile.x >= max_x
                 || tile_max_y <= min_y || tile.y >= max_y);
    }

    [[nodiscard]] auto intersect(DamageRect const& other) const -> DamageRect {
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
};

class DamageRegion {
public:
    void set_full(int width, int height) {
        full_surface_ = true;
        rects_.clear();
        rects_.push_back(DamageRect{
            .min_x = 0,
            .min_y = 0,
            .max_x = width,
            .max_y = height,
        });
    }

    void add(PathRenderer2D::DrawableBounds const& bounds,
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

    void add_rect(DamageRect rect, int width, int height) {
        if (full_surface_) {
            return;
        }
        rect.clamp(width, height);
        if (rect.empty()) {
            return;
        }
        rects_.push_back(rect);
    }

    void finalize(int width, int height) {
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

    [[nodiscard]] auto empty() const -> bool {
        return rects_.empty();
    }

    [[nodiscard]] auto intersects(PathRenderer2D::DrawableBounds const& bounds) const -> bool {
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

    [[nodiscard]] auto intersects(TileDimensions const& tile) const -> bool {
        for (auto const& rect : rects_) {
            if (rect.intersects(tile)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto rectangles() const -> std::span<DamageRect const> {
        return rects_;
    }

    [[nodiscard]] auto area() const -> std::uint64_t {
        std::uint64_t total = 0;
        for (auto const& rect : rects_) {
            total += rect.area();
        }
        return total;
    }

    [[nodiscard]] auto coverage_ratio(int width, int height) const -> double {
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

    void collect_progressive_tiles(ProgressiveSurfaceBuffer const& buffer,
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

    void restrict_to(std::span<DamageRect const> limits) {
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

    void replace_with_rects(std::span<DamageRect const> rects, int width, int height) {
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

private:
    void merge_overlaps() {
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

    bool full_surface_ = false;
    std::vector<DamageRect> rects_;
};

struct ProgressiveTileCopyStats {
    std::uint64_t tiles_updated = 0;
    std::uint64_t bytes_copied = 0;
};

struct ProgressiveTileCopyContext {
    PathSurfaceSoftware& surface;
    ProgressiveSurfaceBuffer& buffer;
    std::span<std::uint8_t const> staging;
    std::size_t row_stride_bytes = 0;
    std::uint64_t revision = 0;
};

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
                auto bytes = copy_single_tile(tile_index, ctx);
                copied_bytes.fetch_add(bytes, std::memory_order_relaxed);
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

    stats.tiles_updated = tiles_done.load(std::memory_order_relaxed);
    stats.bytes_copied = copied_bytes.load(std::memory_order_relaxed);
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

auto compute_drawable_bounds(Scene::DrawableBucketSnapshot const& bucket,
                             std::uint32_t index,
                             int width,
                             int height) -> std::optional<PathRenderer2D::DrawableBounds> {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    bool have_bounds = false;

    if (has_valid_bounds_box(bucket, index)) {
        auto const& box = bucket.bounds_boxes[index];
        min_x = box.min[0];
        min_y = box.min[1];
        max_x = box.max[0];
        max_y = box.max[1];
        have_bounds = true;
    } else if (index < bucket.bounds_spheres.size()) {
        auto const& sphere = bucket.bounds_spheres[index];
        auto radius = sphere.radius;
        min_x = sphere.center[0] - radius;
        max_x = sphere.center[0] + radius;
        min_y = sphere.center[1] - radius;
        max_y = sphere.center[1] + radius;
        have_bounds = true;
    }

    if (!have_bounds) {
        return std::nullopt;
    }

    auto to_min_x = std::clamp(static_cast<int>(std::floor(min_x)), 0, width);
    auto to_max_x = std::clamp(static_cast<int>(std::ceil(max_x)), 0, width);
    auto to_min_y = std::clamp(static_cast<int>(std::floor(min_y)), 0, height);
    auto to_max_y = std::clamp(static_cast<int>(std::ceil(max_y)), 0, height);

    PathRenderer2D::DrawableBounds bounds{
        .min_x = to_min_x,
        .min_y = to_min_y,
        .max_x = to_max_x,
        .max_y = to_max_y,
    };

    if (bounds.empty()) {
        return std::nullopt;
    }

    bounds.min_x = std::max(0, bounds.min_x - 1);
    bounds.min_y = std::max(0, bounds.min_y - 1);
    bounds.max_x = std::min(width, bounds.max_x + 1);
    bounds.max_y = std::min(height, bounds.max_y + 1);

    if (bounds.empty()) {
        return std::nullopt;
    }

    return bounds;
}

auto bounds_equal(PathRenderer2D::DrawableBounds const& lhs,
                  PathRenderer2D::DrawableBounds const& rhs) -> bool {
    return lhs.min_x == rhs.min_x
        && lhs.min_y == rhs.min_y
        && lhs.max_x == rhs.max_x
        && lhs.max_y == rhs.max_y;
}

struct LinearStraightColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

struct LinearPremulColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

auto srgb_to_linear(float value) -> float {
    value = clamp_unit(value);
    if (value <= 0.04045f) {
        return value / 12.92f;
    }
    return std::pow((value + 0.055f) / 1.055f, 2.4f);
}

auto linear_to_srgb(float value) -> float {
    value = std::max(0.0f, value);
    value = std::min(1.0f, value);
    if (value <= 0.0031308f) {
        return value * 12.92f;
    }
    return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

auto make_linear_straight(std::array<float, 4> const& rgba) -> LinearStraightColor {
    auto alpha = clamp_unit(rgba[3]);
    auto r = srgb_to_linear(rgba[0]);
    auto g = srgb_to_linear(rgba[1]);
    auto b = srgb_to_linear(rgba[2]);
    return LinearStraightColor{
        .r = clamp_unit(r),
        .g = clamp_unit(g),
        .b = clamp_unit(b),
        .a = alpha,
    };
}

auto premultiply(LinearStraightColor const& straight) -> LinearPremulColor {
    auto alpha = clamp_unit(straight.a);
    return LinearPremulColor{
        .r = clamp_unit(straight.r) * alpha,
        .g = clamp_unit(straight.g) * alpha,
        .b = clamp_unit(straight.b) * alpha,
        .a = alpha,
    };
}

auto make_linear_color(std::array<float, 4> const& rgba) -> LinearPremulColor {
    return premultiply(make_linear_straight(rgba));
}

auto needs_srgb_encode(Builders::SurfaceDesc const& desc) -> bool {
    switch (desc.pixel_format) {
    case Builders::PixelFormat::RGBA8Unorm_sRGB:
    case Builders::PixelFormat::BGRA8Unorm_sRGB:
        return true;
    default:
        break;
    }
    return desc.color_space == Builders::ColorSpace::sRGB;
}

void blend_pixel(float* dest, LinearPremulColor const& src) {
    auto const inv_alpha = 1.0f - src.a;
    dest[0] = clamp_unit(src.r + dest[0] * inv_alpha);
    dest[1] = clamp_unit(src.g + dest[1] * inv_alpha);
    dest[2] = clamp_unit(src.b + dest[2] * inv_alpha);
    dest[3] = clamp_unit(src.a + dest[3] * inv_alpha);
}

auto lerp(float a, float b, float t) -> float {
    return a + (b - a) * t;
}

auto multiply_straight(LinearStraightColor lhs, LinearStraightColor rhs) -> LinearStraightColor {
    return LinearStraightColor{
        .r = clamp_unit(lhs.r * rhs.r),
        .g = clamp_unit(lhs.g * rhs.g),
        .b = clamp_unit(lhs.b * rhs.b),
        .a = clamp_unit(lhs.a * rhs.a),
    };
}

auto draw_rect_area(float min_x,
                    float min_y,
                    float max_x,
                    float max_y,
                    LinearPremulColor const& color,
                    std::vector<float>& buffer,
                    int width,
                    int height) -> bool {
    auto const clipped_min_x = std::clamp(static_cast<int>(std::floor(min_x)), 0, width);
    auto const clipped_min_y = std::clamp(static_cast<int>(std::floor(min_y)), 0, height);
    auto const clipped_max_x = std::clamp(static_cast<int>(std::ceil(max_x)), 0, width);
    auto const clipped_max_y = std::clamp(static_cast<int>(std::ceil(max_y)), 0, height);

    if (clipped_min_x >= clipped_max_x || clipped_min_y >= clipped_max_y) {
        return false;
    }

    auto const row_stride = static_cast<std::size_t>(width) * 4u;
    for (int y = clipped_min_y; y < clipped_max_y; ++y) {
        auto base = static_cast<std::size_t>(y) * row_stride;
        for (int x = clipped_min_x; x < clipped_max_x; ++x) {
            auto* dest = buffer.data() + base + static_cast<std::size_t>(x) * 4u;
            blend_pixel(dest, color);
        }
    }
    return true;
}

auto draw_rect_command(Scene::RectCommand const& command,
                       std::vector<float>& buffer,
                       int width,
                       int height) -> bool {
    auto color = make_linear_color(command.color);
    return draw_rect_area(command.min_x,
                          command.min_y,
                          command.max_x,
                          command.max_y,
                          color,
                          buffer,
                          width,
                          height);
}

auto draw_rounded_rect_command(Scene::RoundedRectCommand const& command,
                               std::vector<float>& buffer,
                               int width,
                               int height) -> bool {
    auto color = make_linear_color(command.color);

    auto min_x = std::min(command.min_x, command.max_x);
    auto max_x = std::max(command.min_x, command.max_x);
    auto min_y = std::min(command.min_y, command.max_y);
    auto max_y = std::max(command.min_y, command.max_y);

    auto width_f = std::max(0.0f, max_x - min_x);
    auto height_f = std::max(0.0f, max_y - min_y);
    if (width_f <= 0.0f || height_f <= 0.0f) {
        return false;
    }

    auto clamp_positive = [](float value) -> float {
        return std::max(0.0f, value);
    };

    auto radius_tl = clamp_positive(command.radius_top_left);
    auto radius_tr = clamp_positive(command.radius_top_right);
    auto radius_br = clamp_positive(command.radius_bottom_right);
    auto radius_bl = clamp_positive(command.radius_bottom_left);

    auto adjust_pair = [](float& a, float& b, float limit) {
        if (limit <= 0.0f) {
            a = 0.0f;
            b = 0.0f;
            return;
        }
        auto sum = a + b;
        if (sum > limit && sum > 0.0f) {
            auto scale = limit / sum;
            a *= scale;
            b *= scale;
        }
    };

    adjust_pair(radius_tl, radius_tr, width_f);
    adjust_pair(radius_bl, radius_br, width_f);
    adjust_pair(radius_tl, radius_bl, height_f);
    adjust_pair(radius_tr, radius_br, height_f);

    auto min_x_i = std::clamp(static_cast<int>(std::floor(min_x)), 0, width);
    auto max_x_i = std::clamp(static_cast<int>(std::ceil(max_x)), 0, width);
    auto min_y_i = std::clamp(static_cast<int>(std::floor(min_y)), 0, height);
    auto max_y_i = std::clamp(static_cast<int>(std::ceil(max_y)), 0, height);

    if (min_x_i >= max_x_i || min_y_i >= max_y_i) {
        return false;
    }

    auto radius_squared = [](float radius) -> float {
        return radius * radius;
    };

    auto blend_if_inside = [&](int x, int y) -> bool {
        auto px = static_cast<float>(x) + 0.5f;
        auto py = static_cast<float>(y) + 0.5f;

        if (px < min_x || px > max_x || py < min_y || py > max_y) {
            return false;
        }

        bool inside = true;

        if (radius_tl > 0.0f && px < (min_x + radius_tl) && py < (min_y + radius_tl)) {
            auto dx = px - (min_x + radius_tl);
            auto dy = py - (min_y + radius_tl);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_tl);
        } else if (radius_tr > 0.0f && px > (max_x - radius_tr) && py < (min_y + radius_tr)) {
            auto dx = px - (max_x - radius_tr);
            auto dy = py - (min_y + radius_tr);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_tr);
        } else if (radius_br > 0.0f && px > (max_x - radius_br) && py > (max_y - radius_br)) {
            auto dx = px - (max_x - radius_br);
            auto dy = py - (max_y - radius_br);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_br);
        } else if (radius_bl > 0.0f && px < (min_x + radius_bl) && py > (max_y - radius_bl)) {
            auto dx = px - (min_x + radius_bl);
            auto dy = py - (max_y - radius_bl);
            inside = (dx * dx + dy * dy) <= radius_squared(radius_bl);
        }

        if (!inside) {
            return false;
        }

        auto row_stride = static_cast<std::size_t>(width) * 4u;
        auto base = static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 4u;
        auto* dest = buffer.data() + base;
        blend_pixel(dest, color);
        return true;
    };

    bool drawn = false;
    for (int y = min_y_i; y < max_y_i; ++y) {
        for (int x = min_x_i; x < max_x_i; ++x) {
            if (blend_if_inside(x, y)) {
                drawn = true;
            }
        }
    }
    return drawn;
}

auto sample_image_linear(ImageCache::ImageData const& image,
                         float u,
                         float v) -> LinearStraightColor {
    if (image.width == 0 || image.height == 0) {
        return LinearStraightColor{};
    }

    auto clamp01 = [](float value) -> float {
        return std::clamp(value, 0.0f, 1.0f);
    };

    u = clamp01(u);
    v = clamp01(v);

    auto max_x = static_cast<float>(image.width - 1);
    auto max_y = static_cast<float>(image.height - 1);

    auto x = u * max_x;
    auto y = v * max_y;

    auto x0 = static_cast<int>(std::floor(x));
    auto y0 = static_cast<int>(std::floor(y));
    auto x1 = std::min(x0 + 1, static_cast<int>(image.width - 1));
    auto y1 = std::min(y0 + 1, static_cast<int>(image.height - 1));

    auto tx = x - static_cast<float>(x0);
    auto ty = y - static_cast<float>(y0);

    auto fetch = [&](int ix, int iy) -> LinearStraightColor {
        auto index = (static_cast<std::size_t>(iy) * image.width + static_cast<std::size_t>(ix)) * 4u;
        return LinearStraightColor{
            .r = image.pixels[index + 0],
            .g = image.pixels[index + 1],
            .b = image.pixels[index + 2],
            .a = image.pixels[index + 3],
        };
    };

    auto c00 = fetch(x0, y0);
    auto c10 = fetch(x1, y0);
    auto c01 = fetch(x0, y1);
    auto c11 = fetch(x1, y1);

    auto interp_row = [&](LinearStraightColor const& a,
                          LinearStraightColor const& b) -> LinearStraightColor {
        return LinearStraightColor{
            .r = lerp(a.r, b.r, tx),
            .g = lerp(a.g, b.g, tx),
            .b = lerp(a.b, b.b, tx),
            .a = lerp(a.a, b.a, tx),
        };
    };

    auto top = interp_row(c00, c10);
    auto bottom = interp_row(c01, c11);

    return LinearStraightColor{
        .r = lerp(top.r, bottom.r, ty),
        .g = lerp(top.g, bottom.g, ty),
        .b = lerp(top.b, bottom.b, ty),
        .a = lerp(top.a, bottom.a, ty),
    };
}

auto draw_image_command(Scene::ImageCommand const& command,
                        ImageCache::ImageData const& image,
                        LinearStraightColor const& tint,
                        std::vector<float>& buffer,
                        int width,
                        int height) -> bool {
    auto min_x = std::min(command.min_x, command.max_x);
    auto max_x = std::max(command.min_x, command.max_x);
    auto min_y = std::min(command.min_y, command.max_y);
    auto max_y = std::max(command.min_y, command.max_y);

    auto width_f = std::max(0.0f, max_x - min_x);
    auto height_f = std::max(0.0f, max_y - min_y);
    if (width_f <= 0.0f || height_f <= 0.0f) {
        return false;
    }

    auto clamp_pixel = [](float value, int limit, bool ceiling) -> int {
        auto pixel = ceiling ? std::ceil(value) : std::floor(value);
        return std::clamp(static_cast<int>(pixel), 0, limit);
    };

    auto min_x_i = clamp_pixel(min_x, width, false);
    auto max_x_i = clamp_pixel(max_x, width, true);
    auto min_y_i = clamp_pixel(min_y, height, false);
    auto max_y_i = clamp_pixel(max_y, height, true);

    if (min_x_i >= max_x_i || min_y_i >= max_y_i) {
        return false;
    }

    auto uv_width = command.uv_max_x - command.uv_min_x;
    auto uv_height = command.uv_max_y - command.uv_min_y;
    if (uv_width == 0.0f || uv_height == 0.0f) {
        return false;
    }

    bool drawn = false;
    auto const row_stride = static_cast<std::size_t>(width) * 4u;

    for (int y = min_y_i; y < max_y_i; ++y) {
        auto py = static_cast<float>(y) + 0.5f;
        auto local_v = (py - min_y) / height_f;
        auto v = command.uv_min_y + uv_height * local_v;

        for (int x = min_x_i; x < max_x_i; ++x) {
            auto px = static_cast<float>(x) + 0.5f;
            auto local_u = (px - min_x) / width_f;
            auto u = command.uv_min_x + uv_width * local_u;

            auto sampled = sample_image_linear(image, u, v);
            auto tinted = multiply_straight(sampled, tint);
            auto premul = premultiply(tinted);
            if (premul.a <= 0.0f) {
                continue;
            }

            auto base = static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 4u;
            auto* dest = buffer.data() + base;
            blend_pixel(dest, premul);
            drawn = true;
        }
    }

    return drawn;
}

auto draw_text_glyphs_command(Scene::TextGlyphsCommand const& command,
                              std::vector<float>& buffer,
                              int width,
                              int height) -> bool {
    auto color = premultiply(make_linear_straight(command.color));
    auto min_x = std::min(command.min_x, command.max_x);
    auto max_x = std::max(command.min_x, command.max_x);
    auto min_y = std::min(command.min_y, command.max_y);
    auto max_y = std::max(command.min_y, command.max_y);
    return draw_rect_area(min_x, min_y, max_x, max_y, color, buffer, width, height);
}

auto draw_path_command(Scene::PathCommand const& command,
                       std::vector<float>& buffer,
                       int width,
                       int height) -> bool {
    auto color = premultiply(make_linear_straight(command.fill_color));
    auto min_x = std::min(command.min_x, command.max_x);
    auto max_x = std::max(command.min_x, command.max_x);
    auto min_y = std::min(command.min_y, command.max_y);
    auto max_y = std::max(command.min_y, command.max_y);
    return draw_rect_area(min_x, min_y, max_x, max_y, color, buffer, width, height);
}

auto draw_mesh_command(Scene::MeshCommand const& command,
                       Scene::DrawableBucketSnapshot const& bucket,
                       std::size_t drawable_index,
                       std::vector<float>& buffer,
                       int width,
                       int height) -> bool {
    if (drawable_index >= bucket.bounds_boxes.size()) {
        return false;
    }
    if (drawable_index < bucket.bounds_box_valid.size()
        && bucket.bounds_box_valid[drawable_index] == 0) {
        return false;
    }
    auto const& box = bucket.bounds_boxes[drawable_index];
    auto color = premultiply(make_linear_straight(command.color));
    return draw_rect_area(box.min[0], box.min[1], box.max[0], box.max[1], color, buffer, width, height);
}

auto draw_fallback_bounds_box(Scene::DrawableBucketSnapshot const& bucket,
                              std::size_t drawable_index,
                              std::vector<float>& buffer,
                              int width,
                              int height) -> bool {
    if (drawable_index >= bucket.bounds_boxes.size()) {
        return false;
    }
    if (drawable_index < bucket.bounds_box_valid.size()
        && bucket.bounds_box_valid[drawable_index] == 0) {
        return false;
    }
    auto const& box = bucket.bounds_boxes[drawable_index];
    auto color = make_linear_color(color_from_drawable(bucket.drawable_ids[drawable_index]));
    return draw_rect_area(box.min[0],
                          box.min[1],
                          box.max[0],
                          box.max[1],
                          color,
                          buffer,
                          width,
                          height);
}

template <typename T>
auto read_struct(std::vector<std::uint8_t> const& payload,
                 std::size_t offset) -> T {
    T value{};
    std::memcpy(&value, payload.data() + offset, sizeof(T));
    return value;
}

auto compute_command_payload_offsets(std::vector<std::uint32_t> const& kinds,
                                     std::vector<std::uint8_t> const& payload)
    -> SP::Expected<std::vector<std::size_t>> {
    std::vector<std::size_t> offsets;
    offsets.reserve(kinds.size());
    std::size_t cursor = 0;
    for (auto kind_value : kinds) {
        auto kind = static_cast<Scene::DrawCommandKind>(kind_value);
        auto payload_size = Scene::payload_size_bytes(kind);
        if (payload_size == 0) {
            offsets.push_back(cursor);
            continue;
        }
        if (cursor + payload_size > payload.size()) {
            return std::unexpected(make_error("command payload truncated",
                                              SP::Error::Code::InvalidType));
        }
        offsets.push_back(cursor);
        cursor += payload_size;
    }
    if (cursor != payload.size()) {
        return std::unexpected(make_error("command payload size mismatch",
                                          SP::Error::Code::InvalidType));
    }
    return offsets;
}

auto encode_pixel(float const* linear_premul,
                  Builders::SurfaceDesc const& desc,
                  bool encode_srgb) -> std::array<std::uint8_t, 4> {
    auto alpha = clamp_unit(linear_premul[3]);

    std::array<float, 3> premul_linear{
        clamp_unit(linear_premul[0]),
        clamp_unit(linear_premul[1]),
        clamp_unit(linear_premul[2]),
    };

    std::array<float, 3> straight_linear{0.0f, 0.0f, 0.0f};
    if (alpha > 0.0f) {
        for (int i = 0; i < 3; ++i) {
            straight_linear[i] = std::clamp(premul_linear[i] / alpha, 0.0f, 1.0f);
        }
    }

    std::array<float, 3> encoded{};
    if (encode_srgb) {
        for (int i = 0; i < 3; ++i) {
            auto value = linear_to_srgb(straight_linear[i]);
            if (desc.premultiplied_alpha) {
                value *= alpha;
            }
            encoded[i] = clamp_unit(value);
        }
    } else {
        for (int i = 0; i < 3; ++i) {
            auto value = desc.premultiplied_alpha ? premul_linear[i] : straight_linear[i];
            encoded[i] = clamp_unit(value);
        }
    }

    return {
        to_byte(encoded[0]),
        to_byte(encoded[1]),
        to_byte(encoded[2]),
        to_byte(alpha)
    };
}

} // namespace

PathRenderer2D::PathRenderer2D(PathSpace& space)
    : space_(space) {}

auto PathRenderer2D::target_cache() -> std::unordered_map<std::string, TargetState>& {
    static std::unordered_map<std::string, TargetState> cache;
    return cache;
}

auto PathRenderer2D::render(RenderParams params) -> SP::Expected<RenderStats> {
    auto const start = std::chrono::steady_clock::now();
    double damage_ms = 0.0;
    double encode_ms = 0.0;
    double progressive_copy_ms = 0.0;
    double publish_ms = 0.0;

    auto app_root = SP::App::derive_app_root(params.target_path);
    if (!app_root) {
        auto message = std::string{"unable to derive application root for target"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(app_root.error());
    }

    auto sceneField = std::string(params.target_path.getPath()) + "/scene";
    auto sceneRel = space_.read<std::string, std::string>(sceneField);
    if (!sceneRel) {
        auto message = std::string{"target missing scene binding"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, sceneRel.error().code));
    }
    if (sceneRel->empty()) {
        auto message = std::string{"target scene binding is empty"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidPath));
    }

    auto sceneAbsolute = SP::App::resolve_app_relative(SP::App::AppRootPathView{app_root->getPath()},
                                                       *sceneRel);
    if (!sceneAbsolute) {
        auto message = std::string{"failed to resolve scene path '"} + *sceneRel + "'";
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(sceneAbsolute.error());
    }

    auto sceneRevision = Builders::Scene::ReadCurrentRevision(space_, Builders::ScenePath{sceneAbsolute->getPath()});
    if (!sceneRevision) {
        auto message = std::string{"scene has no current revision"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(sceneRevision.error());
    }

    auto revisionBase = std::string(sceneAbsolute->getPath()) + "/builds/" + format_revision(sceneRevision->revision);
    auto bucket = Scene::SceneSnapshotBuilder::decode_bucket(space_, revisionBase);
    if (!bucket) {
        auto message = std::string{"failed to load snapshot bucket for revision "} + std::to_string(sceneRevision->revision);
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(bucket.error());
    }

    auto& surface = params.surface;
    auto const& desc = surface.desc();

    bool const has_buffered = surface.has_buffered();
    bool const has_progressive = surface.has_progressive();
    if (!has_buffered && !has_progressive) {
        auto message = std::string{"surface has neither buffered nor progressive storage"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidType));
    }

    if (params.settings.surface.size_px.width != 0
        && params.settings.surface.size_px.width != desc.size_px.width) {
        auto message = std::string{"render settings width does not match surface descriptor"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidPath));
    }
    if (params.settings.surface.size_px.height != 0
        && params.settings.surface.size_px.height != desc.size_px.height) {
        auto message = std::string{"render settings height does not match surface descriptor"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidPath));
    }

    switch (desc.pixel_format) {
    case Builders::PixelFormat::RGBA8Unorm:
    case Builders::PixelFormat::BGRA8Unorm:
    case Builders::PixelFormat::RGBA8Unorm_sRGB:
    case Builders::PixelFormat::BGRA8Unorm_sRGB:
        break;
    default: {
        auto message = std::string{"pixel format not supported by PathRenderer2D"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidType));
    }
    }

    auto const width = desc.size_px.width;
    auto const height = desc.size_px.height;
    if (width <= 0 || height <= 0) {
        auto message = std::string{"surface dimensions must be positive"};
        (void)set_last_error(space_, params.target_path, message);
        return std::unexpected(make_error(message, SP::Error::Code::InvalidType));
    }

    auto const pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    auto target_key = std::string(params.target_path.getPath());
    auto& state = target_cache()[target_key];
    auto& material_descriptors = state.material_descriptors;
    material_descriptors.clear();
    if (!bucket->material_ids.empty()) {
        material_descriptors.reserve(bucket->material_ids.size());
    }

    std::vector<Builders::DirtyRectHint> dirty_rect_hints;
    std::vector<DamageRect> hint_rects;
    {
        auto hints_path = target_key + "/hints/dirtyRects";
        auto hints = space_.take<std::vector<Builders::DirtyRectHint>>(hints_path);
        if (hints) {
            dirty_rect_hints = std::move(*hints);
            hint_rects.reserve(dirty_rect_hints.size());
        } else {
            auto const& error = hints.error();
            if (error.code != SP::Error::Code::NoObjectFound
                && error.code != SP::Error::Code::NoSuchPath) {
                return std::unexpected(error);
            }
        }
    }

    bool size_changed = state.desc.size_px.width != desc.size_px.width
                        || state.desc.size_px.height != desc.size_px.height;
    bool format_changed = state.desc.pixel_format != desc.pixel_format
                          || state.desc.color_space != desc.color_space
                          || state.desc.premultiplied_alpha != desc.premultiplied_alpha;

    if (state.linear_buffer.size() != pixel_count * 4u) {
        state.linear_buffer.clear();
        state.linear_buffer.resize(pixel_count * 4u);
        size_changed = true;
    }
    auto& linear_buffer = state.linear_buffer;

    auto current_clear = params.settings.clear_color;
    bool full_repaint = size_changed
                        || format_changed
                        || state.last_revision == 0
                        || state.clear_color != current_clear;

    auto const damage_phase_start = std::chrono::steady_clock::now();
    auto const drawable_count = bucket->drawable_ids.size();
    std::vector<std::optional<PathRenderer2D::DrawableBounds>> bounds_by_index(drawable_count);
    std::unordered_map<std::uint64_t, PathRenderer2D::DrawableState> current_states;
    current_states.reserve(drawable_count);

    auto const& drawable_fingerprints = bucket->drawable_fingerprints;

    bool missing_bounds = false;
    for (std::uint32_t i = 0; i < drawable_count; ++i) {
        auto maybe_bounds = compute_drawable_bounds(*bucket, i, width, height);
        if (maybe_bounds) {
            bounds_by_index[i] = maybe_bounds;
        } else {
            missing_bounds = true;
        }

        PathRenderer2D::DrawableState drawable_state{};
        if (maybe_bounds) {
            drawable_state.bounds = *maybe_bounds;
        }
        if (i < drawable_fingerprints.size()) {
            drawable_state.fingerprint = drawable_fingerprints[i];
        }
        current_states.emplace(bucket->drawable_ids[i], drawable_state);
    }
    if (missing_bounds) {
        full_repaint = true;
    }

    bool const collect_damage_metrics = damage_metrics_enabled();
    std::uint64_t fingerprint_matches_exact = 0;
    std::uint64_t fingerprint_matches_remap = 0;
    std::uint64_t fingerprint_changed = 0;
    std::uint64_t fingerprint_new = 0;
    std::uint64_t fingerprint_removed = 0;
    std::uint64_t damage_rect_count = 0;
    double damage_coverage_ratio = 0.0;
    std::uint64_t progressive_tiles_dirty = 0;
    std::uint64_t progressive_tiles_total = 0;
    std::uint64_t progressive_tiles_skipped = 0;

    DamageRegion damage;
    if (full_repaint) {
        damage.set_full(width, height);
        if (collect_damage_metrics) {
            fingerprint_removed = static_cast<std::uint64_t>(state.drawable_states.size());
            if (state.drawable_states.empty()) {
                fingerprint_new = static_cast<std::uint64_t>(current_states.size());
            } else {
                fingerprint_changed = static_cast<std::uint64_t>(current_states.size());
            }
        }
    } else {
        std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> previous_by_fingerprint;
        previous_by_fingerprint.reserve(state.drawable_states.size());
        for (auto const& [prev_id, prev_state] : state.drawable_states) {
            previous_by_fingerprint[prev_state.fingerprint].push_back(prev_id);
        }

        std::unordered_set<std::uint64_t> consumed_previous_ids;
        consumed_previous_ids.reserve(state.drawable_states.size());

        auto add_bounds = [&](PathRenderer2D::DrawableBounds const& bounds) {
            if (!bounds.empty()) {
                damage.add(bounds, width, height, 1);
            }
        };

        for (auto const& [id, current_state] : current_states) {
            auto prev_it = state.drawable_states.find(id);
            if (prev_it != state.drawable_states.end()) {
                consumed_previous_ids.insert(id);

                auto const& prev_state = prev_it->second;
                bool fingerprint_changed_now = current_state.fingerprint != prev_state.fingerprint;
                bool bounds_changed = !bounds_equal(current_state.bounds, prev_state.bounds);
                if (fingerprint_changed_now || bounds_changed) {
                    add_bounds(current_state.bounds);
                    add_bounds(prev_state.bounds);
                    if (collect_damage_metrics) {
                        ++fingerprint_changed;
                    }
                } else {
                    if (collect_damage_metrics) {
                        ++fingerprint_matches_exact;
                    }
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
                        auto prev_found = state.drawable_states.find(candidate_id);
                        if (prev_found == state.drawable_states.end()) {
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
                    if (collect_damage_metrics) {
                        ++fingerprint_changed;
                    }
                } else {
                    if (collect_damage_metrics) {
                        ++fingerprint_matches_remap;
                    }
                }
            } else {
                add_bounds(current_state.bounds);
                if (collect_damage_metrics) {
                    ++fingerprint_new;
                }
            }
        }

        for (auto const& [prev_id, prev_state] : state.drawable_states) {
            if (!consumed_previous_ids.contains(prev_id)) {
                add_bounds(prev_state.bounds);
                if (collect_damage_metrics) {
                    ++fingerprint_removed;
                }
            }
        }
    }
    auto append_hint_rect = [&](DamageRect const& rect) {
        if (rect.empty()) {
            return;
        }
        if (!surface.has_progressive()) {
            hint_rects.push_back(rect);
            return;
        }

        auto const tile_size = surface.progressive_buffer().tile_size();
        if (tile_size <= 1) {
            hint_rects.push_back(rect);
            return;
        }

        auto const clamp_max_x = std::min(rect.max_x, width);
        auto const clamp_max_y = std::min(rect.max_y, height);
        auto const min_x = std::max(rect.min_x, 0);
        auto const min_y = std::max(rect.min_y, 0);

        auto const start_tx = min_x / tile_size;
        auto const start_ty = min_y / tile_size;
        auto const end_tx = (std::max(clamp_max_x - 1, min_x) / tile_size);
        auto const end_ty = (std::max(clamp_max_y - 1, min_y) / tile_size);

        for (int ty = start_ty; ty <= end_ty; ++ty) {
            auto const tile_min_y = ty * tile_size;
            auto const tile_max_y = std::min(tile_min_y + tile_size, height);
            for (int tx = start_tx; tx <= end_tx; ++tx) {
                auto const tile_min_x = tx * tile_size;
                auto const tile_max_x = std::min(tile_min_x + tile_size, width);

                DamageRect tile_rect{
                    .min_x = tile_min_x,
                    .min_y = tile_min_y,
                    .max_x = tile_max_x,
                    .max_y = tile_max_y,
                };

                tile_rect.clamp(width, height);

                if (!tile_rect.empty()) {
                    hint_rects.push_back(tile_rect);
                }
            }
        }
    };

    for (auto const& hint : dirty_rect_hints) {
        DamageRect rect{};
        rect.min_x = static_cast<int>(std::floor(hint.min_x));
        rect.min_y = static_cast<int>(std::floor(hint.min_y));
        rect.max_x = static_cast<int>(std::ceil(hint.max_x));
        rect.max_y = static_cast<int>(std::ceil(hint.max_y));
        rect.clamp(width, height);
        append_hint_rect(rect);
    }

    if (!hint_rects.empty()) {
        if (auto* trace = std::getenv("PATHSPACE_TRACE_DAMAGE")) {
            (void)trace;
            std::cout << "hint rectangles (" << hint_rects.size() << ")" << std::endl;
            for (auto const& rect : hint_rects) {
                std::cout << "  hint=" << rect.min_x << ',' << rect.min_y
                          << " -> " << rect.max_x << ',' << rect.max_y << std::endl;
            }
        }
    }

    damage.finalize(width, height);
    if (collect_damage_metrics) {
        damage_rect_count = static_cast<std::uint64_t>(damage.rectangles().size());
        damage_coverage_ratio = damage.coverage_ratio(width, height);
    }
    if (!hint_rects.empty()) {
        if (damage.empty()) {
            damage.replace_with_rects(hint_rects, width, height);
        } else {
            damage.restrict_to(hint_rects);
        }
    }
    bool const has_damage = !damage.empty();
    auto const damage_phase_end = std::chrono::steady_clock::now();
    damage_ms = std::chrono::duration<double, std::milli>(damage_phase_end - damage_phase_start).count();

    if (has_damage) {
        if (auto* trace = std::getenv("PATHSPACE_TRACE_DAMAGE")) {
            (void)trace;
            auto rects = damage.rectangles();
            std::cout << "damage rectangles (" << rects.size() << ")" << std::endl;
            for (auto const& rect : rects) {
                std::cout << "  rect=" << rect.min_x << ',' << rect.min_y
                          << " -> " << rect.max_x << ',' << rect.max_y << std::endl;
            }
        }
    }

#if defined(DEBUG_TILE_TRACE)
    if (has_damage) {
        auto rects = damage.rectangles();
        for (auto const& rect : rects) {
            std::cout << "damage rect: " << rect.min_x << ',' << rect.min_y
                      << ' ' << rect.max_x << ',' << rect.max_y << std::endl;
        }
    }
#endif

    std::vector<std::uint8_t> local_frame_bytes;
    std::span<std::uint8_t> staging;
    if (has_damage) {
        if (has_buffered) {
            staging = surface.staging_span();
            if (staging.size() < surface.frame_bytes()) {
                auto message = std::string{"surface staging buffer smaller than expected"};
                (void)set_last_error(space_, params.target_path, message);
                return std::unexpected(make_error(message, SP::Error::Code::UnknownError));
            }
        } else {
            local_frame_bytes.resize(surface.frame_bytes());
            staging = std::span<std::uint8_t>{local_frame_bytes.data(), local_frame_bytes.size()};
        }
    }

    auto clear_linear = make_linear_color(current_clear);
    if (has_damage) {
        auto const row_stride = static_cast<std::size_t>(width) * 4u;
        for (auto const& rect : damage.rectangles()) {
            for (int y = rect.min_y; y < rect.max_y; ++y) {
                auto base = static_cast<std::size_t>(y) * row_stride;
                for (int x = rect.min_x; x < rect.max_x; ++x) {
                    auto* dest = linear_buffer.data() + base + static_cast<std::size_t>(x) * 4u;
                    dest[0] = clear_linear.r;
                    dest[1] = clear_linear.g;
                    dest[2] = clear_linear.b;
                    dest[3] = clear_linear.a;
                }
            }
        }
    }

    ProgressiveSurfaceBuffer* progressive_buffer = nullptr;
    std::vector<std::size_t> progressive_dirty_tiles;
    if (has_progressive) {
        if (has_damage) {
            auto desired_tile = choose_progressive_tile_size(width, height, damage, full_repaint, surface);
            surface.ensure_progressive_tile_size(desired_tile);
        }
        progressive_buffer = &surface.progressive_buffer();
        if (has_damage) {
            damage.collect_progressive_tiles(*progressive_buffer, progressive_dirty_tiles);
        }
    }
    if (progressive_buffer && collect_damage_metrics) {
        progressive_tiles_total = static_cast<std::uint64_t>(progressive_buffer->tile_count());
        progressive_tiles_dirty = static_cast<std::uint64_t>(progressive_dirty_tiles.size());
        if (progressive_tiles_total > progressive_tiles_dirty) {
            progressive_tiles_skipped = progressive_tiles_total - progressive_tiles_dirty;
        } else {
            progressive_tiles_skipped = 0;
        }
    }

    auto const stride = static_cast<std::size_t>(surface.row_stride_bytes());
    bool const is_bgra = (desc.pixel_format == Builders::PixelFormat::BGRA8Unorm
                          || desc.pixel_format == Builders::PixelFormat::BGRA8Unorm_sRGB);

    auto payload_offsets = compute_command_payload_offsets(bucket->command_kinds,
                                                           bucket->command_payload);
    if (!payload_offsets) {
        (void)set_last_error(space_, params.target_path,
                             payload_offsets.error().message.value_or("failed to prepare command payload"));
        return std::unexpected(payload_offsets.error());
    }

    std::uint64_t drawn_total = 0;
    std::uint64_t drawn_opaque = 0;
    std::uint64_t drawn_alpha = 0;
    std::uint64_t culled_drawables = 0;
    std::uint64_t executed_commands = 0;
    std::uint64_t unsupported_commands = 0;
    std::uint64_t opaque_sort_violations = 0;
    std::uint64_t alpha_sort_violations = 0;
    double approx_area_total = 0.0;
    double approx_area_opaque = 0.0;
    double approx_area_alpha = 0.0;
    std::uint64_t progressive_tiles_updated = 0;
    std::uint64_t progressive_bytes_copied = 0;

    auto image_asset_prefix = revisionBase + "/assets/images/";

    auto process_drawable = [&](std::uint32_t drawable_index,
                                bool alpha_pass) -> SP::Expected<void> {
        if (drawable_index >= drawable_count) {
            return std::unexpected(make_error("drawable index out of range",
                                              SP::Error::Code::InvalidType));
        }

        if (drawable_index < bucket->visibility.size()
            && bucket->visibility[drawable_index] == 0) {
            ++culled_drawables;
            return {};
        }

        if (!detail::bounding_box_intersects(*bucket, drawable_index, width, height)
            || !detail::bounding_sphere_intersects(*bucket, drawable_index, width, height)) {
            ++culled_drawables;
            return {};
        }

        if (drawable_index >= bucket->command_offsets.size()
            || drawable_index >= bucket->command_counts.size()) {
            return std::unexpected(make_error("command buffer metadata missing",
                                              SP::Error::Code::InvalidType));
        }

        auto command_offset = bucket->command_offsets[drawable_index];
        auto command_count = bucket->command_counts[drawable_index];
        if (static_cast<std::size_t>(command_offset) + command_count
            > bucket->command_kinds.size()) {
            return std::unexpected(make_error("command buffer index out of range",
                                              SP::Error::Code::InvalidType));
        }

        auto material_id = std::uint32_t{0};
        if (drawable_index < bucket->material_ids.size()) {
            material_id = bucket->material_ids[drawable_index];
        }
        auto pipeline_flags = pipeline_flags_for(*bucket, drawable_index);
        MaterialDescriptor* material_desc = nullptr;
        {
            auto [it, inserted] = material_descriptors.try_emplace(material_id, MaterialDescriptor{});
            if (inserted) {
                it->second.material_id = material_id;
            }
            it->second.pipeline_flags |= pipeline_flags;
            material_desc = &it->second;
        }

        bool drawable_drawn = false;
        bool fallback_attempted = false;

        bool skip_draw = false;
        if (has_damage) {
            auto const& bounds_opt = bounds_by_index[drawable_index];
            if (bounds_opt && !damage.intersects(*bounds_opt)) {
                skip_draw = true;
            }
        }

        auto record_material_kind = [&](Scene::DrawCommandKind kind) {
            if (!material_desc) {
                return;
            }
            if (material_desc->command_count == 0) {
                material_desc->primary_draw_kind = static_cast<std::uint32_t>(kind);
            }
            material_desc->command_count += 1;
        };

        if (!skip_draw) {
            if (command_count == 0) {
                drawable_drawn = draw_fallback_bounds_box(*bucket,
                                                          drawable_index,
                                                          linear_buffer,
                                                          width,
                                                          height);
                fallback_attempted = true;
                if (!drawable_drawn) {
                    ++culled_drawables;
                    return {};
                }
            } else {
                for (std::uint32_t cmd = 0; cmd < command_count; ++cmd) {
                    auto command_index = static_cast<std::size_t>(command_offset) + cmd;
                    auto kind = static_cast<Scene::DrawCommandKind>(bucket->command_kinds[command_index]);
                    auto payload_offset = (*payload_offsets)[command_index];
                    auto payload_size = Scene::payload_size_bytes(kind);
                    if (payload_offset + payload_size > bucket->command_payload.size()) {
                        return std::unexpected(make_error("command payload exceeds buffer",
                                                          SP::Error::Code::InvalidType));
                    }

                    switch (kind) {
                    case Scene::DrawCommandKind::Rect: {
                        auto rect = read_struct<Scene::RectCommand>(bucket->command_payload,
                                                                    payload_offset);
                        record_material_kind(Scene::DrawCommandKind::Rect);
                        if (material_desc) {
                            material_desc->color_rgba = rect.color;
                            material_desc->uses_image = false;
                        }
                        if (draw_rect_command(rect, linear_buffer, width, height)) {
                            drawable_drawn = true;
                        }
                        ++executed_commands;
                        break;
                    }
                    case Scene::DrawCommandKind::RoundedRect: {
                        auto rounded = read_struct<Scene::RoundedRectCommand>(bucket->command_payload,
                                                                              payload_offset);
                        record_material_kind(Scene::DrawCommandKind::RoundedRect);
                        if (material_desc) {
                            material_desc->color_rgba = rounded.color;
                            material_desc->uses_image = false;
                        }
                        if (draw_rounded_rect_command(rounded, linear_buffer, width, height)) {
                            drawable_drawn = true;
                        }
                        ++executed_commands;
                        break;
                    }
                    case Scene::DrawCommandKind::TextGlyphs: {
                        auto glyphs = read_struct<Scene::TextGlyphsCommand>(bucket->command_payload,
                                                                            payload_offset);
                        record_material_kind(Scene::DrawCommandKind::TextGlyphs);
                        if (material_desc) {
                            material_desc->color_rgba = glyphs.color;
                            material_desc->uses_image = false;
                        }
                        if (draw_text_glyphs_command(glyphs, linear_buffer, width, height)) {
                            drawable_drawn = true;
                        }
                        ++executed_commands;
                        break;
                    }
                    case Scene::DrawCommandKind::Path: {
                        auto path_cmd = read_struct<Scene::PathCommand>(bucket->command_payload,
                                                                        payload_offset);
                        record_material_kind(Scene::DrawCommandKind::Path);
                        if (material_desc) {
                            material_desc->color_rgba = path_cmd.fill_color;
                            material_desc->uses_image = false;
                        }
                        if (draw_path_command(path_cmd, linear_buffer, width, height)) {
                            drawable_drawn = true;
                        }
                        ++executed_commands;
                        break;
                    }
                    case Scene::DrawCommandKind::Mesh: {
                        auto mesh_cmd = read_struct<Scene::MeshCommand>(bucket->command_payload,
                                                                        payload_offset);
                        record_material_kind(Scene::DrawCommandKind::Mesh);
                        if (material_desc) {
                            material_desc->color_rgba = mesh_cmd.color;
                            material_desc->uses_image = false;
                        }
                        if (draw_mesh_command(mesh_cmd, *bucket, drawable_index, linear_buffer, width, height)) {
                            drawable_drawn = true;
                        }
                        ++executed_commands;
                        break;
                    }
                    case Scene::DrawCommandKind::Image: {
                        auto image_cmd = read_struct<Scene::ImageCommand>(bucket->command_payload,
                                                                         payload_offset);
                        record_material_kind(Scene::DrawCommandKind::Image);
                        if (material_desc) {
                            material_desc->uses_image = true;
                            material_desc->resource_fingerprint = image_cmd.image_fingerprint;
                            material_desc->tint_rgba = image_cmd.tint;
                        }
                        auto tint_straight = make_linear_straight(image_cmd.tint);
                        auto asset_path = image_asset_prefix + fingerprint_to_hex(image_cmd.image_fingerprint) + ".png";
                        auto texture = image_cache_.load(space_, asset_path, image_cmd.image_fingerprint);
                        if (!texture) {
                            auto const error = texture.error();
                            if (error.code != SP::Error::Code::NoObjectFound
                                && error.code != SP::Error::Code::NoSuchPath) {
                                auto message = error.message.value_or("failed to load image asset");
                                (void)set_last_error(space_, params.target_path, message);
                            }
                            ++unsupported_commands;
                            break;
                        }
                        auto const& image_data = *texture;
                        if (draw_image_command(image_cmd,
                                               *image_data,
                                               tint_straight,
                                               linear_buffer,
                                               width,
                                               height)) {
                            drawable_drawn = true;
                        }
                        ++executed_commands;
                        break;
                    }
                    default:
                        ++unsupported_commands;
                        break;
                    }
                }
            }
        } else {
            drawable_drawn = true;
        }

        if (!drawable_drawn) {
            if (!fallback_attempted) {
                drawable_drawn = draw_fallback_bounds_box(*bucket,
                                                          drawable_index,
                                                          linear_buffer,
                                                          width,
                                                          height);
                fallback_attempted = true;
            }
            if (!drawable_drawn) {
                ++culled_drawables;
                return {};
            }
        }

        ++drawn_total;
        if (alpha_pass) {
            ++drawn_alpha;
        } else {
            ++drawn_opaque;
        }
        auto area = approximate_drawable_area(*bucket, drawable_index);
        approx_area_total += area;
        if (alpha_pass) {
            approx_area_alpha += area;
        } else {
            approx_area_opaque += area;
        }
        if (drawable_drawn && material_desc) {
            material_desc->drawable_count += 1;
        }
        return {};
    };

    std::vector<std::uint32_t> fallback_opaque;
    std::vector<std::uint32_t> fallback_alpha;
    if (bucket->opaque_indices.empty() && bucket->alpha_indices.empty()) {
        fallback_opaque.reserve(drawable_count);
        fallback_alpha.reserve(drawable_count);
        for (std::uint32_t i = 0; i < drawable_count; ++i) {
            auto flags = pipeline_flags_for(*bucket, i);
            if (PipelineFlags::is_alpha_pass(flags)) {
                fallback_alpha.push_back(i);
            } else {
                fallback_opaque.push_back(i);
            }
        }
        if (fallback_opaque.empty() && fallback_alpha.empty()) {
            fallback_opaque.resize(drawable_count);
            std::iota(fallback_opaque.begin(), fallback_opaque.end(), 0u);
        }
    }

    auto process_pass = [&](std::vector<std::uint32_t> const& indices,
                            bool alpha_pass,
                            bool explicit_order) -> SP::Expected<void> {
        bool have_prev = false;
        std::uint32_t prev_layer = 0;
        std::uint32_t prev_material = 0;
        std::uint32_t prev_flags = 0;
        float prev_z = 0.0f;

        for (auto drawable_index : indices) {
            if (auto status = process_drawable(drawable_index, alpha_pass); !status) {
                return status;
            }
            if (!explicit_order) {
                continue;
            }
            if (alpha_pass) {
                if (drawable_index >= bucket->layers.size()
                    || drawable_index >= bucket->z_values.size()) {
                    have_prev = false;
                    continue;
                }
                auto layer = bucket->layers[drawable_index];
                auto z = bucket->z_values[drawable_index];
                if (have_prev) {
                    if (layer < prev_layer
                        || (layer == prev_layer && z > prev_z + kSortEpsilon)) {
                        ++alpha_sort_violations;
                    }
                }
                prev_layer = layer;
                prev_z = z;
                have_prev = true;
            } else {
                if (drawable_index >= bucket->layers.size()
                    || drawable_index >= bucket->material_ids.size()
                    || drawable_index >= bucket->z_values.size()) {
                    have_prev = false;
                    continue;
                }
                auto layer = bucket->layers[drawable_index];
                auto material = bucket->material_ids[drawable_index];
                auto flags = pipeline_flags_for(*bucket, drawable_index);
                auto z = bucket->z_values[drawable_index];
                if (have_prev) {
                    bool violation = false;
                    if (layer < prev_layer) {
                        violation = true;
                    } else if (layer == prev_layer) {
                        if (material < prev_material) {
                            violation = true;
                        } else if (material == prev_material) {
                            if (flags < prev_flags) {
                                violation = true;
                            } else if (flags == prev_flags && z < prev_z - kSortEpsilon) {
                                violation = true;
                            }
                        }
                    }
                    if (violation) {
                        ++opaque_sort_violations;
                    }
                }
                prev_layer = layer;
                prev_material = material;
                prev_flags = flags;
                prev_z = z;
                have_prev = true;
            }
        }
        return {};
    };

    if (!bucket->opaque_indices.empty()) {
        if (auto status = process_pass(bucket->opaque_indices, false, true); !status) {
            (void)set_last_error(space_, params.target_path,
                                 status.error().message.value_or("failed to store present metrics"));
            return std::unexpected(status.error());
        }
    } else if (!fallback_opaque.empty()) {
        if (auto status = process_pass(fallback_opaque, false, false); !status) {
            (void)set_last_error(space_, params.target_path,
                                 status.error().message.value_or("failed to store present metrics"));
            return std::unexpected(status.error());
        }
    }

    if (!bucket->alpha_indices.empty()) {
        if (auto status = process_pass(bucket->alpha_indices, true, true); !status) {
            (void)set_last_error(space_, params.target_path,
                                 status.error().message.value_or("failed to store present metrics"));
            return std::unexpected(status.error());
        }
    } else if (!fallback_alpha.empty()) {
        if (auto status = process_pass(fallback_alpha, true, false); !status) {
            (void)set_last_error(space_, params.target_path,
                                 status.error().message.value_or("failed to store present metrics"));
            return std::unexpected(status.error());
        }
    }

    auto const encode_srgb = needs_srgb_encode(desc);
    if (has_damage) {
        auto const encode_start = std::chrono::steady_clock::now();
        for (auto const& rect : damage.rectangles()) {
            for (int row = rect.min_y; row < rect.max_y; ++row) {
                auto* row_ptr = staging.data() + static_cast<std::size_t>(row) * stride;
                auto* linear_row = linear_buffer.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4u;
                for (int col = rect.min_x; col < rect.max_x; ++col) {
                    auto pixel_index = static_cast<std::size_t>(col) * 4u;
                    auto encoded = encode_pixel(linear_row + pixel_index, desc, encode_srgb);
                    auto offset = static_cast<std::size_t>(col) * 4u;
                    if (is_bgra) {
                        row_ptr[offset + 0] = encoded[2];
                        row_ptr[offset + 1] = encoded[1];
                        row_ptr[offset + 2] = encoded[0];
                    } else {
                        row_ptr[offset + 0] = encoded[0];
                        row_ptr[offset + 1] = encoded[1];
                        row_ptr[offset + 2] = encoded[2];
                    }
                    row_ptr[offset + 3] = encoded[3];
                }
            }
        }
        auto const encode_end = std::chrono::steady_clock::now();
        encode_ms = std::chrono::duration<double, std::milli>(encode_end - encode_start).count();
    }

    if (has_progressive && has_damage) {
        auto const progressive_start = std::chrono::steady_clock::now();
        auto const row_stride_bytes = surface.row_stride_bytes();
        auto staging_const = std::span<std::uint8_t const>{staging.data(), staging.size()};
        bool const prefer_parallel =
            progressive_buffer->tile_size() > 4
            && progressive_dirty_tiles.size() >= 16;

        if (prefer_parallel) {
            ProgressiveTileCopyContext ctx{
                .surface = surface,
                .buffer = *progressive_buffer,
                .staging = staging_const,
                .row_stride_bytes = row_stride_bytes,
                .revision = sceneRevision->revision,
            };
            try {
                auto stats_copy = copy_progressive_tiles(progressive_dirty_tiles, ctx);
                progressive_tiles_updated += stats_copy.tiles_updated;
                progressive_bytes_copied += stats_copy.bytes_copied;
            } catch (std::exception const& ex) {
                auto message = std::string{"failed to update progressive tiles: "} + ex.what();
                (void)set_last_error(space_, params.target_path, message);
                return std::unexpected(make_error(std::move(message), SP::Error::Code::UnknownError));
            }
            for (auto tile_index : progressive_dirty_tiles) {
                surface.mark_progressive_dirty(tile_index);
            }
        } else {
            for (auto tile_index : progressive_dirty_tiles) {
                auto dims = progressive_buffer->tile_dimensions(tile_index);
                if (dims.width <= 0 || dims.height <= 0) {
                    continue;
                }
                auto writer = surface.begin_progressive_tile(tile_index, TilePass::OpaqueInProgress);
                auto tile_pixels = writer.pixels();
                auto const row_pitch = static_cast<std::size_t>(dims.width) * 4u;
                auto const tile_rows = std::max(dims.height, 0);
                for (int row = 0; row < tile_rows; ++row) {
                    auto const src_offset = (static_cast<std::size_t>(dims.y + row) * row_stride_bytes)
                                            + static_cast<std::size_t>(dims.x) * 4u;
                    auto const dst_offset = static_cast<std::size_t>(row) * tile_pixels.stride_bytes;
                    std::memcpy(tile_pixels.data + dst_offset,
                                staging_const.data() + src_offset,
                                row_pitch);
                }
                writer.commit(TilePass::AlphaDone, sceneRevision->revision);
                surface.mark_progressive_dirty(tile_index);
                ++progressive_tiles_updated;
                progressive_bytes_copied += row_pitch * static_cast<std::uint64_t>(tile_rows);
            }
        }
        auto const progressive_end = std::chrono::steady_clock::now();
        progressive_copy_ms = std::chrono::duration<double, std::milli>(progressive_end - progressive_start).count();
    }

    auto const end = std::chrono::steady_clock::now();
    auto render_ms = std::chrono::duration<double, std::milli>(end - start).count();

    PathSurfaceSoftware::FrameInfo const frame_info{
        .frame_index = params.settings.time.frame_index,
        .revision = sceneRevision->revision,
        .render_ms = render_ms,
    };
    auto const publish_start = std::chrono::steady_clock::now();
    if (has_buffered && has_damage) {
        surface.publish_buffered_frame(frame_info);
    } else {
        surface.record_frame_info(frame_info);
    }
    auto const publish_end = std::chrono::steady_clock::now();
    publish_ms = std::chrono::duration<double, std::milli>(publish_end - publish_start).count();

    double approx_surface_pixels = static_cast<double>(pixel_count);
    double approx_overdraw_factor = 0.0;
    if (approx_surface_pixels > 0.0) {
        approx_overdraw_factor = approx_area_total / approx_surface_pixels;
    }

    auto& material_list = state.material_list;
    material_list.clear();
    material_list.reserve(material_descriptors.size());
    for (auto const& entry : material_descriptors) {
        material_list.push_back(entry.second);
    }
    std::sort(material_list.begin(),
              material_list.end(),
              [](MaterialDescriptor const& lhs, MaterialDescriptor const& rhs) {
                  return lhs.material_id < rhs.material_id;
              });

    auto metricsBase = std::string(params.target_path.getPath()) + "/output/v1/common";
    if (auto status = replace_single<std::uint64_t>(space_, metricsBase + "/frameIndex", params.settings.time.frame_index); !status) {
        (void)set_last_error(space_, params.target_path, "failed to store frame index");
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::uint64_t>(space_, metricsBase + "/revision", sceneRevision->revision); !status) {
        (void)set_last_error(space_, params.target_path, "failed to store revision");
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<double>(space_, metricsBase + "/renderMs", render_ms); !status) {
        (void)set_last_error(space_, params.target_path, "failed to store render duration");
        return std::unexpected(status.error());
    }
    if (auto status = replace_single<std::string>(space_, metricsBase + "/lastError", std::string{}); !status) {
        return std::unexpected(status.error());
    }
    (void)replace_single<double>(space_, metricsBase + "/damageMs", damage_ms);
    (void)replace_single<double>(space_, metricsBase + "/encodeMs", encode_ms);
    (void)replace_single<double>(space_, metricsBase + "/progressiveCopyMs", progressive_copy_ms);
    (void)replace_single<double>(space_, metricsBase + "/publishMs", publish_ms);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/drawableCount", static_cast<std::uint64_t>(drawable_count));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/opaqueDrawables", drawn_opaque);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/alphaDrawables", drawn_alpha);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/culledDrawables", culled_drawables);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/commandCount", static_cast<std::uint64_t>(bucket->command_kinds.size()));
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/commandsExecuted", executed_commands);

    if (auto status = set_last_error(space_, params.target_path, "", sceneRevision->revision, Builders::Diagnostics::PathSpaceError::Severity::Info); !status) {
        return std::unexpected(status.error());
    }
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/unsupportedCommands", unsupported_commands);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/opaqueSortViolations", opaque_sort_violations);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/alphaSortViolations", alpha_sort_violations);
    (void)replace_single<double>(space_, metricsBase + "/approxOpaquePixels", approx_area_opaque);
    (void)replace_single<double>(space_, metricsBase + "/approxAlphaPixels", approx_area_alpha);
    (void)replace_single<double>(space_, metricsBase + "/approxDrawablePixels", approx_area_total);
    (void)replace_single<double>(space_, metricsBase + "/approxOverdrawFactor", approx_overdraw_factor);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveTilesUpdated", progressive_tiles_updated);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveBytesCopied", progressive_bytes_copied);
    (void)replace_single<std::uint64_t>(space_, metricsBase + "/materialCount",
                                        static_cast<std::uint64_t>(material_list.size()));
    (void)replace_single(space_, metricsBase + "/materialDescriptors", material_list);
    if (collect_damage_metrics) {
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/damageRectangles", damage_rect_count);
        (void)replace_single<double>(space_, metricsBase + "/damageCoverageRatio", damage_coverage_ratio);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintMatchesExact", fingerprint_matches_exact);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintMatchesRemap", fingerprint_matches_remap);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintChanges", fingerprint_changed);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintNew", fingerprint_new);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/fingerprintRemoved", fingerprint_removed);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveTilesDirty", progressive_tiles_dirty);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveTilesTotal", progressive_tiles_total);
        (void)replace_single<std::uint64_t>(space_, metricsBase + "/progressiveTilesSkipped", progressive_tiles_skipped);
    }

    state.drawable_states = std::move(current_states);
    state.clear_color = current_clear;
    state.desc = desc;
    state.last_revision = sceneRevision->revision;

    RenderStats stats{};
    stats.frame_index = params.settings.time.frame_index;
    stats.revision = sceneRevision->revision;
    stats.render_ms = render_ms;
    stats.drawable_count = drawn_total;
    stats.damage_ms = damage_ms;
    stats.encode_ms = encode_ms;
    stats.progressive_copy_ms = progressive_copy_ms;
    stats.publish_ms = publish_ms;
    stats.backend_kind = params.backend_kind;
    stats.materials = material_list;
    auto const surface_bytes = surface.resident_cpu_bytes();
    auto const cache_bytes = image_cache_.resident_bytes();
    stats.resource_cpu_bytes = static_cast<std::uint64_t>(surface_bytes + cache_bytes);
    stats.resource_gpu_bytes = 0;

    return stats;
}

} // namespace SP::UI
