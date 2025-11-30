#include "PaintControls.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace SP::Examples::PaintControls {
namespace {
constexpr int kButtonsPerRow = 3;

struct PaletteEntryMeta {
    const char* id;
    const char* label;
};

constexpr std::array<PaletteEntryMeta, 6> kDefaultPaletteMeta{ {
    {"paint_palette_red", "Red"},
    {"paint_palette_orange", "Orange"},
    {"paint_palette_yellow", "Yellow"},
    {"paint_palette_green", "Green"},
    {"paint_palette_blue", "Blue"},
    {"paint_palette_purple", "Purple"},
} };

auto palette_button_text_color(std::array<float, 4> const& background,
                               SP::UI::Runtime::Widgets::WidgetTheme const& theme)
    -> std::array<float, 4> {
    auto luminance = background[0] * 0.299f + background[1] * 0.587f + background[2] * 0.114f;
    if (luminance > 0.65f) {
        return theme.palette_text_on_light;
    }
    return theme.palette_text_on_dark;
}

} // namespace

auto ComputeLayoutMetrics(int window_width, int window_height) -> PaintLayoutMetrics {
    PaintLayoutMetrics metrics{};
    metrics.padding_x = 32.0f;
    metrics.padding_y = 32.0f;
    auto width_f = static_cast<float>(std::max(window_width, 800));
    auto height_f = static_cast<float>(std::max(window_height, 600));
    if (height_f >= 800.0f) {
        metrics.controls_scale = 1.0f;
    } else {
        metrics.controls_scale = std::clamp(height_f / 800.0f, 0.82f, 1.0f);
    }
    auto scale = metrics.controls_scale;
    metrics.controls_spacing = std::lerp(18.0f, 28.0f, scale);
    metrics.controls_section_spacing = std::lerp(20.0f, 30.0f, scale);
    metrics.controls_padding_main = std::lerp(18.0f, 26.0f, scale);
    metrics.controls_padding_cross = std::lerp(16.0f, 22.0f, scale);
    metrics.section_padding_main = std::lerp(8.0f, 12.0f, scale);
    metrics.section_padding_cross = std::lerp(10.0f, 16.0f, scale);
    metrics.status_block_spacing = std::lerp(6.0f, 10.0f, scale);
    metrics.palette_row_spacing = std::lerp(10.0f, 16.0f, scale);
    metrics.actions_row_spacing = std::lerp(10.0f, 16.0f, scale);
    metrics.palette_button_height = std::lerp(40.0f, 52.0f, scale);
    metrics.controls_width = std::clamp(width_f * 0.30f, 320.0f, 460.0f);
    auto column_width = std::max(metrics.controls_width - metrics.controls_padding_cross * 2.0f, 240.0f);
    metrics.controls_content_width = std::max(column_width - metrics.section_padding_cross * 2.0f, 220.0f);
    metrics.canvas_offset_x = metrics.padding_x + metrics.controls_width + metrics.controls_spacing;
    metrics.canvas_offset_y = metrics.padding_y;
    auto available_width = width_f - (metrics.canvas_offset_x + metrics.padding_x);
    metrics.canvas_width = std::max(640.0f, available_width);
    auto max_canvas_width = width_f - metrics.canvas_offset_x - metrics.padding_x;
    if (max_canvas_width > 0.0f) {
        metrics.canvas_width = std::min(metrics.canvas_width, max_canvas_width);
    } else {
        metrics.canvas_width = std::max(320.0f, metrics.canvas_width);
    }
    auto available_height = height_f - metrics.padding_y * 2.0f;
    metrics.canvas_height = std::max(520.0f, available_height);
    return metrics;
}

auto BuildDefaultPaletteEntries(SP::UI::Runtime::Widgets::WidgetTheme const& theme)
    -> std::vector<PaletteEntry> {
    std::vector<PaletteEntry> colors;
    colors.reserve(kDefaultPaletteMeta.size());
    for (std::size_t i = 0; i < kDefaultPaletteMeta.size(); ++i) {
        auto color = theme.palette_swatches[i];
        if (color[3] <= 0.0f) {
            color = SP::UI::Runtime::Widgets::kDefaultPaletteSwatches[i];
        }
        colors.push_back(PaletteEntry{
            kDefaultPaletteMeta[i].id,
            kDefaultPaletteMeta[i].label,
            color,
        });
    }
    return colors;
}

SP::UI::Runtime::Widgets::TypographyStyle MakeTypography(float font_size, float line_height) {
    SP::UI::Runtime::Widgets::TypographyStyle style{};
    style.font_size = font_size;
    style.line_height = line_height;
    style.letter_spacing = 0.0f;
    style.baseline_shift = 0.0f;
    return style;
}

void EnsureActivePanel(SP::UI::Declarative::Stack::Args& args) {
    if (!args.active_panel.empty() || args.panels.empty()) {
        return;
    }
    args.active_panel = args.panels.front().id;
}

