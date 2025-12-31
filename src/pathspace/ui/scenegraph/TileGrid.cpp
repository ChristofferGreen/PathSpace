#include <pathspace/ui/scenegraph/TileGrid.hpp>

#include <algorithm>

namespace SP::UI::SceneGraph {

TileGrid::TileGrid(TileGridConfig cfg) : cfg_(cfg) {
    tiles_x_ = (cfg_.surface_width + cfg_.tile_width - 1) / cfg_.tile_width;
    tiles_y_ = (cfg_.surface_height + cfg_.tile_height - 1) / cfg_.tile_height;
    auto const total = static_cast<std::size_t>(tiles_x_) * static_cast<std::size_t>(tiles_y_);
    tiles_.reserve(total);
    buckets_.resize(total);
    for (int32_t y = 0; y < tiles_y_; ++y) {
        for (int32_t x = 0; x < tiles_x_; ++x) {
            tiles_.push_back(TileDim{
                .x = x * cfg_.tile_width,
                .y = y * cfg_.tile_height,
                .width = cfg_.tile_width,
                .height = cfg_.tile_height,
            });
        }
    }
}

auto TileGrid::tile_index(int32_t tx, int32_t ty) const -> std::size_t {
    return static_cast<std::size_t>(ty) * static_cast<std::size_t>(tiles_x_) + static_cast<std::size_t>(tx);
}

auto TileGrid::for_each_tile(IntRect const& bbox, auto&& fn) const -> void {
    if (bbox.empty()) {
        return;
    }
    auto min_tx = std::clamp(bbox.min_x / cfg_.tile_width, 0, tiles_x_ - 1);
    auto max_tx = std::clamp((bbox.max_x - 1) / cfg_.tile_width, 0, tiles_x_ - 1);
    auto min_ty = std::clamp(bbox.min_y / cfg_.tile_height, 0, tiles_y_ - 1);
    auto max_ty = std::clamp((bbox.max_y - 1) / cfg_.tile_height, 0, tiles_y_ - 1);

    for (int32_t ty = min_ty; ty <= max_ty; ++ty) {
        for (int32_t tx = min_tx; tx <= max_tx; ++tx) {
            fn(tile_index(tx, ty));
        }
    }
}

auto TileGrid::mark_dirty(IntRect const& bbox, CommandId id) -> void {
    for_each_tile(bbox, [&](std::size_t idx) {
        auto& bucket = buckets_[idx];
        if (bucket.size() < cfg_.max_bucket_size) {
            bucket.push_back(id);
        }
        if (bucket.size() == 1) {
            dirty_.push_back(idx);
        }
    });
}

auto TileGrid::clear_dirty() -> void {
    for (auto idx : dirty_) {
        buckets_[idx].clear();
    }
    dirty_.clear();
}

auto TileGrid::clear_all() -> void {
    for (auto& bucket : buckets_) {
        bucket.clear();
    }
    dirty_.clear();
}

auto TileGrid::tiles() const -> std::span<TileDim const> {
    return std::span<TileDim const>{tiles_};
}

auto TileGrid::bucket(std::size_t tile_index) const -> std::vector<CommandId> const& {
    return buckets_[tile_index];
}

auto TileGrid::dirty_tiles() const -> std::vector<std::size_t> const& {
    return dirty_;
}

auto TileGrid::tiles_for_rect(IntRect const& bbox) const -> std::vector<std::size_t> {
    std::vector<std::size_t> indices;
    for_each_tile(bbox, [&](std::size_t idx) {
        indices.push_back(idx);
    });
    return indices;
}

} // namespace SP::UI::SceneGraph
