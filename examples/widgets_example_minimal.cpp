#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/ui/DrawCommands.hpp>
#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/ui/FontManager.hpp>

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(PATHSPACE_ENABLE_UI)
int main() {
    std::cerr << "widgets_example_minimal requires PATHSPACE_ENABLE_UI=ON.\n";
    return 1;
}
#elif !defined(__APPLE__)
int main(int argc, char** argv) {
    std::cerr << "widgets_example_minimal currently supports only macOS builds.\n";
    return 1;
}
#else

using namespace SP;
using namespace SP::UI;
namespace SceneData = SP::UI::Scene;
namespace SceneBuilders = SP::UI::Builders::Scene;
namespace AppBuilders = SP::UI::Builders::App;
namespace WindowBuilders = SP::UI::Builders::Window;
namespace Widgets = SP::UI::Builders::Widgets;
namespace Builders = SP::UI::Builders;
using Builders::AppRootPathView;
namespace WidgetInput = SP::UI::Builders::Widgets::Input;
namespace WidgetFocus = SP::UI::Builders::Widgets::Focus;
namespace WidgetBindings = SP::UI::Builders::Widgets::Bindings;
namespace TextBuilder = SP::UI::Builders::Text;
using ScenePath = SP::UI::Builders::ScenePath;
using SceneParams = SP::UI::Builders::SceneParams;
using PixelFormat = SP::UI::Builders::PixelFormat;
using ColorSpace = SP::UI::Builders::ColorSpace;

namespace {

constexpr float kMargin = 48.0f;
constexpr float kSpacing = 36.0f;

template <typename T>
auto unwrap_or_exit(SP::Expected<T> value, std::string const& context) -> T {
    if (!value) {
        std::cerr << context;
        if (value.error().message.has_value()) {
            std::cerr << ": " << *value.error().message;
        }
        std::cerr << std::endl;
        std::exit(1);
    }
    return *std::move(value);
}

inline auto unwrap_or_exit(SP::Expected<void> value, std::string const& context) -> void {
    if (!value) {
        std::cerr << context;
        if (value.error().message.has_value()) {
            std::cerr << ": " << *value.error().message;
        }
        std::cerr << std::endl;
        std::exit(1);
    }
}

struct DemoFontConfig {
    std::string family;
    std::string style;
    std::string weight;
    std::vector<std::string> fallback;
    std::uint64_t revision = 0;
};

auto register_font_or_exit(FontManager& manager,
                            AppRootPathView appRoot,
                            DemoFontConfig const& config)
    -> FontManager::ResolvedFont {
    Builders::Resources::Fonts::RegisterFontParams params{
        .family = config.family,
        .style = config.style,
        .weight = config.weight,
        .fallback_families = config.fallback,
        .initial_revision = config.revision,
    };
    auto context = std::string{"register font "} + config.family + "/" + config.style;
    (void)unwrap_or_exit(manager.register_font(appRoot, params), context);
    auto resolved = manager.resolve_font(appRoot, config.family, config.style);
    return unwrap_or_exit(std::move(resolved), context + " resolve");
}

void apply_font_to_typography(Widgets::TypographyStyle& typography,
                              FontManager::ResolvedFont const& resolved) {
    typography.font_family = resolved.family;
    typography.font_style = resolved.style;
    typography.font_weight = resolved.weight;
    typography.font_resource_root = resolved.paths.root.getPath();
    typography.font_active_revision = resolved.active_revision;
    typography.fallback_families = resolved.fallback_chain;
    typography.font_features = {"kern", "liga"};
    typography.font_asset_fingerprint = 0;
}

void attach_demo_fonts(PathSpace& space,
                       AppRootPathView appRoot,
                       Widgets::WidgetTheme& theme) {
    FontManager manager(space);

    DemoFontConfig regular{
        .family = "PathSpaceSans",
        .style = "Regular",
        .weight = "400",
        .fallback = {"system-ui"},
        .revision = 1ull,
    };

    DemoFontConfig semibold{
        .family = "PathSpaceSans",
        .style = "SemiBold",
        .weight = "600",
        .fallback = {"PathSpaceSans", "system-ui"},
        .revision = 2ull,
    };

    auto regular_font = register_font_or_exit(manager, appRoot, regular);
    auto semibold_font = register_font_or_exit(manager, appRoot, semibold);

    auto apply_regular = [&](Widgets::TypographyStyle& typography) {
        apply_font_to_typography(typography, regular_font);
    };
    auto apply_semibold = [&](Widgets::TypographyStyle& typography) {
        apply_font_to_typography(typography, semibold_font);
    };

    apply_semibold(theme.heading);
    apply_regular(theme.caption);
    apply_semibold(theme.button.typography);
    apply_regular(theme.slider.label_typography);
    apply_regular(theme.list.item_typography);
    apply_regular(theme.tree.label_typography);
}

auto make_identity_transform() -> SceneData::Transform {
    SceneData::Transform transform{};
    for (int index = 0; index < 16; ++index) {
        transform.elements[index] = (index % 5 == 0) ? 1.0f : 0.0f;
    }
    return transform;
}

auto mix_color(std::array<float, 4> base,
               std::array<float, 4> target,
               float amount) -> std::array<float, 4> {
    amount = std::clamp(amount, 0.0f, 1.0f);
    std::array<float, 4> out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = std::clamp(base[i] * (1.0f - amount) + target[i] * amount, 0.0f, 1.0f);
    }
    out[3] = std::clamp(base[3], 0.0f, 1.0f);
    return out;
}

auto lighten(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    return mix_color(color, {1.0f, 1.0f, 1.0f, color[3]}, amount);
}

auto desaturate(std::array<float, 4> color, float amount) -> std::array<float, 4> {
    auto gray = std::array<float, 4>{0.5f, 0.5f, 0.5f, color[3]};
    return mix_color(color, gray, amount);
}

template <typename Cmd>
auto read_command(std::vector<std::uint8_t> const& payload, std::size_t offset) -> Cmd {
    Cmd command{};
    std::memcpy(&command, payload.data() + offset, sizeof(Cmd));
    return command;
}

template <typename Cmd>
auto write_command(std::vector<std::uint8_t>& payload, std::size_t offset, Cmd const& command) -> void {
    std::memcpy(payload.data() + offset, &command, sizeof(Cmd));
}

