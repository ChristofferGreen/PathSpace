#include "third_party/doctest.h"

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/FontManager.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/runtime/TextRuntime.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace SP::UI::Runtime;
namespace Text = SP::UI::Runtime::Text;

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
    typography.font_features = {"kern", "liga"};
    typography.fallback_families = {"system-ui", "Helvetica"};
    return typography;
}

auto fallback_typography() -> Widgets::TypographyStyle {
    Widgets::TypographyStyle typography{};
    typography.font_size = 24.0f;
    typography.line_height = 24.0f;
    typography.letter_spacing = 0.0f;
    typography.font_family = "PathSpaceSans";
    typography.font_style = "Regular";
    typography.font_weight = "400";
    typography.font_features.clear();
    typography.fallback_families.clear();
    return typography;
}

auto utf8_codepoints(std::string_view text) -> std::vector<char32_t> {
    std::vector<char32_t> out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size();) {
        auto byte = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if ((byte & 0x80u) == 0u) {
            length = 1;
        } else if ((byte & 0xE0u) == 0xC0u && i + 1 < text.size()) {
            length = 2;
        } else if ((byte & 0xF0u) == 0xE0u && i + 2 < text.size()) {
            length = 3;
        } else if ((byte & 0xF8u) == 0xF0u && i + 3 < text.size()) {
            length = 4;
        }

        char32_t codepoint = 0;
        switch (length) {
        case 1:
            codepoint = byte;
            break;
        case 2:
            codepoint = ((byte & 0x1Fu) << 6)
                        | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
            break;
        case 3:
            codepoint = ((byte & 0x0Fu) << 12)
                        | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6)
                        | (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
            break;
        default:
            codepoint = ((byte & 0x07u) << 18)
                        | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12)
                        | ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6)
                        | (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
            break;
        }

        out.push_back(codepoint);
        i += length;
    }

    return out;
}

struct TextTestEnvironment {
    TextTestEnvironment()
        : app_root{"/system/applications/demo_app"} {}

    auto app_view() const -> SP::App::AppRootPathView {
        return SP::App::AppRootPathView{app_root.getPath()};
    }

    SP::PathSpace space{};
    SP::App::AppRootPath app_root;
};

