#include "third_party/doctest.h"

#include <pathspace/ui/scenegraph/RenderCommandStore.hpp>

using namespace SP::UI::SceneGraph;

TEST_SUITE("RenderCommandStore") {
    TEST_CASE("upsert_new_assigns_id_and_counts") {
        RenderCommandStore store;
        CommandDescriptor cmd{};
        cmd.entity_id = 42;
        cmd.bbox = IntRect{1, 2, 3, 4};
        auto const result = store.upsert(cmd);
        CHECK(result.id == 0);
        CHECK_FALSE(result.replaced);
        CHECK(store.active_count() == 1);
        auto ids = store.active_ids();
        REQUIRE(ids.size() == 1);
        CHECK(ids[0] == 0);
        CHECK(store.entity_id(0) == 42);
        CHECK(store.bbox(0).min_x == 1);
    }

    TEST_CASE("upsert_replace_updates_bbox_and_tracks_previous") {
        RenderCommandStore store;
        CommandDescriptor cmd{};
        cmd.entity_id = 7;
        cmd.bbox = IntRect{0, 0, 10, 10};
        auto first = store.upsert(cmd);
        cmd.bbox = IntRect{5, 5, 15, 20};
        auto second = store.upsert(cmd);
        CHECK(second.replaced);
        CHECK(second.id == first.id);
        REQUIRE(second.previous_bbox.has_value());
        CHECK(second.previous_bbox->min_x == 0);
        CHECK(store.active_count() == 1);
        CHECK(store.bbox(first.id).max_y == 20);
    }

    TEST_CASE("remove_entity_returns_bbox_and_reuses_slot") {
        RenderCommandStore store;
        CommandDescriptor cmd{};
        cmd.entity_id = 1;
        cmd.bbox = IntRect{0, 0, 8, 8};
        auto first = store.upsert(cmd);
        auto removed = store.remove_entity(1);
        REQUIRE(removed.has_value());
        CHECK(removed->max_x == 8);
        CHECK(store.active_count() == 0);
        cmd.entity_id = 2;
        auto second = store.upsert(cmd);
        CHECK(second.id == first.id); // slot reused
    }
}