auto translate_bucket(SceneData::DrawableBucketSnapshot& bucket, float dx, float dy) -> void {
    for (auto& sphere : bucket.bounds_spheres) {
        sphere.center[0] += dx;
        sphere.center[1] += dy;
    }
    for (auto& box : bucket.bounds_boxes) {
        box.min[0] += dx;
        box.max[0] += dx;
        box.min[1] += dy;
        box.max[1] += dy;
    }
    std::size_t payload_offset = 0;
    for (auto kind_value : bucket.command_kinds) {
        auto kind = static_cast<SceneData::DrawCommandKind>(kind_value);
        switch (kind) {
        case SceneData::DrawCommandKind::Rect: {
            auto command = read_command<SceneData::RectCommand>(bucket.command_payload, payload_offset);
            command.min_x += dx;
            command.max_x += dx;
            command.min_y += dy;
            command.max_y += dy;
            write_command(bucket.command_payload, payload_offset, command);
            break;
        }
        case SceneData::DrawCommandKind::RoundedRect: {
            auto command = read_command<SceneData::RoundedRectCommand>(bucket.command_payload, payload_offset);
            command.min_x += dx;
            command.max_x += dx;
            command.min_y += dy;
            command.max_y += dy;
            write_command(bucket.command_payload, payload_offset, command);
            break;
        }
        case SceneData::DrawCommandKind::TextGlyphs: {
            auto command = read_command<SceneData::TextGlyphsCommand>(bucket.command_payload, payload_offset);
            command.min_x += dx;
            command.max_x += dx;
            command.min_y += dy;
            command.max_y += dy;
            write_command(bucket.command_payload, payload_offset, command);
            break;
        }
        default:
            break;
        }
        payload_offset += SceneData::payload_size_bytes(kind);
    }
}

auto append_bucket(SceneData::DrawableBucketSnapshot& dest,
                   SceneData::DrawableBucketSnapshot const& src) -> void {
    if (src.drawable_ids.empty()) {
        return;
    }

    auto drawable_base = static_cast<std::uint32_t>(dest.drawable_ids.size());
    auto command_base = static_cast<std::uint32_t>(dest.command_kinds.size());
    auto clip_base = static_cast<std::int32_t>(dest.clip_nodes.size());

    dest.drawable_ids.insert(dest.drawable_ids.end(), src.drawable_ids.begin(), src.drawable_ids.end());
    dest.world_transforms.insert(dest.world_transforms.end(), src.world_transforms.begin(), src.world_transforms.end());
    dest.bounds_spheres.insert(dest.bounds_spheres.end(), src.bounds_spheres.begin(), src.bounds_spheres.end());
    dest.bounds_boxes.insert(dest.bounds_boxes.end(), src.bounds_boxes.begin(), src.bounds_boxes.end());
    dest.bounds_box_valid.insert(dest.bounds_box_valid.end(), src.bounds_box_valid.begin(), src.bounds_box_valid.end());
    dest.layers.insert(dest.layers.end(), src.layers.begin(), src.layers.end());
    dest.z_values.insert(dest.z_values.end(), src.z_values.begin(), src.z_values.end());
    dest.material_ids.insert(dest.material_ids.end(), src.material_ids.begin(), src.material_ids.end());
    dest.pipeline_flags.insert(dest.pipeline_flags.end(), src.pipeline_flags.begin(), src.pipeline_flags.end());
    dest.visibility.insert(dest.visibility.end(), src.visibility.begin(), src.visibility.end());

    for (auto offset : src.command_offsets) {
        dest.command_offsets.push_back(offset + command_base);
    }
    dest.command_counts.insert(dest.command_counts.end(), src.command_counts.begin(), src.command_counts.end());

    dest.command_kinds.insert(dest.command_kinds.end(), src.command_kinds.begin(), src.command_kinds.end());
    dest.command_payload.insert(dest.command_payload.end(), src.command_payload.begin(), src.command_payload.end());

    for (auto index : src.opaque_indices) {
        dest.opaque_indices.push_back(index + drawable_base);
    }
    for (auto index : src.alpha_indices) {
        dest.alpha_indices.push_back(index + drawable_base);
    }

    for (auto const& entry : src.layer_indices) {
        SceneData::LayerIndices adjusted{entry.layer, {}};
        adjusted.indices.reserve(entry.indices.size());
        for (auto idx : entry.indices) {
            adjusted.indices.push_back(idx + drawable_base);
        }
        dest.layer_indices.push_back(std::move(adjusted));
    }

    for (auto node : src.clip_nodes) {
        if (node.next >= 0) {
            node.next += clip_base;
        }
        dest.clip_nodes.push_back(node);
    }
    for (auto head : src.clip_head_indices) {
        dest.clip_head_indices.push_back(head >= 0 ? head + clip_base : -1);
    }

    dest.authoring_map.insert(dest.authoring_map.end(), src.authoring_map.begin(), src.authoring_map.end());
    dest.drawable_fingerprints.insert(dest.drawable_fingerprints.end(),
                                      src.drawable_fingerprints.begin(),
                                      src.drawable_fingerprints.end());
}

auto make_background_bucket(float width, float height) -> SceneData::DrawableBucketSnapshot {
    SceneData::DrawableBucketSnapshot bucket{};
    auto drawable_id = 0x9000FFF0ull;
    bucket.drawable_ids.push_back(drawable_id);
    bucket.world_transforms.push_back(make_identity_transform());

    SceneData::BoundingBox box{};
    box.min = {0.0f, 0.0f, 0.0f};
    box.max = {width, height, 0.0f};
    bucket.bounds_boxes.push_back(box);
    bucket.bounds_box_valid.push_back(1);

    SceneData::BoundingSphere sphere{};
    sphere.center = {width * 0.5f, height * 0.5f, 0.0f};
    sphere.radius = std::sqrt(sphere.center[0] * sphere.center[0]
                              + sphere.center[1] * sphere.center[1]);
    bucket.bounds_spheres.push_back(sphere);

    bucket.layers.push_back(0);
    bucket.z_values.push_back(0.0f);
    bucket.material_ids.push_back(0);
    bucket.pipeline_flags.push_back(0);
    bucket.visibility.push_back(1);
    bucket.command_offsets.push_back(0);
    bucket.command_counts.push_back(1);
    bucket.opaque_indices.push_back(0);
    bucket.clip_head_indices.push_back(-1);

    SceneData::RectCommand rect{};
    rect.min_x = 0.0f;
    rect.min_y = 0.0f;
    rect.max_x = width;
    rect.max_y = height;
    rect.color = {0.11f, 0.12f, 0.15f, 1.0f};

    bucket.command_kinds.push_back(static_cast<std::uint32_t>(SceneData::DrawCommandKind::Rect));
    bucket.command_payload.resize(sizeof(SceneData::RectCommand));
    std::memcpy(bucket.command_payload.data(), &rect, sizeof(SceneData::RectCommand));

    bucket.authoring_map.push_back(SceneData::DrawableAuthoringMapEntry{
        drawable_id, "widgets/minimal/background", 0, 0});
    bucket.drawable_fingerprints.push_back(drawable_id);
    return bucket;
}

struct MinimalSceneBuild {
    SceneData::DrawableBucketSnapshot bucket;
    WidgetInput::LayoutSnapshot layout;
    int width = 0;
    int height = 0;
};

