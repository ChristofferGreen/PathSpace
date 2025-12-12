#include "Common.hpp"

#include <algorithm>
#include <cmath>

namespace SP::UI::Declarative {

namespace WidgetDetail = SP::UI::Declarative::Detail;
namespace BuilderWidgets = SP::UI::Runtime::Widgets;
using SP::UI::Runtime::Widgets::WidgetSpacePath;
using SP::UI::Runtime::WidgetPath;

namespace {

auto sanitize_slider_range(float min_value,
                           float max_value,
                           float step) -> BuilderWidgets::SliderRange {
    BuilderWidgets::SliderRange range{};
    range.minimum = std::min(min_value, max_value);
    range.maximum = std::max(min_value, max_value);
    if (range.minimum == range.maximum) {
        range.maximum = range.minimum + 1.0f;
    }
    range.step = std::max(step, 0.0f);
    return range;
}

auto clamp_slider_value(float value,
                        BuilderWidgets::SliderRange const& range) -> float {
    float clamped = std::clamp(value, range.minimum, range.maximum);
    if (range.step > 0.0f) {
        float steps = std::round((clamped - range.minimum) / range.step);
        clamped = range.minimum + steps * range.step;
        clamped = std::clamp(clamped, range.minimum, range.maximum);
    }
    return clamped;
}

} // namespace

namespace Slider {

auto Fragment(Args args) -> WidgetFragment {
    auto range = sanitize_slider_range(args.minimum, args.maximum, args.step);
    args.style.width = std::max(args.style.width, 32.0f);
    args.style.height = std::max(args.style.height, 16.0f);
    args.style.track_height = std::clamp(args.style.track_height, 1.0f, args.style.height);
    args.style.thumb_radius =
        std::clamp(args.style.thumb_radius, args.style.track_height * 0.5f, args.style.height * 0.5f);
    auto clamped_value = clamp_slider_value(args.value, range);

    auto on_change = std::move(args.on_change);
    bool has_change_handler = static_cast<bool>(on_change);
    auto builder = WidgetDetail::FragmentBuilder{"slider",
                                   [args = std::move(args),
                                    range,
                                    clamped_value,
                                    has_change_handler](FragmentContext const& ctx) -> SP::Expected<void> {
                                       BuilderWidgets::SliderState state{};
                                       state.enabled = args.enabled;
                                       state.value = clamped_value;
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
                                                                                             "/meta/range"),
                                                                             range);
                                           !status) {
                                           return status;
                                       }
                                       if (auto status = WidgetDetail::initialize_render(ctx.space,
                                                                                  ctx.root,
                                                                                  WidgetKind::Slider);
                                           !status) {
                                           return status;
                                       }
                                       if (auto status = WidgetDetail::mirror_slider_capsule(ctx.space,
                                                                                             ctx.root,
                                                                                             state,
                                                                                             args.style,
                                                                                             range,
                                                                                             has_change_handler);
                                           !status) {
                                           return status;
                                       }
                                       return SP::Expected<void>{};
                                   }}
        ;

    if (on_change) {
        HandlerVariant handler = SliderHandler{std::move(*on_change)};
        builder.with_handler("change", HandlerKind::Slider, std::move(handler));
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

auto SetValue(PathSpace& space,
              WidgetPath const& widget,
              float value) -> SP::Expected<void> {
    auto range = space.read<BuilderWidgets::SliderRange, std::string>(
        WidgetSpacePath(widget.getPath(), "/meta/range"));
    if (!range) {
        return std::unexpected(range.error());
    }
    auto clamped = clamp_slider_value(value, *range);
    auto state = space.read<BuilderWidgets::SliderState, std::string>(
        WidgetSpacePath(widget.getPath(), "/state"));
    if (!state) {
        return std::unexpected(state.error());
    }
    if (state->value == clamped) {
        return {};
    }
    state->value = clamped;
    if (auto status = WidgetDetail::write_state(space, widget.getPath(), *state); !status) {
        return status;
    }
    if (auto status = WidgetDetail::update_slider_capsule_state(space, widget.getPath(), *state); !status) {
        return status;
    }
    return WidgetDetail::mark_render_dirty(space, widget.getPath());
}

} // namespace Slider

} // namespace SP::UI::Declarative
