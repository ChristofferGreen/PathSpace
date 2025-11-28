#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SP::UI::Runtime::Widgets {
struct TypographyStyle;
}

namespace SP::UI::Declarative::Text {

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
    std::string font_resource_root;
    std::uint64_t font_revision = 0;
    std::uint64_t font_asset_fingerprint = 0;
    std::vector<std::string> font_features;
    std::vector<std::string> fallback_families;
};

class ScopedShapingContext {
public:
    ScopedShapingContext(SP::PathSpace& space, SP::App::AppRootPathView appRoot);
    ~ScopedShapingContext();

    ScopedShapingContext(ScopedShapingContext const&) = delete;
    ScopedShapingContext& operator=(ScopedShapingContext const&) = delete;
    ScopedShapingContext(ScopedShapingContext&&) = delete;
    ScopedShapingContext& operator=(ScopedShapingContext&&) = delete;

private:
    bool active_ = false;
    bool had_previous_ = false;
    SP::PathSpace* previous_space_ = nullptr;
    void* previous_manager_ = nullptr;
    std::string previous_app_root_;
};

auto MeasureTextWidth(std::string_view text,
                      SP::UI::Runtime::Widgets::TypographyStyle const& typography) -> float;

auto BuildTextBucket(std::string_view text,
                     float origin_x,
                     float baseline_y,
                     SP::UI::Runtime::Widgets::TypographyStyle const& typography,
                     std::array<float, 4> color,
                     std::uint64_t drawable_id,
                     std::string authoring_id,
                     float z_value) -> std::optional<BuildResult>;

} // namespace SP::UI::Declarative::Text