auto build_minimal_bucket(Widgets::WidgetTheme const& theme,
                          Widgets::ButtonStyle const& button_style,
                          std::string const& button_label,
                          Widgets::ButtonState const& button_state,
                          Widgets::ToggleStyle const& toggle_style,
                          Widgets::ToggleState const& toggle_state,
                          Widgets::SliderStyle const& slider_style,
                          Widgets::SliderState const& slider_state,
                          Widgets::SliderRange const& slider_range,
                          Widgets::ListStyle const& list_style,
                          std::vector<Widgets::ListItem> const& list_items,
                          Widgets::ListState const& list_state,
                          Widgets::TreeStyle const& tree_style,
                          std::vector<Widgets::TreeNode> const& tree_nodes,
                          Widgets::TreeState const& tree_state) -> MinimalSceneBuild {
    MinimalSceneBuild result{};
    SceneData::DrawableBucketSnapshot content{};

    auto& layout = result.layout;
    layout = WidgetInput::LayoutSnapshot{};

    float cursor_y = kMargin;
    float max_width = 0.0f;

    // Title label
    {
        Widgets::TypographyStyle heading = theme.heading;
        float baseline = cursor_y + heading.baseline_shift;
        auto label = Widgets::BuildLabel(
            Widgets::LabelBuildParams::Make("Widgets Minimal", heading)
                .WithOrigin(kMargin, baseline)
                .WithColor(theme.heading_color)
                .WithDrawable(0x10000001ull, "widgets/minimal/title", 0.05f));
        if (label) {
            append_bucket(content, label->bucket);
            max_width = std::max(max_width, kMargin + label->width);
            cursor_y += heading.line_height + kSpacing * 0.5f;
        }
    }

    // Button bucket
    {
        auto button_bucket = Widgets::BuildButtonPreview(
            button_style,
            button_state,
            Widgets::ButtonPreviewOptions{
                .authoring_root = "widgets/minimal/button",
                .pulsing_highlight = button_state.focused,
            });
        float button_width = std::max(button_style.width, 1.0f);
        float button_height = std::max(button_style.height, 1.0f);
        float button_x = kMargin;
        float button_y = cursor_y;

        translate_bucket(button_bucket, button_x, button_y);
        append_bucket(content, button_bucket);

        layout.button = WidgetInput::WidgetBounds{
            button_x,
            button_y,
            button_x + button_width,
            button_y + button_height,
        };

        std::optional<WidgetInput::WidgetBounds> in_button_label_bounds;
        float interior_label_width = TextBuilder::MeasureTextWidth(button_label, button_style.typography);
        float interior_label_height = button_style.typography.line_height;
        float interior_label_x = button_x + std::max(0.0f, (button_width - interior_label_width) * 0.5f);
        float interior_label_top = button_y + std::max(0.0f, (button_height - interior_label_height) * 0.5f);
        float interior_label_baseline = interior_label_top + button_style.typography.baseline_shift;
        if (auto interior_label = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make(button_label, button_style.typography)
                    .WithOrigin(interior_label_x, interior_label_baseline)
                    .WithColor(button_style.text_color)
                    .WithDrawable(0x10000010ull, "widgets/minimal/button/label", 0.06f))) {
            in_button_label_bounds = Widgets::LabelBounds(*interior_label);
            if (!in_button_label_bounds) {
                in_button_label_bounds = WidgetInput::WidgetBounds{
                    interior_label_x,
                    interior_label_top,
                    interior_label_x + interior_label->width,
                    interior_label_top + interior_label_height,
                };
            }
            append_bucket(content, interior_label->bucket);
            max_width = std::max(max_width, interior_label_x + interior_label->width);
        }

        layout.button_footprint = layout.button;
        if (in_button_label_bounds) {
            layout.button_footprint.include(*in_button_label_bounds);
        }
        layout.button_footprint.normalize();
        WidgetInput::ExpandForFocusHighlight(layout.button_footprint);

        cursor_y = layout.button.max_y + kSpacing * 0.75f;
        max_width = std::max(max_width, layout.button.max_x);

        // Button caption label
        Widgets::TypographyStyle caption = theme.caption;
        float baseline = layout.button.max_y + caption.baseline_shift + 8.0f;
        auto caption_label = Widgets::BuildLabel(
            Widgets::LabelBuildParams::Make(button_label, caption)
                .WithOrigin(button_x, baseline)
                .WithColor(theme.caption_color)
                .WithDrawable(0x10000002ull, "widgets/minimal/button_caption", 0.05f));
        if (caption_label) {
            append_bucket(content, caption_label->bucket);
            max_width = std::max(max_width, button_x + caption_label->width);
            cursor_y = baseline + caption.line_height + kSpacing * 0.5f;
        }
    }

    // Toggle bucket
    {
        Widgets::TypographyStyle toggle_caption_typography = theme.caption;
        float toggle_caption_baseline = cursor_y + toggle_caption_typography.baseline_shift;
        if (auto toggle_label = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make("Toggle", toggle_caption_typography)
                    .WithOrigin(kMargin, toggle_caption_baseline)
                    .WithColor(theme.caption_color)
                    .WithDrawable(0x10000003ull, "widgets/minimal/toggle_label", 0.05f))) {
            append_bucket(content, toggle_label->bucket);
            max_width = std::max(max_width, kMargin + toggle_label->width);
            cursor_y = toggle_caption_baseline + toggle_caption_typography.line_height + 8.0f;
        }

        auto toggle_bucket = Widgets::BuildTogglePreview(
            toggle_style,
            toggle_state,
            Widgets::TogglePreviewOptions{
                .authoring_root = "widgets/minimal/toggle",
                .pulsing_highlight = toggle_state.focused,
            });

        float toggle_width = std::max(toggle_style.width, 1.0f);
        float toggle_height = std::max(toggle_style.height, 16.0f);
        float toggle_x = kMargin;
        float toggle_y = cursor_y;

        translate_bucket(toggle_bucket, toggle_x, toggle_y);
        append_bucket(content, toggle_bucket);

        layout.toggle = WidgetInput::WidgetBounds{
            toggle_x,
            toggle_y,
            toggle_x + toggle_width,
            toggle_y + toggle_height,
        };
        layout.toggle_footprint = layout.toggle;
        WidgetInput::ExpandForFocusHighlight(layout.toggle_footprint);

        cursor_y = layout.toggle.max_y + kSpacing;
        max_width = std::max(max_width, layout.toggle.max_x);
    }

    // Slider bucket
    {
        Widgets::TypographyStyle slider_caption_typography = theme.caption;
        float slider_caption_baseline = cursor_y + slider_caption_typography.baseline_shift;
        if (auto slider_label = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make("Slider", slider_caption_typography)
                    .WithOrigin(kMargin, slider_caption_baseline)
                    .WithColor(theme.caption_color)
                    .WithDrawable(0x10000004ull, "widgets/minimal/slider_label", 0.05f))) {
            append_bucket(content, slider_label->bucket);
            max_width = std::max(max_width, kMargin + slider_label->width);
            cursor_y = slider_caption_baseline + slider_caption_typography.line_height + 8.0f;
        }

        auto slider_bucket = Widgets::BuildSliderPreview(slider_style,
                                                         slider_range,
                                                         slider_state,
                                                         Widgets::SliderPreviewOptions{
                                                             .authoring_root = "widgets/minimal/slider",
                                                             .pulsing_highlight = slider_state.focused,
                                                         });

        float slider_width = std::max(slider_style.width, 1.0f);
        float slider_height = std::max(slider_style.height, 16.0f);
        float slider_x = kMargin;
        float slider_y = cursor_y;

        translate_bucket(slider_bucket, slider_x, slider_y);
        append_bucket(content, slider_bucket);

        WidgetInput::SliderLayout slider_layout{};
        slider_layout.bounds = WidgetInput::WidgetBounds{
            slider_x,
            slider_y,
            slider_x + slider_width,
            slider_y + slider_height,
        };
        float track_height = std::clamp(slider_style.track_height, 1.0f, slider_height);
        float track_half = track_height * 0.5f;
        float track_center = slider_y + slider_height * 0.5f;
        slider_layout.track = WidgetInput::WidgetBounds{
            slider_x,
            track_center - track_half,
            slider_x + slider_width,
            track_center + track_half,
        };
        layout.slider = slider_layout;

        layout.slider_footprint = slider_layout.bounds;
        WidgetInput::ExpandForFocusHighlight(layout.slider_footprint);

        cursor_y = slider_layout.bounds.max_y + kSpacing;
        max_width = std::max(max_width, slider_layout.bounds.max_x);
    }

    // List bucket
    {
        Widgets::TypographyStyle list_caption_typography = theme.caption;
        float list_caption_baseline = cursor_y + list_caption_typography.baseline_shift;
        if (auto list_caption = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make("Inventory", list_caption_typography)
                    .WithOrigin(kMargin, list_caption_baseline)
                    .WithColor(theme.caption_color)
                    .WithDrawable(0x10000005ull, "widgets/minimal/list_label", 0.05f))) {
            append_bucket(content, list_caption->bucket);
            max_width = std::max(max_width, kMargin + list_caption->width);
            cursor_y = list_caption_baseline + list_caption_typography.line_height + 8.0f;
        }

        auto list_preview = Widgets::BuildListPreview(list_style,
                                                      list_items,
                                                      list_state,
                                                      Widgets::ListPreviewOptions{
                                                          .authoring_root = "widgets/minimal/list",
                                                          .label_inset = 20.0f,
                                                          .pulsing_highlight = list_state.focused,
                                                      });
        auto list_bucket = list_preview.bucket;

        float list_width = list_preview.layout.bounds.max_x - list_preview.layout.bounds.min_x;
        float list_height = list_preview.layout.bounds.max_y - list_preview.layout.bounds.min_y;
        float list_x = kMargin;
        float list_y = cursor_y;

        translate_bucket(list_bucket, list_x, list_y);
        append_bucket(content, list_bucket);

        layout.list_footprint = WidgetInput::BoundsFromRect(list_preview.layout.bounds);
        layout.list_footprint.min_x += list_x;
        layout.list_footprint.max_x += list_x;
        layout.list_footprint.min_y += list_y;
        layout.list_footprint.max_y += list_y;
        WidgetInput::ExpandForFocusHighlight(layout.list_footprint);

        if (auto list_layout = WidgetInput::MakeListLayout(list_preview.layout)) {
            list_layout->bounds.min_x += list_x;
            list_layout->bounds.max_x += list_x;
            list_layout->bounds.min_y += list_y;
            list_layout->bounds.max_y += list_y;
            for (auto& bounds : list_layout->item_bounds) {
                bounds.min_x += list_x;
                bounds.max_x += list_x;
                bounds.min_y += list_y;
                bounds.max_y += list_y;
            }
            layout.list = *list_layout;
        } else {
            layout.list.reset();
        }

        cursor_y = list_y + list_height + kSpacing;
        max_width = std::max(max_width, list_x + list_width);

        auto const& sanitized_style = list_preview.layout.style;
        std::size_t label_count = std::min(list_preview.layout.rows.size(), list_items.size());
        for (std::size_t index = 0; index < label_count; ++index) {
            auto const& row = list_preview.layout.rows[index];
            auto const& item = list_items[index];
            float label_x = list_x + row.label_bounds.min_x;
            float label_top = list_y + row.label_bounds.min_y;
            float label_baseline = list_y + row.label_baseline;
            auto label = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make(item.label, sanitized_style.item_typography)
                    .WithOrigin(label_x, label_baseline)
                    .WithColor(sanitized_style.item_text_color)
                    .WithDrawable(0x10010000ull + static_cast<std::uint64_t>(index),
                                   "widgets/minimal/list/item/" + row.id,
                                   0.65f));
            if (label) {
                append_bucket(content, label->bucket);
                max_width = std::max(max_width, label_x + label->width);
                cursor_y = std::max(cursor_y,
                                     label_top + sanitized_style.item_typography.line_height + kSpacing);
            }
        }
    }

    // Tree bucket
    {
        Widgets::TypographyStyle tree_caption_typography = theme.caption;
        float tree_caption_baseline = cursor_y + tree_caption_typography.baseline_shift;
        if (auto tree_caption = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make("Workspace", tree_caption_typography)
                    .WithOrigin(kMargin, tree_caption_baseline)
                    .WithColor(theme.caption_color)
                    .WithDrawable(0x10000006ull, "widgets/minimal/tree_label", 0.05f))) {
            append_bucket(content, tree_caption->bucket);
            max_width = std::max(max_width, kMargin + tree_caption->width);
            cursor_y = tree_caption_baseline + tree_caption_typography.line_height + 8.0f;
        }

        auto tree_preview = Widgets::BuildTreePreview(tree_style,
                                                      tree_nodes,
                                                      tree_state,
                                                      Widgets::TreePreviewOptions{
                                                          .authoring_root = "widgets/minimal/tree",
                                                          .pulsing_highlight = tree_state.focused,
                                                      });
        auto tree_bucket = tree_preview.bucket;

        float tree_width = tree_preview.layout.bounds.max_x - tree_preview.layout.bounds.min_x;
        float tree_height = tree_preview.layout.bounds.max_y - tree_preview.layout.bounds.min_y;
        float tree_x = kMargin;
        float tree_y = cursor_y;

        translate_bucket(tree_bucket, tree_x, tree_y);
        append_bucket(content, tree_bucket);

        layout.tree_footprint = WidgetInput::BoundsFromRect(tree_preview.layout.bounds);
        layout.tree_footprint.min_x += tree_x;
        layout.tree_footprint.max_x += tree_x;
        layout.tree_footprint.min_y += tree_y;
        layout.tree_footprint.max_y += tree_y;
        WidgetInput::ExpandForFocusHighlight(layout.tree_footprint);

        if (auto tree_layout = WidgetInput::MakeTreeLayout(tree_preview.layout)) {
            WidgetInput::TranslateTreeLayout(*tree_layout, tree_x, tree_y);
            layout.tree = *tree_layout;
        } else {
            layout.tree.reset();
        }

        cursor_y = tree_y + tree_height + kMargin;
        max_width = std::max(max_width, tree_x + tree_width);

        auto const& rows = tree_preview.layout.rows;
        auto const& sanitized_style = tree_preview.layout.style;
        for (std::size_t index = 0; index < rows.size(); ++index) {
            auto const& row = rows[index];
            float row_top = tree_y + row.row_bounds.min_y;
            float row_height_local = row.row_bounds.max_y - row.row_bounds.min_y;
            float label_height = sanitized_style.label_typography.line_height;
            float text_top = row_top + std::max(0.0f, (row_height_local - label_height) * 0.5f);
            float toggle_right = tree_x + row.toggle_bounds.max_x;
            float label_x = toggle_right + 8.0f;
            float label_baseline = text_top + sanitized_style.label_typography.baseline_shift;
            auto text = row.label.empty() ? std::string("(node)") : row.label;
            auto color = sanitized_style.text_color;
            if (!row.enabled || !tree_state.enabled) {
                color = desaturate(color, 0.4f);
            }
            if (row.loading) {
                color = lighten(color, 0.15f);
            }
            auto label = Widgets::BuildLabel(
                Widgets::LabelBuildParams::Make(text, sanitized_style.label_typography)
                    .WithOrigin(label_x, label_baseline)
                    .WithColor(color)
                    .WithDrawable(0x10020000ull + static_cast<std::uint64_t>(index),
                                   "widgets/minimal/tree/label/" + row.id,
                                   0.65f));
            if (label) {
                append_bucket(content, label->bucket);
                max_width = std::max(max_width, label_x + label->width);
                cursor_y = std::max(cursor_y, text_top + label_height + kSpacing);
            }
        }
    }

    float canvas_width = std::max(max_width + kMargin, 360.0f);
    float canvas_height = std::max(cursor_y, 360.0f);

    SceneData::DrawableBucketSnapshot bucket{};
    append_bucket(bucket, make_background_bucket(canvas_width, canvas_height));
    append_bucket(bucket, content);

    result.bucket = std::move(bucket);
    result.width = static_cast<int>(std::ceil(canvas_width));
    result.height = static_cast<int>(std::ceil(canvas_height));
    return result;
}

