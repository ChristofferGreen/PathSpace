#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/FontManager.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/TextBuilder.hpp>

#include <array>

namespace {

using namespace SP::UI::Builders;
namespace Text = SP::UI::Builders::Text;

auto default_typography() -> Widgets::TypographyStyle {
    Widgets::TypographyStyle typography{};
    typography.font_size = 28.0f;
    typography.line_height = 28.0f;
    typography.letter_spacing = 1.0f;
    typography.baseline_shift = 0.0f;
    typography.font_family = "PathSpaceSans";
    typography.font_style = "italic";
    typography.font_weight = "600";
    typography.language = "fr";
    typography.direction = "ltr";
    typography.font_resource_root = "/system/applications/demo_app/resources/fonts/PathSpaceSans/Italic";
    typography.font_active_revision = 7ull;
    typography.font_asset_fingerprint = 0xC001F00DDEADBEEFull;
    typography.font_features = {"kern", "liga"};
    typography.fallback_families = {"system-ui", "Helvetica"};
    return typography;
}

} // namespace

TEST_CASE("TextBuilder builds buckets for simple strings") {
    auto typography = default_typography();
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    auto result = Text::BuildTextBucket("AB",
                                        0.0f,
                                        0.0f,
                                        typography,
                                        color,
                                        0x1234u,
                                        "widgets/test/label",
                                        0.5f);
    REQUIRE(result.has_value());

    CHECK(result->width > 0.0f);
    CHECK(result->height > 0.0f);
    CHECK_FALSE(result->bucket.command_kinds.empty());
    CHECK_EQ(result->bucket.command_counts.size(), 1);
    CHECK_EQ(result->bucket.command_counts.front(),
             static_cast<std::uint32_t>(result->bucket.command_kinds.size()));
    CHECK_EQ(result->bucket.authoring_map.size(), 1);
    CHECK_EQ(result->bucket.authoring_map.front().authoring_node_id, "widgets/test/label");
    CHECK_EQ(result->bucket.drawable_ids.size(), 1);
    CHECK_EQ(result->bucket.drawable_ids.front(), 0x1234u);
    CHECK_EQ(result->font_family, typography.font_family);
    CHECK_EQ(result->font_style, typography.font_style);
    CHECK_EQ(result->font_weight, typography.font_weight);
    CHECK_EQ(result->language, typography.language);
    CHECK_EQ(result->direction, typography.direction);
    CHECK_EQ(result->font_resource_root, typography.font_resource_root);
    CHECK_EQ(result->font_revision, typography.font_active_revision);
    CHECK_EQ(result->font_asset_fingerprint, typography.font_asset_fingerprint);
    CHECK_EQ(result->font_features, typography.font_features);
    CHECK_EQ(result->fallback_families, typography.fallback_families);
    REQUIRE_EQ(result->bucket.drawable_fingerprints.size(), 1);
    CHECK_EQ(result->bucket.drawable_fingerprints.front(), typography.font_asset_fingerprint);
    REQUIRE_EQ(result->bucket.font_assets.size(), 1);
    CHECK_EQ(result->bucket.font_assets.front().drawable_id, result->bucket.drawable_ids.front());
    CHECK_EQ(result->bucket.font_assets.front().resource_root, typography.font_resource_root);
    CHECK_EQ(result->bucket.font_assets.front().revision, typography.font_active_revision);
    CHECK_EQ(result->bucket.font_assets.front().fingerprint, typography.font_asset_fingerprint);
}

TEST_CASE("TextBuilder skips whitespace-only input") {
    auto typography = default_typography();
    std::array<float, 4> color{0.5f, 0.5f, 0.5f, 1.0f};
    auto result = Text::BuildTextBucket("   ",
                                        10.0f,
                                        5.0f,
                                        typography,
                                        color,
                                        0x55u,
                                        "widgets/test/empty",
                                        0.0f);
    CHECK_FALSE(result.has_value());
}

TEST_CASE("TextBuilder MeasureTextWidth provides non-negative widths") {
    auto typography = default_typography();
    CHECK(Text::MeasureTextWidth("", typography) == doctest::Approx(0.0f));
    CHECK(Text::MeasureTextWidth(" ", typography) >= 0.0f);
    CHECK(Text::MeasureTextWidth("Test", typography) >
          Text::MeasureTextWidth("T", typography));
}

TEST_CASE("TextBuilder builds shaped bucket when shaping context available") {
    SP::PathSpace space;
    SP::App::AppRootPath app_root{"/system/applications/demo_app"};
    SP::App::AppRootPathView app_view{app_root.getPath()};

    SP::UI::FontManager manager(space);
    SP::UI::Builders::Resources::Fonts::RegisterFontParams params{
        .family = "DemoSans",
        .style = "Regular",
        .weight = "400",
        .fallback_families = {"system-ui"},
        .initial_revision = 1ull,
        .atlas_soft_bytes = 4ull * 1024ull * 1024ull,
        .atlas_hard_bytes = 8ull * 1024ull * 1024ull,
        .shaped_run_approx_bytes = 512ull,
    };

    auto registered = manager.register_font(app_view, params);
    REQUIRE(registered);

    Widgets::TypographyStyle typography{};
    typography.font_family = params.family;
    typography.font_style = params.style;
    typography.font_weight = params.weight;
    typography.font_resource_root = registered->root.getPath();
    typography.font_active_revision = params.initial_revision;
    typography.font_size = 24.0f;
    typography.line_height = 24.0f;
    typography.letter_spacing = 0.0f;

    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};

    Text::ScopedShapingContext shaping(space, app_view);
    auto result = Text::BuildTextBucket("Hello",
                                        0.0f,
                                        0.0f,
                                        typography,
                                        color,
                                        0xBEEF,
                                        "widgets/test/shaped",
                                        0.0f);
    REQUIRE(result.has_value());
    REQUIRE_EQ(result->bucket.command_kinds.size(), 1);
    auto kind = static_cast<SP::UI::Scene::DrawCommandKind>(result->bucket.command_kinds.front());
    CHECK(kind == SP::UI::Scene::DrawCommandKind::TextGlyphs);
    CHECK_FALSE(result->bucket.glyph_vertices.empty());
    CHECK_EQ(result->bucket.font_assets.size(), 1);
    CHECK_EQ(result->bucket.font_assets.front().resource_root, typography.font_resource_root);
    CHECK_EQ(result->bucket.font_assets.front().revision, typography.font_active_revision);
}