auto BuildPaletteFragment(PaletteComponentConfig const& config)
    -> SP::UI::Declarative::WidgetFragment {
    SP::UI::Declarative::Stack::Args column{};
    column.style.axis = SP::UI::Runtime::Widgets::StackAxis::Vertical;
    auto vertical_spacing = std::max(config.layout.palette_row_spacing, 8.0f);
    column.style.spacing = vertical_spacing;
    column.style.align_cross = SP::UI::Runtime::Widgets::StackAlignCross::Stretch;
    auto column_width = std::max(config.layout.controls_content_width, 240.0f);
    column.style.width = column_width;

    int row_index = 0;
    for (std::size_t index = 0; index < config.entries.size();) {
        SP::UI::Declarative::Stack::Args row{};
        row.style.axis = SP::UI::Runtime::Widgets::StackAxis::Horizontal;
        row.style.spacing = std::max(10.0f, 14.0f * config.layout.controls_scale);
        row.style.align_cross = SP::UI::Runtime::Widgets::StackAlignCross::Stretch;
        auto total_spacing = row.style.spacing * static_cast<float>(kButtonsPerRow - 1);
        auto available_width = std::max(column_width - total_spacing,
                                        96.0f * static_cast<float>(kButtonsPerRow));
        auto base_width = std::max(96.0f, available_width / static_cast<float>(kButtonsPerRow));

        for (int col = 0; col < kButtonsPerRow && index < config.entries.size(); ++col, ++index) {
            auto entry = config.entries[index];
            SP::UI::Declarative::Button::Args args{};
            args.label = entry.label;
            args.style.width = base_width;
            args.style.height = config.layout.palette_button_height;
            args.style.corner_radius = std::max(6.0f, 10.0f * config.layout.controls_scale);
            auto overrides = args.style_override();
            overrides.background_color(entry.color)
                .text_color(palette_button_text_color(entry.color, config.theme))
                .typography(MakeTypography(19.0f * config.layout.controls_scale,
                                           24.0f * config.layout.controls_scale));
            args.on_press = [entry, handler = config.on_select, brush_state = config.brush_state](
                                SP::UI::Declarative::ButtonContext& ctx) {
                if (brush_state) {
                    brush_state->color = entry.color;
                }
                if (handler) {
                    handler(ctx, entry);
                }
            };
            row.panels.push_back(SP::UI::Declarative::Stack::Panel{
                .id = entry.id,
                .fragment = SP::UI::Declarative::Button::Fragment(std::move(args)),
            });
        }

        EnsureActivePanel(row);
        column.panels.push_back(SP::UI::Declarative::Stack::Panel{
            .id = std::string{"palette_row_"} + std::to_string(row_index++),
            .fragment = SP::UI::Declarative::Stack::Fragment(std::move(row)),
        });
    }

    EnsureActivePanel(column);
    return SP::UI::Declarative::Stack::Fragment(std::move(column));
}

auto BuildBrushSliderFragment(BrushSliderConfig const& config)
    -> SP::UI::Declarative::WidgetFragment {
    SP::UI::Declarative::Slider::Args slider{};
    slider.minimum = config.minimum;
    slider.maximum = config.maximum;
    slider.step = config.step;
    slider.value = config.brush_state ? config.brush_state->size : slider.minimum;
    slider.style.width = std::max(200.0f, config.layout.controls_content_width);
    slider.style.height = std::max(34.0f, 44.0f * config.layout.controls_scale);
    slider.style.track_height = std::max(7.0f, 9.0f * config.layout.controls_scale);
    slider.style.thumb_radius = std::max(9.0f, 12.0f * config.layout.controls_scale);
    slider.style_override()
        .label_color({0.84f, 0.88f, 0.94f, 1.0f})
        .label_typography(MakeTypography(19.0f * config.layout.controls_scale,
                                         24.0f * config.layout.controls_scale));
    slider.on_change = [handler = config.on_change, brush_state = config.brush_state](
                          SP::UI::Declarative::SliderContext& ctx) {
        if (brush_state) {
            brush_state->size = ctx.value;
        }
        if (handler) {
            handler(ctx, ctx.value);
        }
    };
    return SP::UI::Declarative::Slider::Fragment(slider);
}

auto BuildHistoryActionsFragment(HistoryActionsConfig const& config)
    -> SP::UI::Declarative::WidgetFragment {
    SP::UI::Declarative::Stack::Args row{};
    row.style.axis = SP::UI::Runtime::Widgets::StackAxis::Horizontal;
    row.style.spacing = std::max(config.layout.actions_row_spacing, 8.0f);
    row.style.align_cross = SP::UI::Runtime::Widgets::StackAlignCross::Stretch;
    row.style.padding_main_start = config.layout.section_padding_main;
    row.style.padding_main_end = config.layout.section_padding_main;
    row.style.padding_cross_start = config.layout.section_padding_cross;
    row.style.padding_cross_end = config.layout.section_padding_cross;
    row.style.width = config.layout.controls_content_width + config.layout.section_padding_cross * 2.0f;
    auto column_width = std::max(config.layout.controls_content_width, 240.0f);
    auto button_width = std::max(150.0f, (column_width - row.style.spacing) * 0.5f);

    auto build_button = [&](std::string id,
                            std::string label,
                            HistoryAction action) {
        SP::UI::Declarative::Button::Args args{};
        args.label = std::move(label);
        args.enabled = false;
        args.style.width = button_width;
        args.style.height = std::max(36.0f, 44.0f * config.layout.controls_scale);
        args.style.corner_radius = std::max(6.0f, 9.0f * config.layout.controls_scale);
        args.style_override().typography(MakeTypography(18.0f * config.layout.controls_scale,
                                                        22.0f * config.layout.controls_scale));
        args.on_press = [handler = config.on_action, action](SP::UI::Declarative::ButtonContext& ctx) {
            if (handler) {
                handler(ctx, action);
            }
        };
        row.panels.push_back(SP::UI::Declarative::Stack::Panel{
            .id = std::move(id),
            .fragment = SP::UI::Declarative::Button::Fragment(std::move(args)),
        });
    };

    build_button("undo_button", config.undo_label, HistoryAction::Undo);
    build_button("redo_button", config.redo_label, HistoryAction::Redo);
    EnsureActivePanel(row);
    return SP::UI::Declarative::Stack::Fragment(std::move(row));
}

} // namespace SP::Examples::PaintControls