struct MinimalContext {
    PathSpace* space = nullptr;
    SP::App::AppRootPath app_root{std::string{}};
    ScenePath scene;

    Widgets::ButtonPaths button_paths;
    Widgets::ButtonState button_state{};
    Widgets::ButtonStyle button_style{};
    std::string button_label;
    WidgetBindings::ButtonBinding button_binding{};

    Widgets::TogglePaths toggle_paths;
    Widgets::ToggleState toggle_state{};
    Widgets::ToggleStyle toggle_style{};
    WidgetBindings::ToggleBinding toggle_binding{};

    Widgets::SliderPaths slider_paths;
    Widgets::SliderStyle slider_style{};
    Widgets::SliderState slider_state{};
    Widgets::SliderRange slider_range{};

    Widgets::ListPaths list_paths;
    Widgets::ListStyle list_style{};
    Widgets::ListState list_state{};
    std::vector<Widgets::ListItem> list_items;

    Widgets::TreePaths tree_paths;
    Widgets::TreeStyle tree_style{};
    Widgets::TreeState tree_state{};
    std::vector<Widgets::TreeNode> tree_nodes;

    WidgetBindings::SliderBinding slider_binding{};
    WidgetBindings::ListBinding list_binding{};
    WidgetBindings::TreeBinding tree_binding{};

