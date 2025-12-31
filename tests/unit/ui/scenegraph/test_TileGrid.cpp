#include "third_party/doctest.h"

#include <pathspace/ui/scenegraph/TileGrid.hpp>

using namespace SP::UI::SceneGraph;

TEST_SUITE("TileGrid") {
    TEST_CASE("bbox_maps_to_expected_tiles") {
        TileGrid grid(TileGridConfig{.tile_width = 4, .tile_height = 4, .surface_width = 12, .surface_height = 8});
        grid.mark_dirty(IntRect{2, 2, 6, 6}, 1);
        auto dirty = grid.dirty_tiles();
        CHECK(dirty.size() == 4); // tiles (0,0), (1,0), (0,1), (1,1)
    }

    TEST_CASE("bucket_caps_at_max") {
        TileGrid grid(TileGridConfig{.tile_width = 8, .tile_height = 8, .surface_width = 8, .surface_height = 8, .max_bucket_size = 2});
        grid.mark_dirty(IntRect{0, 0, 8, 8}, 1);
        grid.mark_dirty(IntRect{0, 0, 8, 8}, 2);
        grid.mark_dirty(IntRect{0, 0, 8, 8}, 3);
        auto bucket = grid.bucket(0);
        CHECK(bucket.size() == 2);
        CHECK(bucket[0] == 1);
        CHECK(bucket[1] == 2);
    }

    TEST_CASE("clear_dirty_clears_only_dirty") {
        TileGrid grid(TileGridConfig{.tile_width = 4, .tile_height = 4, .surface_width = 8, .surface_height = 4});
        grid.mark_dirty(IntRect{0, 0, 4, 4}, 1);
        grid.mark_dirty(IntRect{4, 0, 8, 4}, 2);
        CHECK(grid.dirty_tiles().size() == 2);
        grid.clear_dirty();
        CHECK(grid.dirty_tiles().empty());
        CHECK(grid.bucket(0).empty());
        CHECK(grid.bucket(1).empty());
    }
}

