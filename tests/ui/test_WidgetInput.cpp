#include "third_party/doctest.h"

#include <pathspace/ui/runtime/UIRuntime.hpp>

namespace WidgetInput = SP::UI::Runtime::Widgets::Input;
namespace Widgets = SP::UI::Runtime::Widgets;

TEST_CASE("WidgetInput SliderPointerForValue computes horizontal position") {
    WidgetInput::WidgetInputContext ctx{};

    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    ctx.pointer_x = &pointer_x;
    ctx.pointer_y = &pointer_y;

    Widgets::SliderState slider_state{};
    ctx.slider_state = &slider_state;

    Widgets::SliderStyle slider_style{};
    slider_style.width = 200.0f;
    slider_style.height = 40.0f;
    slider_style.track_height = 10.0f;
    ctx.slider_style = &slider_style;

    Widgets::SliderRange slider_range{};
    slider_range.minimum = 0.0f;
    slider_range.maximum = 100.0f;
    slider_range.step = 0.0f;
    ctx.slider_range = &slider_range;

    WidgetInput::LayoutSnapshot layout{};
    WidgetInput::SliderLayout slider_layout{};
    slider_layout.bounds = WidgetInput::WidgetBounds{0.0f, 0.0f, 200.0f, 40.0f};
    slider_layout.track = WidgetInput::WidgetBounds{0.0f, 15.0f, 200.0f, 25.0f};
    layout.slider = slider_layout;
    ctx.layout = layout;

    auto midpoint = WidgetInput::SliderPointerForValue(ctx, 50.0f);
    CHECK_EQ(midpoint.first, doctest::Approx(100.0f));
    CHECK_EQ(midpoint.second, doctest::Approx(20.0f));

    auto minimum = WidgetInput::SliderPointerForValue(ctx, 0.0f);
    CHECK_EQ(minimum.first, doctest::Approx(0.0f));

auto maximum = WidgetInput::SliderPointerForValue(ctx, 100.0f);
CHECK_EQ(maximum.first, doctest::Approx(200.0f));
}

TEST_CASE("WidgetInput::BoundsFromRect normalizes bounds") {
    Widgets::ListPreviewRect rect{};
    rect.min_x = 40.0f;
    rect.max_x = 10.0f;
    rect.min_y = 30.0f;
    rect.max_y = 50.0f;
    auto bounds = WidgetInput::BoundsFromRect(rect);
    CHECK(bounds.min_x == doctest::Approx(10.0f));
    CHECK(bounds.max_x == doctest::Approx(40.0f));
    CHECK(bounds.height() == doctest::Approx(20.0f));
}

TEST_CASE("WidgetInput::ExpandForFocusHighlight grows bounds") {
    WidgetInput::WidgetBounds bounds{20.0f, 20.0f, 40.0f, 40.0f};
    WidgetInput::ExpandForFocusHighlight(bounds);
    CHECK(bounds.min_x == doctest::Approx(10.0f));
    CHECK(bounds.min_y == doctest::Approx(10.0f));
    CHECK(bounds.max_x == doctest::Approx(50.0f));
    CHECK(bounds.max_y == doctest::Approx(50.0f));
}

TEST_CASE("WidgetInput::MakeDirtyHint reflects bounds extents") {
    WidgetInput::WidgetBounds bounds{5.0f, 7.0f, 15.0f, 19.0f};
    auto hint = WidgetInput::MakeDirtyHint(bounds);
    CHECK(hint.min_x == doctest::Approx(5.0f));
    CHECK(hint.max_y == doctest::Approx(19.0f));
}

TEST_CASE("WidgetInput::MakeListLayout emits item bounds when rows exist") {
    Widgets::ListPreviewLayout layout{};
    layout.bounds = Widgets::ListPreviewRect{0.0f, 0.0f, 200.0f, 120.0f};
    layout.content_top = 4.0f;
    layout.item_height = 24.0f;
    layout.rows.push_back(Widgets::ListPreviewRowLayout{
        .id = "first",
        .enabled = true,
        .hovered = false,
        .selected = false,
        .row_bounds = Widgets::ListPreviewRect{0.0f, 4.0f, 200.0f, 28.0f},
    });
    auto result = WidgetInput::MakeListLayout(layout);
    REQUIRE(result);
    CHECK(result->bounds.max_x == doctest::Approx(200.0f));
    REQUIRE(result->item_bounds.size() == 1);
    CHECK(result->item_bounds.front().max_y == doctest::Approx(28.0f));

    Widgets::ListPreviewLayout empty_layout{};
    auto empty = WidgetInput::MakeListLayout(empty_layout);
    CHECK_FALSE(empty);
}
