#include "Common.hpp"

#include "../../WidgetDetail.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
using SP::UI::Builders::WidgetPath;
namespace BuilderDetail = SP::UI::Builders::Detail;
namespace BuilderWidgets = SP::UI::Builders::Widgets;

namespace {

auto panels_root(std::string const& root) -> std::string {
    return root + "/panels";
}

auto write_panel_metadata(PathSpace& space,
                          std::string const& root,
                          std::string const& panel_id,
                          std::uint32_t order,
                          bool visible) -> SP::Expected<void> {
    auto panel_root = panels_root(root) + "/" + panel_id;
    if (auto status = WidgetDetail::write_value(space, panel_root + "/order", order); !status) {
        return status;
    }
    if (auto status = WidgetDetail::write_value(space, panel_root + "/visible", visible); !status) {
        return status;
    }
    auto target = root + "/children/" + panel_id;
    return WidgetDetail::write_value(space, panel_root + "/target", target);
}

auto update_panel_visibility(PathSpace& space,
                             std::string const& root,
                             std::string const& active_panel) -> SP::Expected<void> {
    auto root_path = panels_root(root);
    auto panels = space.listChildren(SP::ConcretePathStringView{root_path});
    for (auto const& name : panels) {
        auto panel_root = root_path + "/" + name;
        bool is_active = name == active_panel;
        if (auto status = WidgetDetail::write_value(space, panel_root + "/visible", is_active); !status) {
            return status;
        }
    }
    return {};
}

auto read_panel_order(PathSpace& space,
                      std::string const& root,
                      std::string const& panel_id) -> std::uint32_t {
    auto order_path = panels_root(root) + "/" + panel_id + "/order";
    auto stored = space.read<std::uint32_t, std::string>(order_path);
    if (!stored) {
        return 0;
    }
    return *stored;
}

auto sorted_child_specs(PathSpace& space,
                        std::string const& root) -> std::vector<BuilderWidgets::StackChildSpec> {
    auto children_root = root + "/children";
    auto names = space.listChildren(SP::ConcretePathStringView{children_root});
    std::vector<BuilderWidgets::StackChildSpec> specs;
    specs.reserve(names.size());
    for (auto const& name : names) {
        BuilderWidgets::StackChildSpec spec{};
        spec.id = name;
        auto child_root = children_root + "/" + name;
        spec.widget_path = child_root;
        spec.scene_path = child_root;
        specs.push_back(std::move(spec));
    }
    std::sort(specs.begin(), specs.end(), [&](auto const& lhs, auto const& rhs) {
        auto left_order = read_panel_order(space, root, lhs.id);
        auto right_order = read_panel_order(space, root, rhs.id);
        if (left_order == right_order) {
            return lhs.id < rhs.id;
        }
        return left_order < right_order;
    });
    return specs;
}

auto rebuild_layout(PathSpace& space,
                    std::string const& root,
                    BuilderWidgets::StackLayoutStyle const& style) -> SP::Expected<void> {
    auto specs = sorted_child_specs(space, root);
    if (specs.empty()) {
        auto computed = BuilderWidgets::StackLayoutState{};
        computed.width = std::max(style.width, 0.0f);
        computed.height = std::max(style.height, 0.0f);
        if (auto status = BuilderDetail::write_stack_metadata(space, root, style, specs, computed); !status) {
            return status;
        }
        (void)WidgetDetail::mark_render_dirty(space, root);
        return {};
    }

    BuilderWidgets::StackLayoutParams params{};
    params.name = root;
    params.style = style;
    params.children = specs;

    auto computed = BuilderDetail::compute_stack(space, params);
    if (!computed) {
        return std::unexpected(computed.error());
    }
    auto layout_state = computed->first.state;
    if (auto status = BuilderDetail::write_stack_metadata(space, root, style, specs, layout_state); !status) {
        return status;
    }
    (void)WidgetDetail::mark_render_dirty(space, root);
    return {};
}

} // namespace

namespace Stack {

auto Fragment(Args args) -> WidgetFragment {
    std::vector<std::string> panel_ids;
    panel_ids.reserve(args.panels.size());
    for (auto const& panel : args.panels) {
        panel_ids.push_back(panel.id);
    }
    std::vector<std::pair<std::string, WidgetFragment>> child_fragments;
    child_fragments.reserve(args.panels.size());
    for (auto& panel : args.panels) {
        child_fragments.emplace_back(panel.id, std::move(panel.fragment));
    }

    auto on_select = std::move(args.on_select);
    auto builder = WidgetDetail::FragmentBuilder{"stack",
                                   [panel_ids = std::move(panel_ids),
                                    active_panel = std::move(args.active_panel)](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                ctx.root + "/state/active_panel",
                                                                                active_panel);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::Stack);
                                               !status) {
                                               return status;
                                           }
                                           for (std::uint32_t index = 0; index < panel_ids.size(); ++index) {
                                               auto const& panel_id = panel_ids[index];
                                               if (auto status = WidgetDetail::ensure_child_name(panel_id); !status) {
                                                   return status;
                                               }
                                               auto visible = panel_id == active_panel;
                                               if (auto status = write_panel_metadata(ctx.space,
                                                                                     ctx.root,
                                                                                     panel_id,
                                                                                     index,
                                                                                     visible);
                                                   !status) {
                                                   return status;
                                               }
                                           }
                                           return SP::Expected<void>{};
                                       }}
        .with_children(std::move(child_fragments));

    if (on_select) {
        HandlerVariant handler = StackPanelHandler{std::move(*on_select)};
        builder.with_handler("panel_select", HandlerKind::StackPanel, std::move(handler));
    }

    return builder.build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto style_override = args.style;
    auto fragment = Fragment(std::move(args));
    auto mounted = MountFragment(space, parent, name, fragment, options);
    if (!mounted) {
        return mounted;
    }
    auto status = rebuild_layout(space, mounted->getPath(), style_override);
    if (!status) {
        return std::unexpected(status.error());
    }
    return mounted;
}

auto SetActivePanel(PathSpace& space,
                    WidgetPath const& widget,
                    std::string_view panel_id) -> SP::Expected<void> {
    auto root = widget.getPath();
    if (auto status = WidgetDetail::write_value(space,
                                                root + "/state/active_panel",
                                                std::string(panel_id));
        !status) {
        return status;
    }
    if (auto status = update_panel_visibility(space, root, std::string(panel_id)); !status) {
        return status;
    }
    (void)WidgetDetail::mark_render_dirty(space, root);
    return {};
}

} // namespace Stack

} // namespace SP::UI::Declarative
