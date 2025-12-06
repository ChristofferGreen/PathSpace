#include "Common.hpp"

#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
using SP::UI::Runtime::Widgets::WidgetSpacePath;
using SP::UI::Runtime::WidgetPath;

namespace {

auto sanitize_list_items(std::vector<List::ListItem> items) -> std::vector<List::ListItem> {
    for (auto& item : items) {
        if (item.id.empty()) {
            item.id = item.label;
        }
    }
    return items;
}

} // namespace

namespace List {

auto Fragment(Args args) -> WidgetFragment {
    auto items = sanitize_list_items(std::move(args.items));
    auto children = std::move(args.children);
    auto on_child_event = std::move(args.on_child_event);
    auto builder = WidgetDetail::FragmentBuilder{"list",
                                   [args = std::move(args),
                                    items = std::move(items)](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           BuilderWidgets::ListState state{};
                                           if (auto status = WidgetDetail::write_state(ctx.space,
                                                                                ctx.root,
                                                                                state);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_style(ctx.space,
                                                                                ctx.root,
                                                                                args.style);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                WidgetSpacePath(ctx.root,
                                                                                                "/meta/items"),
                                                                                items);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::List);
                                               !status) {
                                               return status;
                                           }
                                           return SP::Expected<void>{};
                                       }}
        .with_children(std::move(children));

    if (on_child_event) {
        HandlerVariant handler = ListChildHandler{std::move(*on_child_event)};
        builder.with_handler("child_event", HandlerKind::ListChild, std::move(handler));
    }

    return builder.build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    return MountFragment(space, parent, name, fragment, options);
}

auto SetItems(PathSpace& space,
              WidgetPath const& widget,
              std::vector<ListItem> items) -> SP::Expected<void> {
    items = sanitize_list_items(std::move(items));
    if (auto status = WidgetDetail::write_value(space,
                                          WidgetSpacePath(widget.getPath(), "/meta/items"),
                                          std::move(items));
        !status) {
        return status;
    }
    return WidgetDetail::mark_render_dirty(space, widget.getPath());
}

} // namespace List

} // namespace SP::UI::Declarative