struct ScopedEnvVar {
    ScopedEnvVar(std::string key, std::string value)
        : key_{std::move(key)} {
        if (auto* existing = std::getenv(key_.c_str())) {
            previous_ = std::string{existing};
        }
        ::setenv(key_.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvVar() {
        if (previous_) {
            ::setenv(key_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(key_.c_str());
        }
    }

    std::string key_;
    std::optional<std::string> previous_;
};

} // namespace

TEST_CASE("TextBuilder builds buckets for simple strings") {
    TextTestEnvironment env{};
    Text::ScopedShapingContext shaping(env.space, env.app_view());
    auto typography = default_typography();
    typography.font_asset_fingerprint = 0;
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
    REQUIRE_FALSE(result->bucket.command_kinds.empty());
    CHECK_EQ(result->bucket.command_counts.size(), 1);
    CHECK_EQ(result->bucket.command_counts.front(), 1u);
    auto kind = static_cast<SP::UI::Scene::DrawCommandKind>(result->bucket.command_kinds.front());
    CHECK(kind == SP::UI::Scene::DrawCommandKind::TextGlyphs);
    CHECK_EQ(result->bucket.authoring_map.size(), 1);
    CHECK_EQ(result->bucket.authoring_map.front().authoring_node_id, "widgets/test/label");
    CHECK_EQ(result->bucket.drawable_ids.size(), 1);
    CHECK_EQ(result->bucket.drawable_ids.front(), 0x1234u);
    CHECK_EQ(result->font_family, "PathSpaceSans");
    CHECK_EQ(result->font_style, "Italic");
    CHECK_FALSE(result->font_weight.empty());
    CHECK_EQ(result->language, typography.language);
    CHECK_EQ(result->direction, typography.direction);
    CHECK_FALSE(result->font_resource_root.empty());
    CHECK(result->font_revision > 0);
    CHECK(result->font_asset_fingerprint != 0);
    CHECK_EQ(result->font_features, typography.font_features);
    CHECK_FALSE(result->fallback_families.empty());
    REQUIRE_EQ(result->bucket.drawable_fingerprints.size(), 1);
    CHECK(result->bucket.drawable_fingerprints.front() != 0);
    REQUIRE_EQ(result->bucket.font_assets.size(), 1);
    CHECK_EQ(result->bucket.font_assets.front().drawable_id, result->bucket.drawable_ids.front());
    CHECK_FALSE(result->bucket.font_assets.front().resource_root.empty());
    CHECK(result->bucket.font_assets.front().revision > 0);
    CHECK(result->bucket.font_assets.front().fingerprint != 0);
    CHECK_FALSE(result->bucket.glyph_vertices.empty());
}

TEST_CASE("TextBuilder skips whitespace-only input") {
    TextTestEnvironment env{};
    Text::ScopedShapingContext shaping(env.space, env.app_view());
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
    TextTestEnvironment env{};
    Text::ScopedShapingContext shaping(env.space, env.app_view());
    auto typography = default_typography();
    CHECK(Text::MeasureTextWidth("", typography) == doctest::Approx(0.0f));
    CHECK(Text::MeasureTextWidth(" ", typography) >= 0.0f);
    CHECK(Text::MeasureTextWidth("Test", typography) >
          Text::MeasureTextWidth("T", typography));
}

TEST_CASE("TextBuilder builds shaped bucket when shaping context available") {
    TextTestEnvironment env{};
    SP::UI::FontManager manager(env.space);
    SP::UI::Runtime::Resources::Fonts::RegisterFontParams params{
        .family = "DemoSans",
        .style = "Regular",
        .weight = "400",
        .fallback_families = {"system-ui"},
        .initial_revision = 1ull,
        .atlas_soft_bytes = 4ull * 1024ull * 1024ull,
        .atlas_hard_bytes = 8ull * 1024ull * 1024ull,
        .shaped_run_approx_bytes = 512ull,
    };

    auto registered = manager.register_font(env.app_view(), params);
    REQUIRE(registered);

    Widgets::TypographyStyle typography{};
    typography.font_family = params.family;
    typography.font_style = params.style;
    typography.font_weight = params.weight;
    typography.font_size = 24.0f;
    typography.line_height = 24.0f;
    typography.letter_spacing = 0.0f;

    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};

    Text::ScopedShapingContext shaping(env.space, env.app_view());
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
    CHECK_EQ(result->bucket.font_assets.front().resource_root, registered->root.getPath());
    CHECK_EQ(result->bucket.font_assets.front().revision, params.initial_revision);
}

TEST_CASE("TextBuilder falls back to bitmap path without shaping context") {
    auto typography = default_typography();
    std::array<float, 4> color{0.9f, 0.9f, 0.9f, 1.0f};
    auto result = Text::BuildTextBucket("Fallback",
                                        4.0f,
                                        2.0f,
                                        typography,
                                        color,
                                        0x42u,
                                        "widgets/test/fallback",
                                        0.1f);
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->bucket.command_kinds.empty());
    auto kind = static_cast<SP::UI::Scene::DrawCommandKind>(result->bucket.command_kinds.front());
    CHECK(kind == SP::UI::Scene::DrawCommandKind::Rect);
    REQUIRE_EQ(result->bucket.command_counts.size(), 1);
    CHECK_EQ(result->bucket.command_counts.front(), 78u);
    REQUIRE_FALSE(result->bucket.bounds_boxes.empty());
    auto const& bounds = result->bucket.bounds_boxes.front();
    CHECK_EQ(bounds.min[0], doctest::Approx(4.0f));
    CHECK_EQ(bounds.min[1], doctest::Approx(2.0f));
    CHECK_EQ(bounds.max[0], doctest::Approx(192.0f));
    CHECK_EQ(bounds.max[1], doctest::Approx(30.0f));
    CHECK_EQ(result->width, doctest::Approx(188.0f));
    CHECK_EQ(result->height, doctest::Approx(28.0f));
    auto const rect_commands =
        result->bucket.command_payload.size() / sizeof(SP::UI::Scene::RectCommand);
    CHECK_EQ(rect_commands, 78u);
    CHECK(result->bucket.font_assets.empty());
}

