#include "third_party/doctest.h"

#include <pathspace/ui/scenegraph/TileDirtyTracker.hpp>

using namespace SP::UI::SceneGraph;
using SP::UI::Runtime::DirtyRectHint;

namespace {

auto rect_equals(IntRect const& lhs, IntRect const& rhs) -> bool {
    return lhs.min_x == rhs.min_x && lhs.min_y == rhs.min_y && lhs.max_x == rhs.max_x
           && lhs.max_y == rhs.max_y;
}

} // namespace

TEST_SUITE("ui.scenegraph.render.tile.dirty.tracker") {
    TEST_CASE("marks_new_entities_dirty") {
        TileDirtyTracker tracker;
        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{0, 0, 2, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = SP::UI::Scene::DrawCommandKind::Rect,
            .payload_handle = 0,
            .entity_id = 1,
        });
        store.upsert(CommandDescriptor{
            .bbox = IntRect{4, 4, 6, 6},
            .z = 1,
            .opacity = 1.0f,
            .kind = SP::UI::Scene::DrawCommandKind::Rect,
            .payload_handle = 1,
            .entity_id = 2,
        });

        auto dirty = tracker.compute_dirty(store, {}, 8, 8, false);
        CHECK(dirty.size() == 2);
        CHECK((rect_equals(dirty[0], IntRect{0, 0, 2, 2}) || rect_equals(dirty[1], IntRect{0, 0, 2, 2})));
        CHECK((rect_equals(dirty[0], IntRect{4, 4, 6, 6}) || rect_equals(dirty[1], IntRect{4, 4, 6, 6})));
    }

    TEST_CASE("replacements_union_old_and_new_bbox") {
        TileDirtyTracker tracker;
        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{0, 0, 2, 2},
            .z = 0,
            .opacity = 1.0f,
            .kind = SP::UI::Scene::DrawCommandKind::Rect,
            .payload_handle = 0,
            .entity_id = 10,
        });
        // Seed previous frame.
        (void)tracker.compute_dirty(store, {}, 8, 8, false);

        RenderCommandStore updated;
        updated.upsert(CommandDescriptor{
            .bbox = IntRect{1, 1, 4, 3},
            .z = 0,
            .opacity = 1.0f,
            .kind = SP::UI::Scene::DrawCommandKind::Rect,
            .payload_handle = 0,
            .entity_id = 10,
        });

        auto dirty = tracker.compute_dirty(updated, {}, 8, 8, false);
        REQUIRE(dirty.size() == 1);
        CHECK(rect_equals(dirty[0], IntRect{0, 0, 4, 3}));
    }

    TEST_CASE("removals_mark_previous_bbox_dirty") {
        TileDirtyTracker tracker;
        RenderCommandStore store;
        store.upsert(CommandDescriptor{
            .bbox = IntRect{2, 2, 6, 6},
            .z = 0,
            .opacity = 1.0f,
            .kind = SP::UI::Scene::DrawCommandKind::Rect,
            .payload_handle = 0,
            .entity_id = 20,
        });
        (void)tracker.compute_dirty(store, {}, 8, 8, false);

        RenderCommandStore cleared;
        auto dirty = tracker.compute_dirty(cleared, {}, 8, 8, false);
        REQUIRE(dirty.size() == 1);
        CHECK(rect_equals(dirty[0], IntRect{2, 2, 6, 6}));
    }

    TEST_CASE("dirty_hints_are_clamped") {
        TileDirtyTracker tracker;
        RenderCommandStore store;
        DirtyRectHint hint{
            .min_x = -5.0f,
            .min_y = -5.0f,
            .max_x = 10.0f,
            .max_y = 10.0f,
        };
        auto dirty = tracker.compute_dirty(store, std::span<DirtyRectHint const>{&hint, 1}, 8, 8, false);
        REQUIRE(dirty.size() == 1);
        CHECK(rect_equals(dirty[0], IntRect{0, 0, 8, 8}));
    }
}
