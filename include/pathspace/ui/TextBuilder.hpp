#pragma once

#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace SP::UI::Builders {

namespace Widgets {
struct TypographyStyle;
}

namespace Text {

using DrawableBucketSnapshot = SP::UI::Scene::DrawableBucketSnapshot;

struct BuildResult {
    DrawableBucketSnapshot bucket;
    float width = 0.0f;
    float height = 0.0f;
    std::string font_family;
    std::string font_style;
    std::string font_weight;
    std::string language;
    std::string direction;
};

auto MeasureTextWidth(std::string_view text,
                      Widgets::TypographyStyle const& typography) -> float;

auto BuildTextBucket(std::string_view text,
                     float origin_x,
                     float baseline_y,
                     Widgets::TypographyStyle const& typography,
                     std::array<float, 4> color,
                     std::uint64_t drawable_id,
                     std::string authoring_id,
                     float z_value) -> std::optional<BuildResult>;

} // namespace Text

} // namespace SP::UI::Builders
