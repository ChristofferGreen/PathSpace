#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/declarative/widgets/Common.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/WidgetPrimitives.hpp>

using SP::UI::Runtime::Widgets::WidgetSpacePath;

TEST_CASE("reset_widget_space does not create empty widget space") {
    SP::PathSpace space;
    std::string widget_root = "/app/widgets/ghost";

    // Simulate runtime clearing any previous widget data without writing new values.
    auto reset = SP::UI::Declarative::Detail::reset_widget_space(space, widget_root);
    REQUIRE(reset);

    auto children = space.listChildren(SP::ConcretePathStringView{widget_root});
    CHECK(children.empty());

    // First write should lazily create the nested widget space.
    auto write_kind = SP::UI::Declarative::Detail::write_value(
        space, WidgetSpacePath(widget_root, "/meta/kind"), std::string{"button"});
    REQUIRE(write_kind);

    children = space.listChildren(SP::ConcretePathStringView{widget_root});
    REQUIRE(children.size() == 1);
    CHECK(children.front() == "space");

    auto kind = space.read<std::string, std::string>(
        WidgetSpacePath(widget_root, "/meta/kind"));
    REQUIRE(kind);
    CHECK_EQ(*kind, "button");
}

TEST_CASE("WritePrimitives stays lazy when mirroring empty primitives") {
    SP::PathSpace space;
    std::string widget_root = "/app/widgets/primitives_none";

    SP::UI::Declarative::Primitives::WidgetPrimitiveIndex empty_index{};
    auto mirrored = SP::UI::Declarative::Primitives::WritePrimitives(
        space, widget_root, {}, empty_index);
    REQUIRE(mirrored);

    auto children = space.listChildren(SP::ConcretePathStringView{widget_root});
    CHECK(children.empty());
}