    Widgets::WidgetTheme theme{};

    WidgetFocus::Config focus_config{};
    WidgetInput::FocusTarget focus_target = WidgetInput::FocusTarget::Button;
    int focus_list_index = 0;
    int focus_tree_index = 0;

    WidgetInput::LayoutSnapshot layout{};
    std::string target_path;
    int scene_width = 0;
    int scene_height = 0;
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
    bool pointer_down = false;
    bool slider_dragging = false;
    std::string tree_pointer_down_id;
    bool tree_pointer_toggle = false;
};

static void reload_widget_states(MinimalContext& ctx) {
    auto* space = ctx.space;
    if (!space) {
        return;
    }

    ctx.button_state = unwrap_or_exit(space->read<Widgets::ButtonState, std::string>(
                                          std::string(ctx.button_paths.state.getPath())),
                                      "read button state");
    ctx.button_style = unwrap_or_exit(space->read<Widgets::ButtonStyle, std::string>(
                                           std::string(ctx.button_paths.root.getPath()) + "/meta/style"),
                                       "read button style");
    ctx.toggle_state = unwrap_or_exit(space->read<Widgets::ToggleState, std::string>(
                                          std::string(ctx.toggle_paths.state.getPath())),
                                      "read toggle state");
    ctx.toggle_style = unwrap_or_exit(space->read<Widgets::ToggleStyle, std::string>(
                                           std::string(ctx.toggle_paths.root.getPath()) + "/meta/style"),
                                       "read toggle style");

    ctx.slider_style = unwrap_or_exit(space->read<Widgets::SliderStyle, std::string>(
                                           std::string(ctx.slider_paths.root.getPath()) + "/meta/style"),
                                       "read slider style");
    ctx.slider_state = unwrap_or_exit(space->read<Widgets::SliderState, std::string>(
                                          std::string(ctx.slider_paths.state.getPath())),
                                      "read slider state");
    ctx.slider_range = unwrap_or_exit(space->read<Widgets::SliderRange, std::string>(
                                          std::string(ctx.slider_paths.range.getPath())),
                                      "read slider range");

    ctx.list_style = unwrap_or_exit(space->read<Widgets::ListStyle, std::string>(
                                        std::string(ctx.list_paths.root.getPath()) + "/meta/style"),
                                    "read list style");
    ctx.list_state = unwrap_or_exit(space->read<Widgets::ListState, std::string>(
                                        std::string(ctx.list_paths.state.getPath())),
                                    "read list state");
    ctx.list_items = unwrap_or_exit(space->read<std::vector<Widgets::ListItem>, std::string>(
                                        std::string(ctx.list_paths.items.getPath())),
                                    "read list items");

    ctx.tree_style = unwrap_or_exit(space->read<Widgets::TreeStyle, std::string>(
                                        std::string(ctx.tree_paths.root.getPath()) + "/meta/style"),
                                    "read tree style");
    ctx.tree_state = unwrap_or_exit(space->read<Widgets::TreeState, std::string>(
                                        std::string(ctx.tree_paths.state.getPath())),
                                    "read tree state");
    ctx.tree_nodes = unwrap_or_exit(space->read<std::vector<Widgets::TreeNode>, std::string>(
                                        std::string(ctx.tree_paths.nodes.getPath())),
                                    "read tree nodes");
}

static void refresh_scene(MinimalContext& ctx) {
    auto* space = ctx.space;
    if (!space) {
        return;
    }

    auto build = build_minimal_bucket(ctx.theme,
                                      ctx.button_style,
                                      ctx.button_label,
                                      ctx.button_state,
                                      ctx.toggle_style,
                                      ctx.toggle_state,
                                      ctx.slider_style,
                                      ctx.slider_state,
                                      ctx.slider_range,
                                      ctx.list_style,
                                      ctx.list_items,
                                      ctx.list_state,
                                      ctx.tree_style,
                                      ctx.tree_nodes,
                                      ctx.tree_state);

    ctx.scene_width = build.width;
    ctx.scene_height = build.height;
    ctx.layout = build.layout;

    auto app_view = SP::App::AppRootPathView{ctx.app_root.getPath()};
    SceneData::SceneSnapshotBuilder builder(*space, app_view, ctx.scene);

    SceneData::SnapshotPublishOptions options{};
    options.metadata.author = "widgets_example_minimal";
    options.metadata.tool_version = "widgets_example_minimal";
    options.metadata.created_at = std::chrono::system_clock::now();
    options.metadata.drawable_count = build.bucket.drawable_ids.size();
    options.metadata.command_count = build.bucket.command_kinds.size();

    unwrap_or_exit(builder.publish(options, build.bucket), "publish minimal scene snapshot");
    unwrap_or_exit(SceneBuilders::WaitUntilReady(*space,
                                                 ctx.scene,
                                                 std::chrono::milliseconds{50}),
                   "wait for minimal scene readiness");
}

