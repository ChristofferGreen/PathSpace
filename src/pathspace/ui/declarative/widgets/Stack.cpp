#include "Common.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include <pathspace/ui/LocalWindowBridge.hpp>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
using SP::UI::Runtime::WidgetPath;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
using SP::UI::Runtime::Widgets::WidgetSpacePath;
namespace DeclarativeDetail = SP::UI::Declarative::Detail;

namespace {

auto panels_root(std::string const& root) -> std::string {
    return WidgetSpacePath(root, "/panels");
}

auto maybe_surface_size(PathSpace& space, std::string const& widget_root)
    -> std::optional<std::pair<int, int>> {
    int live_w = 0;
    int live_h = 0;
    SP::UI::GetLocalWindowContentSize(&live_w, &live_h);
    if (live_w > 0 && live_h > 0) {
        return std::make_pair(live_w, live_h);
    }

    // widget_root: /system/applications/<app>/windows/<win>/views/<view>/widgets/<id>
    auto widgets_pos = widget_root.find("/widgets/");
    auto windows_pos = widget_root.find("/windows/");
    if (widgets_pos == std::string::npos || windows_pos == std::string::npos) {
        return std::nullopt;
    }
    auto view_root = widget_root.substr(0, widgets_pos);
    auto app_root = widget_root.substr(0, windows_pos);

    auto surface_rel_path = view_root + "/surface";
    auto surface_rel = space.read<std::string, std::string>(surface_rel_path);
    if (!surface_rel) {
        return std::nullopt;
    }
    auto surface_abs = SP::App::resolve_app_relative(SP::App::AppRootPathView{app_root}, *surface_rel);
    if (!surface_abs) {
        return std::nullopt;
    }

    auto desc = space.read<SP::UI::Runtime::SurfaceDesc, std::string>(
        std::string(surface_abs->getPath()) + "/desc");
    if (!desc) {
        return std::nullopt;
    }
    return std::make_pair(desc->size_px.width, desc->size_px.height);
}

auto write_panel_metadata(PathSpace& space,
                          std::string const& root,
                          std::string const& panel_id,
                          std::uint32_t order,
                          bool visible,
                          BuilderWidgets::StackChildConstraints const* constraints) -> SP::Expected<void> {
    auto panel_root = panels_root(root) + "/" + panel_id;
    if (auto status = WidgetDetail::write_value(space, panel_root + "/order", order); !status) {
        return status;
    }
    if (auto status = WidgetDetail::write_value(space, panel_root + "/visible", visible); !status) {
        return status;
    }
    auto target = BuilderWidgets::WidgetChildRoot(space, root, panel_id);
    if (auto status = WidgetDetail::write_value(space, panel_root + "/target", target); !status) {
        return status;
    }
    if (constraints) {
        if (auto status = WidgetDetail::write_value(space, panel_root + "/constraints", *constraints); !status) {
            return status;
        }
    }
    return {};
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
    auto names = BuilderWidgets::WidgetChildNames(space, root);
    std::vector<BuilderWidgets::StackChildSpec> specs;
    specs.reserve(names.size());
    for (auto const& name : names) {
        BuilderWidgets::StackChildSpec spec{};
        spec.id = name;
        auto child_root = BuilderWidgets::WidgetChildRoot(space, root, name);
        auto canonical_child = BuilderWidgets::WidgetSpaceRoot(child_root);
        spec.widget_path = canonical_child;
        spec.scene_path = canonical_child;
        auto constraints_path = panels_root(root) + "/" + name + "/constraints";
        auto constraints = space.read<BuilderWidgets::StackChildConstraints, std::string>(constraints_path);
        if (constraints) {
            spec.constraints = *constraints;
        }
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
    BuilderWidgets::StackLayoutStyle effective_style = style;
    if ((effective_style.width <= 0.0f || effective_style.height <= 0.0f)) {
        if (auto surface_size = maybe_surface_size(space, root)) {
            if (effective_style.width <= 0.0f) {
                effective_style.width = static_cast<float>(surface_size->first);
            }
            if (effective_style.height <= 0.0f) {
                effective_style.height = static_cast<float>(surface_size->second);
            }
        }
    }
    auto specs = sorted_child_specs(space, root);
    if (specs.empty()) {
        auto computed = BuilderWidgets::StackLayoutState{};
        computed.width = std::max(effective_style.width, 0.0f);
        computed.height = std::max(effective_style.height, 0.0f);
        if (auto status = DeclarativeDetail::write_stack_metadata(space, root, effective_style, specs, computed); !status) {
            return status;
        }
        (void)WidgetDetail::mark_render_dirty(space, root);
        return {};
    }

    BuilderWidgets::StackLayoutParams params{};
    params.name = root;
    params.style = effective_style;
    params.children = specs;

    auto computed = DeclarativeDetail::compute_stack_layout_state(space, params);
    if (!computed) {
        return std::unexpected(computed.error());
    }
    auto layout_state = *computed;
    if (auto status = DeclarativeDetail::write_stack_metadata(space, root, effective_style, specs, layout_state); !status) {
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
    std::vector<BuilderWidgets::StackChildConstraints> panel_constraints;
    panel_constraints.reserve(args.panels.size());
    for (auto const& panel : args.panels) {
        panel_ids.push_back(panel.id);
        panel_constraints.push_back(panel.constraints);
    }
    std::vector<std::pair<std::string, WidgetFragment>> child_fragments;
    child_fragments.reserve(args.panels.size());
    for (auto& panel : args.panels) {
        child_fragments.emplace_back(panel.id, std::move(panel.fragment));
    }

    auto on_select = std::move(args.on_select);
    auto style_override = args.style;
    bool has_select_handler = static_cast<bool>(on_select);

    auto builder = WidgetDetail::FragmentBuilder{
        "stack",
        [panel_ids = std::move(panel_ids),
         panel_constraints = std::move(panel_constraints),
         active_panel = std::move(args.active_panel),
         style_override,
         has_select_handler](FragmentContext const& ctx) -> SP::Expected<void> {
            if (auto status = WidgetDetail::write_value(ctx.space,
                                                        WidgetSpacePath(ctx.root, "/state/active_panel"),
                                                        active_panel);
                !status) {
                return status;
            }
            if (auto status = WidgetDetail::initialize_render(ctx.space, ctx.root, WidgetKind::Stack);
                !status) {
                return status;
            }
            for (std::uint32_t index = 0; index < panel_ids.size(); ++index) {
                auto const& panel_id = panel_ids[index];
                auto const* constraints = index < panel_constraints.size() ? &panel_constraints[index] : nullptr;
                if (auto status = WidgetDetail::ensure_child_name(panel_id); !status) {
                    return status;
                }
                auto visible = panel_id == active_panel;
                if (auto status = write_panel_metadata(ctx.space,
                                                      ctx.root,
                                                      panel_id,
                                                      index,
                                                      visible,
                                                      constraints);
                    !status) {
                    return status;
                }
            }
            if (auto status = WidgetDetail::mirror_stack_capsule(ctx.space,
                                                                 ctx.root,
                                                                 style_override,
                                                                 panel_ids,
                                                                 active_panel,
                                                                 has_select_handler);
                !status) {
                return status;
            }
            return SP::Expected<void>{};
        }}
        .with_children(std::move(child_fragments));

    if (on_select) {
        HandlerVariant handler = StackPanelHandler{std::move(*on_select)};
        builder.with_handler("panel_select", HandlerKind::StackPanel, std::move(handler));
    }

    builder.with_finalize([style_override = std::move(style_override)](FragmentContext const& ctx) {
        return rebuild_layout(ctx.space, ctx.root, style_override);
    });

    return builder.build();
}

auto Create(PathSpace& space,
            SP::App::ConcretePathView parent,
            std::string_view name,
            Args args,
            MountOptions const& options) -> SP::Expected<WidgetPath> {
    auto fragment = Fragment(std::move(args));
    auto mounted = MountFragment(space, parent, name, fragment, options);
    if (!mounted) {
        return mounted;
    }
    return mounted;
}

auto SetActivePanel(PathSpace& space,
                    WidgetPath const& widget,
                    std::string_view panel_id) -> SP::Expected<void> {
    auto root = widget.getPath();
    if (auto status = WidgetDetail::write_value(space,
                                                WidgetSpacePath(root, "/state/active_panel"),
                                                std::string(panel_id));
        !status) {
        return status;
    }
    if (auto status = update_panel_visibility(space, root, std::string(panel_id)); !status) {
        return status;
    }
    if (auto status = WidgetDetail::update_stack_capsule_state(space, root, std::string(panel_id));
        !status) {
        return status;
    }
    (void)WidgetDetail::mark_render_dirty(space, root);
    return {};
}

} // namespace Stack

} // namespace SP::UI::Declarative
