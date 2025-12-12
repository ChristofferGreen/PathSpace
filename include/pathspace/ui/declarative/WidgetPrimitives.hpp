#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace SP::UI::Declarative::Primitives {

enum class WidgetPrimitiveKind : std::uint8_t {
    Surface,
    Text,
    Icon,
    BoxLayout,
    Behavior,
};

enum class LayoutAxis : std::uint8_t {
    Horizontal,
    Vertical,
};

enum class LayoutDistribution : std::uint8_t {
    Even,
    Weighted,
    Intrinsic,
};

struct BoxLayoutPrimitive {
    LayoutAxis axis = LayoutAxis::Vertical;
    LayoutDistribution distribution = LayoutDistribution::Intrinsic;
    float spacing = 0.0f;
    std::array<float, 4> padding{0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<float> weights;
    bool stretch_children = false;
};

enum class SurfaceShape : std::uint8_t {
    Rectangle,
    RoundedRect,
};

struct SurfacePrimitive {
    SurfaceShape shape = SurfaceShape::RoundedRect;
    std::array<float, 4> fill_color{0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> border_color{0.0f, 0.0f, 0.0f, 0.0f};
    float border_width = 0.0f;
    float corner_radius = 0.0f;
    bool clip_children = false;
};

struct TextPrimitive {
    std::string text;
    std::string text_path;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    SP::UI::Runtime::Widgets::TypographyStyle typography{};
};

struct IconPrimitive {
    std::string glyph;
    std::string atlas_path;
    float size = 0.0f;
};

enum class BehaviorKind : std::uint8_t {
    Clickable,
    Toggle,
    Focusable,
    Scroll,
    Input,
};

struct BehaviorPrimitive {
    BehaviorKind kind = BehaviorKind::Clickable;
    std::vector<std::string> topics;
};

struct WidgetPrimitive {
    std::string id;
    WidgetPrimitiveKind kind = WidgetPrimitiveKind::Surface;
    std::vector<std::string> children;
    std::variant<SurfacePrimitive,
                 TextPrimitive,
                 IconPrimitive,
                 BoxLayoutPrimitive,
                 BehaviorPrimitive>
        data;
};

struct WidgetPrimitiveIndex {
    std::vector<std::string> roots;
};

[[nodiscard]] auto WritePrimitives(PathSpace& space,
                                   std::string const& widget_root,
                                   std::vector<WidgetPrimitive> const& primitives,
                                   WidgetPrimitiveIndex const& index) -> SP::Expected<void>;

} // namespace SP::UI::Declarative::Primitives