static void rebuild_bindings(MinimalContext& ctx) {
    if (ctx.target_path.empty()) {
        return;
    }
    auto* space = ctx.space;
    if (!space) {
        return;
    }

    auto app_view = SP::App::AppRootPathView{ctx.app_root.getPath()};
    auto target_view = SP::ConcretePathStringView{ctx.target_path};

    if (ctx.layout.button_footprint.width() > 0.0f || ctx.layout.button_footprint.height() > 0.0f) {
        auto button_hint = WidgetInput::MakeDirtyHint(ctx.layout.button_footprint);
        ctx.button_binding = unwrap_or_exit(WidgetBindings::CreateButtonBinding(*space,
                                                                                app_view,
                                                                                ctx.button_paths,
                                                                                target_view,
                                                                                button_hint),
                                            "create button binding");
    }

    if (ctx.layout.toggle_footprint.width() > 0.0f || ctx.layout.toggle_footprint.height() > 0.0f) {
        auto toggle_hint = WidgetInput::MakeDirtyHint(ctx.layout.toggle_footprint);
        ctx.toggle_binding = unwrap_or_exit(WidgetBindings::CreateToggleBinding(*space,
                                                                                app_view,
                                                                                ctx.toggle_paths,
                                                                                target_view,
                                                                                toggle_hint),
                                            "create toggle binding");
    }

    if (ctx.layout.slider_footprint.width() > 0.0f || ctx.layout.slider_footprint.height() > 0.0f) {
        auto slider_hint = WidgetInput::MakeDirtyHint(ctx.layout.slider_footprint);
        ctx.slider_binding = unwrap_or_exit(WidgetBindings::CreateSliderBinding(*space,
                                                                                app_view,
                                                                                ctx.slider_paths,
                                                                                target_view,
                                                                                slider_hint),
                                            "create slider binding");
    }

    if (ctx.layout.list_footprint.width() > 0.0f || ctx.layout.list_footprint.height() > 0.0f) {
        auto list_hint = WidgetInput::MakeDirtyHint(ctx.layout.list_footprint);
        ctx.list_binding = unwrap_or_exit(WidgetBindings::CreateListBinding(*space,
                                                                            app_view,
                                                                            ctx.list_paths,
                                                                            target_view,
                                                                            list_hint),
                                          "create list binding");
    }

    if (ctx.layout.tree_footprint.width() > 0.0f || ctx.layout.tree_footprint.height() > 0.0f) {
        auto tree_hint = WidgetInput::MakeDirtyHint(ctx.layout.tree_footprint);
        ctx.tree_binding = unwrap_or_exit(WidgetBindings::CreateTreeBinding(*space,
                                                                            app_view,
                                                                            ctx.tree_paths,
                                                                            target_view,
                                                                            tree_hint),
                                          "create tree binding");
    }
}

static auto make_input_context(MinimalContext& ctx) -> WidgetInput::WidgetInputContext;

static void apply_input_update(MinimalContext& ctx, WidgetInput::InputUpdate const& update) {
    if (!ctx.space) {
        return;
    }
    if (update.focus_changed || update.state_changed) {
        reload_widget_states(ctx);
        refresh_scene(ctx);
        rebuild_bindings(ctx);
    }
}

static void handle_local_mouse(SP::UI::LocalMouseEvent const& ev, void* user_data) {
    auto* ctx = static_cast<MinimalContext*>(user_data);
    if (!ctx || !ctx->space) {
        return;
    }

    auto move_pointer = [&](float x, float y) {
        ctx->pointer_x = x;
        ctx->pointer_y = y;
        auto input = make_input_context(*ctx);
        auto update = WidgetInput::HandlePointerMove(input, x, y);
        apply_input_update(*ctx, update);
    };

    switch (ev.type) {
    case SP::UI::LocalMouseEventType::AbsoluteMove:
        if (ev.x >= 0 && ev.y >= 0) {
            move_pointer(static_cast<float>(ev.x), static_cast<float>(ev.y));
        }
        break;
    case SP::UI::LocalMouseEventType::Move: {
        float new_x = ctx->pointer_x + static_cast<float>(ev.dx);
        float new_y = ctx->pointer_y + static_cast<float>(ev.dy);
        move_pointer(new_x, new_y);
        break;
    }
    case SP::UI::LocalMouseEventType::ButtonDown:
        if (ev.x >= 0 && ev.y >= 0) {
            move_pointer(static_cast<float>(ev.x), static_cast<float>(ev.y));
        }
        if (ev.button == SP::UI::LocalMouseButton::Left) {
            auto input = make_input_context(*ctx);
            auto update = WidgetInput::HandlePointerDown(input);
            apply_input_update(*ctx, update);
        }
        break;
    case SP::UI::LocalMouseEventType::ButtonUp:
        if (ev.x >= 0 && ev.y >= 0) {
            move_pointer(static_cast<float>(ev.x), static_cast<float>(ev.y));
        }
        if (ev.button == SP::UI::LocalMouseButton::Left) {
            auto input = make_input_context(*ctx);
            auto update = WidgetInput::HandlePointerUp(input);
            apply_input_update(*ctx, update);
        }
        break;
    case SP::UI::LocalMouseEventType::Wheel: {
        auto input = make_input_context(*ctx);
        auto update = WidgetInput::HandlePointerWheel(input, ev.wheel);
        apply_input_update(*ctx, update);
        break;
    }
    }
}

static void clear_local_mouse(void* user_data) {
    auto* ctx = static_cast<MinimalContext*>(user_data);
    if (!ctx) {
        return;
    }
    ctx->pointer_down = false;
    ctx->slider_dragging = false;
    ctx->tree_pointer_down_id.clear();
    ctx->tree_pointer_toggle = false;
}

