#include <pathspace/ui/declarative/WidgetPrimitives.hpp>

#include <pathspace/ui/declarative/Detail.hpp>

#include <memory>

namespace SP::UI::Declarative::Primitives {

namespace {

using SP::UI::Declarative::Detail::replace_single;
using SP::UI::Runtime::Widgets::WidgetSpacePath;

auto primitives_root(std::string const& widget_root) -> std::string {
    return WidgetSpacePath(widget_root, "/capsule/primitives");
}

} // namespace

auto WritePrimitives(PathSpace& space,
                     std::string const& widget_root,
                     std::vector<WidgetPrimitive> const& primitives,
                     WidgetPrimitiveIndex const& index) -> SP::Expected<void> {
    auto root = primitives_root(widget_root);

    auto cleared = space.take<std::unique_ptr<PathSpace>>(root);
    if (!cleared) {
        auto const& error = cleared.error();
        if (error.code != SP::Error::Code::NoSuchPath
            && error.code != SP::Error::Code::NoObjectFound) {
            return std::unexpected(error);
        }
    }

    for (auto const& primitive : primitives) {
        auto path = WidgetSpacePath(widget_root, "/capsule/primitives/" + primitive.id);
        auto status = replace_single(space, path, primitive);
        if (!status) {
            return status;
        }
    }

    auto index_path = WidgetSpacePath(widget_root, "/capsule/primitives/index");
    return replace_single(space, index_path, index);
}

} // namespace SP::UI::Declarative::Primitives
