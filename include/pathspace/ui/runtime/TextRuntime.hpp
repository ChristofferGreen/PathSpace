#pragma once

#include <pathspace/ui/declarative/Text.hpp>

namespace SP::UI::Runtime {

namespace Widgets {
struct TypographyStyle;
}

namespace Text {

using DrawableBucketSnapshot = SP::UI::Declarative::Text::DrawableBucketSnapshot;
using BuildResult = SP::UI::Declarative::Text::BuildResult;
using ScopedShapingContext = SP::UI::Declarative::Text::ScopedShapingContext;

inline auto MeasureTextWidth(std::string_view text,
                             Widgets::TypographyStyle const& typography) -> float {
    return SP::UI::Declarative::Text::MeasureTextWidth(text, typography);
}

inline auto BuildTextBucket(std::string_view text,
                            float origin_x,
                            float baseline_y,
                            Widgets::TypographyStyle const& typography,
                            std::array<float, 4> color,
                            std::uint64_t drawable_id,
                            std::string authoring_id,
                            float z_value) -> std::optional<BuildResult> {
    return SP::UI::Declarative::Text::BuildTextBucket(text,
                                                      origin_x,
                                                      baseline_y,
                                                      typography,
                                                      color,
                                                      drawable_id,
                                                      std::move(authoring_id),
                                                      z_value);
}

} // namespace Text

} // namespace SP::UI::Runtime
