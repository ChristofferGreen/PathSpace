#pragma once

#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <array>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace SP::Examples::PaintControls {

struct PaintLayoutMetrics {
    float controls_width = 0.0f;
    float controls_spacing = 0.0f;
    float padding_x = 0.0f;
    float padding_y = 0.0f;
    float controls_padding_main = 0.0f;
    float controls_padding_cross = 0.0f;
    float palette_button_height = 0.0f;
    float canvas_width = 0.0f;
    float canvas_height = 0.0f;
    float canvas_offset_x = 0.0f;
    float canvas_offset_y = 0.0f;
    float controls_scale = 1.0f;
};

struct BrushState {
    float size = 12.0f;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct PaletteEntry {
    std::string id;
    std::string label;
    std::array<float, 4> color;
};

struct PaletteComponentConfig {
    PaintLayoutMetrics const& layout;
    SP::UI::Builders::Widgets::WidgetTheme const& theme;
    std::span<const PaletteEntry> entries;
    std::shared_ptr<BrushState> brush_state;
    std::function<void(SP::UI::Declarative::ButtonContext&, PaletteEntry const&)> on_select;
};

struct BrushSliderConfig {
    PaintLayoutMetrics const& layout;
    std::shared_ptr<BrushState> brush_state;
    float minimum = 1.0f;
    float maximum = 64.0f;
    float step = 1.0f;
    std::function<void(SP::UI::Declarative::SliderContext&, float)> on_change;
};

enum class HistoryAction {
    Undo,
    Redo,
};

struct HistoryActionsConfig {
    PaintLayoutMetrics const& layout;
    std::function<void(SP::UI::Declarative::ButtonContext&, HistoryAction)> on_action;
    std::string undo_label = "Undo Stroke";
    std::string redo_label = "Redo Stroke";
};

auto ComputeLayoutMetrics(int window_width, int window_height) -> PaintLayoutMetrics;

auto BuildDefaultPaletteEntries(SP::UI::Builders::Widgets::WidgetTheme const& theme)
    -> std::vector<PaletteEntry>;

auto BuildPaletteFragment(PaletteComponentConfig const& config)
    -> SP::UI::Declarative::WidgetFragment;

auto BuildBrushSliderFragment(BrushSliderConfig const& config)
    -> SP::UI::Declarative::WidgetFragment;

auto BuildHistoryActionsFragment(HistoryActionsConfig const& config)
    -> SP::UI::Declarative::WidgetFragment;

SP::UI::Builders::Widgets::TypographyStyle MakeTypography(float font_size, float line_height);
void EnsureActivePanel(SP::UI::Declarative::Stack::Args& args);

} // namespace SP::Examples::PaintControls
