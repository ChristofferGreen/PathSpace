#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <array>
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
};

struct StackDescriptor {
    std::string active_panel;
    std::vector<StackPanelDescriptor> panels;
};

struct InputFieldDescriptor {
    BuilderWidgets::TextFieldStyle style{};
    BuilderWidgets::TextFieldState state{};
};

struct PaintSurfaceDescriptor {
    float brush_size = 0.0f;
    std::array<float, 4> brush_color{1.0f, 1.0f, 1.0f, 1.0f};
    bool gpu_enabled = false;
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

[[nodiscard]] auto BuildWidgetBucket(WidgetDescriptor const& descriptor,
                                     DescriptorBucketOptions const& options = {})
    -> SP::Expected<SP::UI::Scene::DrawableBucketSnapshot>;

} // namespace SP::UI::Declarative
