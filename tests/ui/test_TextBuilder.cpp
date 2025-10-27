#include "third_party/doctest.h"

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