static auto make_input_context(MinimalContext& ctx) -> WidgetInput::WidgetInputContext {
    WidgetInput::WidgetInputContext input{};
    input.space = ctx.space;
    input.layout = ctx.layout;

    static constexpr std::array<WidgetInput::FocusTarget, 5> kFocusOrder{
        WidgetInput::FocusTarget::Button,
        WidgetInput::FocusTarget::Toggle,
        WidgetInput::FocusTarget::Slider,
        WidgetInput::FocusTarget::List,
        WidgetInput::FocusTarget::Tree,
    };

    input.focus.config = &ctx.focus_config;
    input.focus.current = &ctx.focus_target;
    input.focus.order = std::span<const WidgetInput::FocusTarget>{kFocusOrder};
    input.focus.button = ctx.button_paths.root;
    input.focus.toggle = ctx.toggle_paths.root;
    input.focus.slider = ctx.slider_paths.root;
    input.focus.list = ctx.list_paths.root;
    input.focus.tree = ctx.tree_paths.root;
    input.focus.focus_list_index = &ctx.focus_list_index;
    input.focus.focus_tree_index = &ctx.focus_tree_index;

    input.button_binding = &ctx.button_binding;
    input.button_paths = &ctx.button_paths;
    input.button_state = &ctx.button_state;

    input.toggle_binding = &ctx.toggle_binding;
    input.toggle_paths = &ctx.toggle_paths;
    input.toggle_state = &ctx.toggle_state;

    input.slider_binding = &ctx.slider_binding;
    input.slider_paths = &ctx.slider_paths;
    input.slider_state = &ctx.slider_state;
    input.slider_style = &ctx.slider_style;
    input.slider_range = &ctx.slider_range;

    input.list_binding = &ctx.list_binding;
    input.list_paths = &ctx.list_paths;
    input.list_state = &ctx.list_state;
    input.list_style = &ctx.list_style;
    input.list_items = &ctx.list_items;

    input.tree_binding = &ctx.tree_binding;
    input.tree_paths = &ctx.tree_paths;
    input.tree_state = &ctx.tree_state;
    input.tree_style = &ctx.tree_style;
    input.tree_nodes = &ctx.tree_nodes;

    input.pointer_x = &ctx.pointer_x;
    input.pointer_y = &ctx.pointer_y;
    input.pointer_down = &ctx.pointer_down;
    input.slider_dragging = &ctx.slider_dragging;
    input.tree_pointer_down_id = &ctx.tree_pointer_down_id;
    input.tree_pointer_toggle = &ctx.tree_pointer_toggle;

    return input;
}

struct PresentStats {
    bool presented = false;
    bool skipped = false;
};

static auto present_frame(PathSpace& space,
                          AppBuilders::BootstrapResult const& bootstrap,
                          int width,
                          int height) -> std::optional<PresentStats> {
    auto present_result = WindowBuilders::Present(space,
                                                  bootstrap.window,
                                                  bootstrap.view_name);
    if (!present_result) {
        std::cerr << "widgets_example_minimal: present failed";
        if (present_result.error().message) {
            std::cerr << ": " << *present_result.error().message;
        }
        std::cerr << '\n';
        return std::nullopt;
    }

    PresentStats stats{};
    stats.skipped = present_result->stats.skipped;
    auto dispatched = AppBuilders::PresentToLocalWindow(*present_result,
                                                        width,
                                                        height);
    stats.presented = dispatched.presented;
    return stats;
}

constexpr unsigned int kKeycodeTab = 0x30;
constexpr unsigned int kKeycodeSpace = 0x31;
constexpr unsigned int kKeycodeReturn = 0x24;
constexpr unsigned int kKeycodeLeft = 0x7B;
constexpr unsigned int kKeycodeRight = 0x7C;
constexpr unsigned int kKeycodeDown = 0x7D;
constexpr unsigned int kKeycodeUp = 0x7E;
constexpr unsigned int kKeycodeEscape = 0x35;

static void handle_key_event(SP::UI::LocalKeyEvent const& key, void* user_data) {
    auto* ctx = static_cast<MinimalContext*>(user_data);
    if (!ctx || !ctx->space) {
        return;
    }

    if (key.type != SP::UI::LocalKeyEventType::KeyDown) {
        return;
    }

    bool handled = false;
    switch (key.keycode) {
    case kKeycodeTab: {
        bool backward = (key.modifiers & SP::UI::LocalKeyModifierShift) != 0;
        auto input = make_input_context(*ctx);
        auto update = WidgetInput::CycleFocus(input, !backward);
        apply_input_update(*ctx, update);
        handled = true;
        break;
    }
    case kKeycodeLeft:
        if (ctx->focus_target == WidgetInput::FocusTarget::Slider) {
            auto input = make_input_context(*ctx);
            auto update = WidgetInput::AdjustSliderByStep(input, -1);
            apply_input_update(*ctx, update);
            handled = true;
        }
        break;
    case kKeycodeRight:
        if (ctx->focus_target == WidgetInput::FocusTarget::Slider) {
            auto input = make_input_context(*ctx);
            auto update = WidgetInput::AdjustSliderByStep(input, 1);
            apply_input_update(*ctx, update);
            handled = true;
        }
        break;
    case kKeycodeDown:
        if (ctx->focus_target == WidgetInput::FocusTarget::List) {
            auto input = make_input_context(*ctx);
            auto update = WidgetInput::MoveListFocus(input, +1);
            apply_input_update(*ctx, update);
            handled = true;
        } else if (ctx->focus_target == WidgetInput::FocusTarget::Tree) {
            auto input = make_input_context(*ctx);
            auto update = WidgetInput::MoveTreeFocus(input, +1);
            apply_input_update(*ctx, update);
            handled = true;
        }
        break;
    case kKeycodeUp:
        if (ctx->focus_target == WidgetInput::FocusTarget::List) {
            auto input = make_input_context(*ctx);
            auto update = WidgetInput::MoveListFocus(input, -1);
            apply_input_update(*ctx, update);
            handled = true;
        } else if (ctx->focus_target == WidgetInput::FocusTarget::Tree) {
            auto input = make_input_context(*ctx);
            auto update = WidgetInput::MoveTreeFocus(input, -1);
            apply_input_update(*ctx, update);
            handled = true;
        }
        break;
    case kKeycodeSpace:
    case kKeycodeReturn: {
        auto input = make_input_context(*ctx);
        auto update = WidgetInput::ActivateFocusedWidget(input);
        apply_input_update(*ctx, update);
        handled = true;
        break;
    }
    case kKeycodeEscape:
        SP::UI::RequestLocalWindowQuit();
        handled = true;
        break;
    default:
        break;
    }

    if (!handled && ctx->focus_target == WidgetInput::FocusTarget::Tree) {
        if (key.character == U' ') {
            auto input = make_input_context(*ctx);
            auto update = WidgetInput::TreeApplyOp(input,
                                                   WidgetBindings::WidgetOpKind::TreeToggle);
            apply_input_update(*ctx, update);
        }
    }
}

} // namespace

