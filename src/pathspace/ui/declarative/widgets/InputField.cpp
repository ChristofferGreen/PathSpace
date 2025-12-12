#include "Common.hpp"

#include "../DescriptorDetail.hpp"

#include <utility>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
namespace DescriptorHelpers = SP::UI::Declarative::DescriptorDetail;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
using SP::UI::Runtime::WidgetPath;
using SP::UI::Runtime::Widgets::WidgetSpacePath;

namespace InputField {

auto Fragment(Args args) -> WidgetFragment {
    auto on_change = std::move(args.on_change);
    auto on_submit = std::move(args.on_submit);
    bool has_change_handler = static_cast<bool>(on_change);
    bool has_submit_handler = static_cast<bool>(on_submit);

    auto builder = WidgetDetail::FragmentBuilder{"input_field",
                                   [args = std::move(args),
                                    has_change_handler,
                                    has_submit_handler](FragmentContext const& ctx)
                                       -> SP::Expected<void> {
                                           auto text_path = WidgetSpacePath(ctx.root, "/state/text");
                                           auto placeholder_path = WidgetSpacePath(ctx.root, "/state/placeholder");
                                           auto focused_path = WidgetSpacePath(ctx.root, "/state/focused");
                                           auto hovered_path = WidgetSpacePath(ctx.root, "/state/hovered");
                                           auto enabled_path = WidgetSpacePath(ctx.root, "/state/enabled");
                                           auto cursor_path = WidgetSpacePath(ctx.root, "/state/cursor");
                                           auto selection_start_path =
                                               WidgetSpacePath(ctx.root, "/state/selection_start");
                                           auto selection_end_path =
                                               WidgetSpacePath(ctx.root, "/state/selection_end");

                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                text_path,
                                                                                args.text);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                placeholder_path,
                                                                                args.placeholder);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                focused_path,
                                                                                args.focused);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                hovered_path,
                                                                                false);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                enabled_path,
                                                                                true);
                                               !status) {
                                               return status;
                                           }

                                           auto cursor = static_cast<std::uint32_t>(args.text.size());
                                           if (auto status = WidgetDetail::write_value(ctx.space, cursor_path, cursor);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                selection_start_path,
                                                                                cursor);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::write_value(ctx.space,
                                                                                selection_end_path,
                                                                                cursor);
                                               !status) {
                                               return status;
                                           }

                                           BuilderWidgets::TextFieldStyle style{};
                                           auto theme = DescriptorHelpers::ResolveThemeForWidget(
                                               ctx.space, WidgetPath{ctx.root});
                                           if (theme) {
                                               style = theme->theme.text_field;
                                           }

                                           if (auto status = WidgetDetail::write_style(ctx.space,
                                                                                ctx.root,
                                                                                style);
                                               !status) {
                                               return status;
                                           }

                                           BuilderWidgets::TextFieldState capsule_state{};
                                           capsule_state.text = args.text;
                                           capsule_state.placeholder = args.placeholder;
                                           capsule_state.focused = args.focused;
                                           capsule_state.enabled = true;
                                           capsule_state.cursor = cursor;
                                           capsule_state.selection_start = cursor;
                                           capsule_state.selection_end = cursor;

                                           if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                      ctx.root,
                                                                                      WidgetKind::InputField);
                                               !status) {
                                               return status;
                                           }
                                           if (auto status = WidgetDetail::mirror_input_capsule(ctx.space,
                                                                                         ctx.root,
                                                                                         capsule_state,
                                                                                         style,
                                                                                         has_change_handler,
                                                                                         has_submit_handler);
                                               !status) {
                                               return status;
                                           }
                                           return SP::Expected<void>{};
                                       }};

    if (on_change) {
        HandlerVariant handler = InputFieldHandler{std::move(*on_change)};
        builder.with_handler("change", HandlerKind::InputChange, std::move(handler));
    }
    if (on_submit) {
        HandlerVariant handler = InputFieldHandler{std::move(*on_submit)};
        builder.with_handler("submit", HandlerKind::InputSubmit, std::move(handler));
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

auto SetText(PathSpace& space,
             WidgetPath const& widget,
             std::string_view text) -> SP::Expected<void> {
    auto widget_root = widget.getPath();
    auto updated_text = std::string(text);
    if (auto status = WidgetDetail::write_value(space,
                                          WidgetSpacePath(widget_root, "/state/text"),
                                          updated_text);
        !status) {
        return status;
    }

    auto cursor = static_cast<std::uint32_t>(updated_text.size());
    (void)WidgetDetail::write_value(space, WidgetSpacePath(widget_root, "/state/cursor"), cursor);
    (void)WidgetDetail::write_value(space,
                                    WidgetSpacePath(widget_root, "/state/selection_start"),
                                    cursor);
    (void)WidgetDetail::write_value(space,
                                    WidgetSpacePath(widget_root, "/state/selection_end"),
                                    cursor);

    auto capsule_state = space.read<BuilderWidgets::TextFieldState, std::string>(
        WidgetSpacePath(widget_root, "/capsule/state"));
    BuilderWidgets::TextFieldState state{};
    if (capsule_state) {
        state = *capsule_state;
    }
    state.text = updated_text;
    state.cursor = cursor;
    state.selection_start = cursor;
    state.selection_end = cursor;

    if (auto status = WidgetDetail::update_input_capsule_state(space, widget_root, state); !status) {
        return status;
    }

    return WidgetDetail::mark_render_dirty(space, widget_root);
}

} // namespace InputField

} // namespace SP::UI::Declarative