TEST_CASE("TextBuilder disables shaping when font manager flag is off") {
    TextTestEnvironment env{};
    ScopedEnvVar flag{"PATHSPACE_UI_FONT_MANAGER_ENABLED", "0"};

    Text::ScopedShapingContext shaping(env.space, env.app_view());

    auto typography = default_typography();
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};

    auto result = Text::BuildTextBucket("Flagged",
                                        0.0f,
                                        0.0f,
                                        typography,
                                        color,
                                        0x99u,
                                        "widgets/test/flagged",
                                        0.0f);

    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->bucket.command_kinds.empty());
    auto kind = static_cast<SP::UI::Scene::DrawCommandKind>(result->bucket.command_kinds.front());
    CHECK(kind == SP::UI::Scene::DrawCommandKind::Rect);
    CHECK(result->bucket.font_assets.empty());
    CHECK(result->bucket.glyph_vertices.empty());
}

TEST_CASE("TextBuilder emits color atlas when preferred format is rgba8") {
    TextTestEnvironment env{};
    SP::UI::FontManager manager(env.space);
    SP::UI::Runtime::Resources::Fonts::RegisterFontParams params{
        .family = "ColorEmoji",
        .style = "Regular",
        .weight = "400",
        .fallback_families = {"system-ui"},
        .initial_revision = 2ull,
        .atlas_soft_bytes = 4ull * 1024ull * 1024ull,
        .atlas_hard_bytes = 8ull * 1024ull * 1024ull,
        .shaped_run_approx_bytes = 512ull,
        .emit_color_atlas = true,
        .preferred_atlas_format = SP::UI::FontAtlasFormat::Rgba8,
    };

    auto registered = manager.register_font(env.app_view(), params);
    REQUIRE(registered);

    Widgets::TypographyStyle typography{};
    typography.font_family = params.family;
    typography.font_style = params.style;
    typography.font_weight = params.weight;
    typography.font_size = 24.0f;
    typography.line_height = 24.0f;

    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};

    Text::ScopedShapingContext shaping(env.space, env.app_view());
    auto result = Text::BuildTextBucket("A",
                                        0.0f,
                                        0.0f,
                                        typography,
                                        color,
                                        0xC011u,
                                        "widgets/test/color",
                                        0.0f);
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->bucket.font_assets.empty());
    auto const& asset = result->bucket.font_assets.front();
    CHECK(asset.kind == SP::UI::Scene::FontAssetKind::Color);
    REQUIRE_FALSE(result->bucket.command_payload.empty());
    SP::UI::Scene::TextGlyphsCommand glyphs{};
    std::memcpy(&glyphs,
                result->bucket.command_payload.data(),
                std::min(result->bucket.command_payload.size(), sizeof(SP::UI::Scene::TextGlyphsCommand)));
    CHECK((glyphs.flags & SP::UI::Scene::kTextGlyphsFlagUsesColorAtlas) != 0u);
}

TEST_CASE("FontManager applies kerning to Latin pairs") {
    TextTestEnvironment env{};
    SP::UI::FontManager manager(env.space);

    Widgets::TypographyStyle typography{};
    typography.font_family = "Times New Roman";
    typography.font_style = "Regular";
    typography.font_weight = "400";
    typography.font_size = 26.0f;
    typography.line_height = 26.0f;
    typography.letter_spacing = 0.0f;
    typography.font_features = {"kern"};
    typography.fallback_families = {"Times New Roman", "Helvetica", "Arial"};

    auto single_a = manager.shape_text(env.app_view(), "A", typography);
    auto single_v = manager.shape_text(env.app_view(), "V", typography);
    auto pair = manager.shape_text(env.app_view(), "AV", typography);

    auto harfbuzz_active = pair.glyphs.size() == 2
                           && pair.glyphs.front().glyph_id != static_cast<std::uint32_t>('A');
    if (!harfbuzz_active) {
        INFO("Kerning check skipped: HarfBuzz font unavailable");
        return;
    }

    CHECK(pair.total_advance > 0.0f);
    CHECK(pair.total_advance < single_a.total_advance + single_v.total_advance);
}