int main(int /*argc*/, char** /*argv*/) {
    PathSpace space;

    App::AppRootPath app_root{"/system/applications/widgets_example_minimal"};
    App::AppRootPathView app_root_view{app_root.getPath()};

    auto theme_selection = Widgets::LoadTheme(space, app_root_view, std::string_view{"skylight"});
    auto theme = std::move(theme_selection.theme);
    attach_demo_fonts(space, app_root_view, theme);

    auto button_params = Widgets::MakeButtonParams("demo_button", "Action")
                             .WithTheme(theme)
                             .Build();
    auto button_paths = unwrap_or_exit(Widgets::CreateButton(space,
                                                             app_root_view,
                                                             button_params),
                                       "create button widget");

    auto toggle_params = Widgets::MakeToggleParams("demo_toggle")
                             .WithTheme(theme)
                             .Build();
    auto toggle_paths = unwrap_or_exit(Widgets::CreateToggle(space,
                                                             app_root_view,
                                                             toggle_params),
                                       "create toggle widget");

    auto slider_params = Widgets::MakeSliderParams("demo_slider")
                             .WithRange(0.0f, 100.0f)
                             .WithValue(35.0f)
                             .WithStep(5.0f)
                             .WithTheme(theme)
                             .Build();
    auto slider_paths = unwrap_or_exit(Widgets::CreateSlider(space,
                                                             app_root_view,
                                                             slider_params),
                                       "create slider widget");

    auto list_params = Widgets::MakeListParams("inventory_list")
                           .WithItems({
                               Widgets::ListItem{.id = "potion", .label = "Potion", .enabled = true},
                               Widgets::ListItem{.id = "ether", .label = "Ether", .enabled = true},
                               Widgets::ListItem{.id = "elixir", .label = "Elixir", .enabled = true},
                               Widgets::ListItem{.id = "antidote", .label = "Antidote", .enabled = true},
                           })
                           .WithTheme(theme)
                           .Build();
    auto list_paths = unwrap_or_exit(Widgets::CreateList(space,
                                                         app_root_view,
                                                         list_params),
                                     "create list widget");
    auto list_state = Widgets::MakeListState()
                          .WithFocused(false)
                          .WithSelectedIndex(0)
                          .Build();
    unwrap_or_exit(Widgets::UpdateListState(space, list_paths, list_state),
                   "initialize list state");

    std::vector<Widgets::TreeNode> tree_nodes{
        Widgets::TreeNode{.id = "workspace", .parent_id = "", .label = "workspace/", .enabled = true, .expandable = true, .loaded = true},
        Widgets::TreeNode{.id = "docs", .parent_id = "workspace", .label = "docs/", .enabled = true, .expandable = false, .loaded = false},
        Widgets::TreeNode{.id = "src", .parent_id = "workspace", .label = "src/", .enabled = true, .expandable = true, .loaded = true},
        Widgets::TreeNode{.id = "examples", .parent_id = "src", .label = "ui/examples/", .enabled = true, .expandable = false, .loaded = false},
        Widgets::TreeNode{.id = "tests", .parent_id = "workspace", .label = "tests/", .enabled = true, .expandable = false, .loaded = false},
    };

    auto tree_params = Widgets::MakeTreeParams("workspace_tree")
                           .WithNodes(tree_nodes)
                           .WithTheme(theme)
                           .Build();
    auto tree_paths = unwrap_or_exit(Widgets::CreateTree(space,
                                                         app_root_view,
                                                         tree_params),
                                     "create tree widget");
    auto tree_state = Widgets::MakeTreeState()
                          .WithFocused(false)
                          .WithSelectedId("workspace")
                          .WithExpandedIds({"workspace", "src"})
                          .Build();
    unwrap_or_exit(Widgets::UpdateTreeState(space, tree_paths, tree_state),
                   "initialize tree state");

    MinimalContext ctx{};
    ctx.space = &space;
    ctx.app_root = app_root;
    ctx.scene = unwrap_or_exit(SceneBuilders::Create(space,
                                                     app_root_view,
                                                     SceneParams{
                                                         .name = "widgets_example_minimal_scene",
                                                         .description = "Minimal slider/list/tree showcase",
                                                     }),
                               "create minimal scene");

    ctx.theme = theme;
    ctx.button_paths = button_paths;
    ctx.toggle_paths = toggle_paths;
    ctx.slider_paths = slider_paths;
    ctx.list_paths = list_paths;
    ctx.tree_paths = tree_paths;
    ctx.button_label = button_params.label;

    reload_widget_states(ctx);
    refresh_scene(ctx);

    AppBuilders::BootstrapParams bootstrap_params{};
    bootstrap_params.renderer.name = "minimal_renderer";
    bootstrap_params.renderer.description = "Minimal widget renderer";
    bootstrap_params.surface.name = "minimal_surface";
    bootstrap_params.surface.desc.size_px.width = ctx.scene_width;
    bootstrap_params.surface.desc.size_px.height = ctx.scene_height;
    bootstrap_params.surface.desc.pixel_format = PixelFormat::RGBA8Unorm_sRGB;
    bootstrap_params.surface.desc.color_space = ColorSpace::sRGB;
    bootstrap_params.surface.desc.premultiplied_alpha = true;
    bootstrap_params.window.name = "minimal_window";
    bootstrap_params.window.title = "PathSpace Widgets Minimal";
    bootstrap_params.window.width = ctx.scene_width;
    bootstrap_params.window.height = ctx.scene_height;
    bootstrap_params.window.scale = 1.0f;
    bootstrap_params.window.background = "#1f232b";
    bootstrap_params.present_policy.mode = PathWindowView::PresentMode::AlwaysLatestComplete;
    bootstrap_params.present_policy.auto_render_on_present = true;
    bootstrap_params.present_policy.vsync_align = false;
    bootstrap_params.view_name = "main";

    auto bootstrap = unwrap_or_exit(AppBuilders::Bootstrap(space,
                                                           app_root_view,
                                                           ctx.scene,
                                                           bootstrap_params),
                                    "bootstrap renderer");

    ctx.target_path = bootstrap.target.getPath();
    ctx.focus_config = WidgetFocus::MakeConfig(app_root_view, bootstrap.target);

    rebuild_bindings(ctx);

    auto focus_update = WidgetFocus::Set(space,
                                         ctx.focus_config,
                                         ctx.button_paths.root);
    if (focus_update && focus_update->changed) {
        ctx.focus_target = WidgetInput::FocusTarget::Button;
        reload_widget_states(ctx);
        refresh_scene(ctx);
        rebuild_bindings(ctx);
    }

    SP::UI::SetLocalWindowCallbacks({
        &handle_local_mouse,
        &clear_local_mouse,
        &ctx,
        &handle_key_event,
    });

    SP::UI::InitLocalWindowWithSize(bootstrap.surface_desc.size_px.width,
                                    bootstrap.surface_desc.size_px.height,
                                    "PathSpace Widgets Minimal");

    int window_width = bootstrap.surface_desc.size_px.width;
    int window_height = bootstrap.surface_desc.size_px.height;

    while (!SP::UI::LocalWindowQuitRequested()) {
        SP::UI::PollLocalWindow();

        int requested_width = window_width;
        int requested_height = window_height;
        SP::UI::GetLocalWindowContentSize(&requested_width, &requested_height);
        if (requested_width <= 0 || requested_height <= 0) {
            continue;
        }
        if (requested_width != window_width || requested_height != window_height) {
            window_width = requested_width;
            window_height = requested_height;
            unwrap_or_exit(AppBuilders::UpdateSurfaceSize(space,
                                                          bootstrap,
                                                          window_width,
                                                          window_height),
                           "resize surface");
        }

        present_frame(space, bootstrap, window_width, window_height);
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }

    return 0;
}

#endif // PATHSPACE_ENABLE_UI && __APPLE__
