#include <pathspace/ui/declarative/Detail.hpp>

#include "../BuildersDetail.hpp"
#include "../WidgetDetail.hpp"

namespace SP::UI::Declarative::Detail {

auto write_stack_metadata(PathSpace& space,
                          std::string const& rootPath,
                          Widgets::StackLayoutStyle const& style,
                          std::vector<Widgets::StackChildSpec> const& children,
                          Widgets::StackLayoutState const& layout) -> SP::Expected<void> {
    return SP::UI::Builders::Detail::write_stack_metadata(space, rootPath, style, children, layout);
}

auto compute_stack_layout_state(PathSpace& space,
                                Widgets::StackLayoutParams const& params)
    -> SP::Expected<Widgets::StackLayoutState> {
    auto computed = SP::UI::Builders::Detail::compute_stack(space, params);
    if (!computed) {
        return std::unexpected(computed.error());
    }
    return computed->first.state;
}

void translate_bucket(SP::UI::Scene::DrawableBucketSnapshot& bucket, float x, float y) {
    SP::UI::Builders::Detail::translate_bucket(bucket, x, y);
}

void append_bucket(SP::UI::Scene::DrawableBucketSnapshot& target,
                   SP::UI::Scene::DrawableBucketSnapshot const& source) {
    SP::UI::Builders::Detail::append_bucket(target, source);
}

auto build_text_field_bucket(Widgets::TextFieldStyle const& style,
                             Widgets::TextFieldState const& state,
                             std::string_view authoring_root,
                             bool pulsing_highlight)
    -> SP::UI::Scene::DrawableBucketSnapshot {
    return SP::UI::Builders::Detail::build_text_field_bucket(style,
                                                             state,
                                                             authoring_root,
                                                             pulsing_highlight);
}

} // namespace SP::UI::Declarative::Detail
