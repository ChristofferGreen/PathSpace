#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/ui/declarative/PaintSurfaceTypes.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace SP::UI::Declarative {

namespace BuilderWidgets = SP::UI::Builders::Widgets;

struct ButtonDescriptor {
    BuilderWidgets::ButtonStyle style{};
    BuilderWidgets::ButtonState state{};
    std::string label;
};

struct ToggleDescriptor {
    BuilderWidgets::ToggleStyle style{};
    BuilderWidgets::ToggleState state{};
};

struct SliderDescriptor {
    BuilderWidgets::SliderStyle style{};
    BuilderWidgets::SliderState state{};
    BuilderWidgets::SliderRange range{};
};

struct ListDescriptor {
    BuilderWidgets::ListStyle style{};
    BuilderWidgets::ListState state{};
    std::vector<BuilderWidgets::ListItem> items;
};

struct TreeDescriptor {
    BuilderWidgets::TreeStyle style{};
    BuilderWidgets::TreeState state{};
    std::vector<BuilderWidgets::TreeNode> nodes;
};

struct LabelDescriptor {
    std::string text;
    BuilderWidgets::TypographyStyle typography{};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct StackPanelDescriptor {
    std::string id;
    std::string target;
    bool visible = false;
};

struct StackDescriptor {
    std::string active_panel;
    BuilderWidgets::StackLayoutStyle style{};
    BuilderWidgets::StackLayoutState layout{};
    std::vector<BuilderWidgets::StackChildSpec> children;
    std::vector<StackPanelDescriptor> panels;
};

struct InputFieldDescriptor {
    BuilderWidgets::TextFieldStyle style{};
    BuilderWidgets::TextFieldState state{};
};

struct PaintSurfaceStrokeDescriptor {
    std::uint64_t id = 0;
    PaintStrokeMeta meta{};
    std::vector<PaintStrokePoint> points;
};

struct PaintSurfaceDescriptor {
    float brush_size = 0.0f;
    std::array<float, 4> brush_color{1.0f, 1.0f, 1.0f, 1.0f};
    bool gpu_enabled = false;
    bool gpu_ready = false;
    PaintBufferMetrics buffer{};
    PaintBufferViewport viewport{};
    std::uint64_t buffer_revision = 0;
    std::vector<Builders::DirtyRectHint> pending_dirty;
    std::optional<PaintTexturePayload> texture;
    PaintGpuStats gpu_stats{};
    std::vector<PaintSurfaceStrokeDescriptor> strokes;
};

struct WidgetDescriptor {
    WidgetKind kind = WidgetKind::Button;
    SP::UI::Builders::WidgetPath widget;
    std::variant<ButtonDescriptor,
                 ToggleDescriptor,
                 SliderDescriptor,
                 ListDescriptor,
                 TreeDescriptor,
                 LabelDescriptor,
                 StackDescriptor,
                 InputFieldDescriptor,
                 PaintSurfaceDescriptor> data;
};

struct DescriptorBucketOptions {
    bool pulsing_highlight = true;
};

[[nodiscard]] auto LoadWidgetDescriptor(PathSpace& space,
                                        SP::UI::Builders::WidgetPath const& widget)
    -> SP::Expected<WidgetDescriptor>;

[[nodiscard]] auto BuildWidgetBucket(PathSpace& space,
                                     WidgetDescriptor const& descriptor,
                                     DescriptorBucketOptions const& options = {})
    -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot>;

} // namespace SP::UI::Declarative