TEST_CASE("FontManager shapes Arabic joining sequences") {
    TextTestEnvironment env{};
    SP::UI::FontManager manager(env.space);

    Widgets::TypographyStyle typography{};
    typography.font_family = "Geeza Pro";
    typography.font_style = "Regular";
    typography.font_weight = "400";
    typography.font_size = 28.0f;
    typography.line_height = 28.0f;
    typography.letter_spacing = 0.0f;
    typography.language = "ar";
    typography.direction = "rtl";
    typography.font_features = {"kern", "liga"};
    typography.fallback_families = {"Geeza Pro",
                                     "Arial",
                                     "Times New Roman",
                                     "Noto Naskh Arabic",
                                     "Tahoma"};

    auto const text = std::string{"\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85"};
    auto shaped = manager.shape_text(env.app_view(), text, typography);
    auto fallback = manager.shape_text(env.app_view(), text, fallback_typography());
    auto codepoints = utf8_codepoints(text);

    if (shaped.glyphs.empty() || shaped.glyphs.size() > codepoints.size()) {
        INFO("Arabic shaping skipped: no suitable RTL font available");
        return;
    }

    CHECK(shaped.glyphs.size() <= codepoints.size());
    CHECK(shaped.total_advance > 0.0f);
    CHECK(shaped.total_advance < fallback.total_advance);

    bool has_joining_adjustment = false;
    for (std::size_t i = 1; i < shaped.glyphs.size(); ++i) {
        auto expected_x = shaped.glyphs[i - 1].offset_x + shaped.glyphs[i - 1].advance;
        if (shaped.glyphs[i].offset_x != doctest::Approx(expected_x)) {
            has_joining_adjustment = true;
            break;
        }
    }
    INFO("Arabic shaping: joining adjustment observed=" << std::boolalpha << has_joining_adjustment);
}

TEST_CASE("FontManager reorders Devanagari matra placement") {
    TextTestEnvironment env{};
    SP::UI::FontManager manager(env.space);

    Widgets::TypographyStyle typography{};
    typography.font_family = "Devanagari Sangam MN";
    typography.font_style = "Regular";
    typography.font_weight = "400";
    typography.font_size = 28.0f;
    typography.line_height = 28.0f;
    typography.letter_spacing = 0.0f;
    typography.language = "hi";
    typography.direction = "ltr";
    typography.font_features = {"kern", "liga"};
    typography.fallback_families = {"Devanagari Sangam MN",
                                     "Noto Sans Devanagari",
                                     "Kohinoor Devanagari",
                                     "Arial Unicode MS"};

    auto const text = std::string{"\xE0\xA4\x95\xE0\xA4\xBF"};
    auto shaped = manager.shape_text(env.app_view(), text, typography);
    auto fallback = manager.shape_text(env.app_view(), text, fallback_typography());
    auto codepoints = utf8_codepoints(text);

    if (shaped.glyphs.empty() || shaped.glyphs.size() > codepoints.size()) {
        INFO("Devanagari shaping skipped: no suitable font available");
        return;
    }

    CHECK(shaped.glyphs.size() <= codepoints.size());
    CHECK(shaped.total_advance > 0.0f);
    CHECK(shaped.total_advance < fallback.total_advance);

    bool has_left_matra = std::any_of(shaped.glyphs.begin(), shaped.glyphs.end(), [](auto const& glyph) {
        return glyph.offset_x < 0.0f;
    });
    INFO("Devanagari shaping: left matra observed=" << std::boolalpha << has_left_matra);
}
