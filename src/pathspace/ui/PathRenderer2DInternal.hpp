#pragma once

#include <pathspace/ui/PathRenderer2D.hpp>

#include <span>
#include <vector>
#include <cstdint>

namespace SP::UI {

class PathSurfaceSoftware;
class ProgressiveSurfaceBuffer;
struct TileDimensions;

namespace PathRenderer2DInternal {

struct DamageRect {
    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;

    static auto from_bounds(PathRenderer2D::DrawableBounds const& bounds) -> DamageRect;

    auto clamp(int width, int height) -> void;
    auto expand(int margin, int width, int height) -> void;

    [[nodiscard]] auto empty() const -> bool;
    [[nodiscard]] auto width() const -> int;
    [[nodiscard]] auto height() const -> int;
    [[nodiscard]] auto area() const -> std::uint64_t;

    auto merge(DamageRect const& other) -> void;
    [[nodiscard]] auto overlaps_or_touches(DamageRect const& other) const -> bool;
    [[nodiscard]] auto intersects(PathRenderer2D::DrawableBounds const& bounds) const -> bool;
    [[nodiscard]] auto intersects(TileDimensions const& tile) const -> bool;
    [[nodiscard]] auto intersect(DamageRect const& other) const -> DamageRect;
};

class DamageRegion {
public:
    void set_full(int width, int height);
    void add(PathRenderer2D::DrawableBounds const& bounds, int width, int height, int margin);
    void add_rect(DamageRect rect, int width, int height);
    void finalize(int width, int height);

    [[nodiscard]] auto empty() const -> bool;
    [[nodiscard]] auto intersects(PathRenderer2D::DrawableBounds const& bounds) const -> bool;
    [[nodiscard]] auto intersects(TileDimensions const& tile) const -> bool;
    [[nodiscard]] auto rectangles() const -> std::span<DamageRect const>;
    [[nodiscard]] auto area() const -> std::uint64_t;
    [[nodiscard]] auto coverage_ratio(int width, int height) const -> double;

    void collect_progressive_tiles(ProgressiveSurfaceBuffer const& buffer,
                                   std::vector<std::size_t>& out) const;
    void restrict_to(std::span<DamageRect const> limits);
    void replace_with_rects(std::span<DamageRect const> rects, int width, int height);

private:
    void merge_overlaps();

    bool full_surface_ = false;
    std::vector<DamageRect> rects_;
};

struct ProgressiveTileCopyStats {
    std::uint64_t tiles_updated = 0;
    std::uint64_t bytes_copied = 0;
    std::size_t workers_used = 0;
};

struct ProgressiveTileCopyContext {
    PathSurfaceSoftware& surface;
    ProgressiveSurfaceBuffer& buffer;
    std::span<std::uint8_t const> staging;
    std::size_t row_stride_bytes = 0;
    std::uint64_t revision = 0;
};

auto copy_progressive_tiles(std::span<std::size_t const> tile_indices,
                            ProgressiveTileCopyContext const& ctx) -> ProgressiveTileCopyStats;

auto choose_progressive_tile_size(int width,
                                  int height,
                                  DamageRegion const& damage,
                                  bool full_repaint,
                                  PathSurfaceSoftware const& surface) -> int;

} // namespace PathRenderer2DInternal
} // namespace SP::UI

