#pragma once

#include <pathspace/ui/scenegraph/RenderCommandStore.hpp>

#include <parallel_hashmap/phmap.h>

#include <cstdint>
#include <span>
#include <vector>

namespace SP::UI::SceneGraph {

struct TileDim {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

struct TileGridConfig {
    int32_t tile_width = 64;
    int32_t tile_height = 64;
    int32_t surface_width = 0;
    int32_t surface_height = 0;
    std::size_t max_bucket_size = 256;
};

class TileGrid {
public:
    explicit TileGrid(TileGridConfig cfg);

    auto mark_dirty(IntRect const& bbox, CommandId id) -> void;
    auto clear_dirty() -> void;
    auto clear_all() -> void;

    [[nodiscard]] auto tiles() const -> std::span<TileDim const>;
    [[nodiscard]] auto tile_count() const -> std::size_t { return tiles_.size(); }
    [[nodiscard]] auto bucket(std::size_t tile_index) const -> std::vector<CommandId> const&;
    [[nodiscard]] auto dirty_tiles() const -> std::vector<std::size_t> const&;
    [[nodiscard]] auto tiles_for_rect(IntRect const& bbox) const -> std::vector<std::size_t>;

private:
    [[nodiscard]] auto tile_index(int32_t tx, int32_t ty) const -> std::size_t;
    auto for_each_tile(IntRect const& bbox, auto&& fn) const -> void;

    TileGridConfig              cfg_{};
    int32_t                     tiles_x_ = 0;
    int32_t                     tiles_y_ = 0;
    std::vector<TileDim>        tiles_{};
    std::vector<std::vector<CommandId>> buckets_{};
    std::vector<std::size_t>    dirty_{};
};

} // namespace SP::UI::SceneGraph
