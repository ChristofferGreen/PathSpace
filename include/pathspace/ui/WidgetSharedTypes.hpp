#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/task/Future.hpp>
#include <pathspace/ui/LocalWindowBridge.hpp>
#include <pathspace/ui/PathWindowView.hpp>
#include <pathspace/ui/SceneSnapshotBuilder.hpp>
#include <pathspace/ui/TextBuilder.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SP::UI::Builders::Scene {
struct HitTestResult;
} // namespace SP::UI::Builders::Scene

namespace SP::UI::Builders {
using AppRootPath = SP::App::AppRootPath;
using AppRootPathView = SP::App::AppRootPathView;
using ConcretePath = SP::ConcretePathString;
using ConcretePathView = SP::ConcretePathStringView;
using UnvalidatedPathView = SP::UnvalidatedPathView;
using ScenePath = ConcretePath;
using RendererPath = ConcretePath;
using SurfacePath = ConcretePath;
using WindowPath = ConcretePath;
using HtmlTargetPath = ConcretePath;
using WidgetPath = ConcretePath;

namespace Text {
struct BuildResult;
} // namespace Text

struct DirtyRectHint {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
};

[[nodiscard]] inline auto MakeDirtyRectHint(float min_x,
                                            float min_y,
                                            float max_x,
                                            float max_y) -> DirtyRectHint {
    return DirtyRectHint{min_x, min_y, max_x, max_y};
}

enum class ParamUpdateMode {
    Queue,
    ReplaceActive,
};

struct AutoRenderRequestEvent {
    std::uint64_t sequence = 0;
    std::string reason;
    std::uint64_t frame_index = 0;
};

namespace Widgets {

namespace Reducers {
struct WidgetAction;
} // namespace Reducers

namespace Input {
struct WidgetBounds;
}

struct TypographyStyle {
    float font_size = 28.0f;
    float line_height = 28.0f;
    float letter_spacing = 1.0f;
    float baseline_shift = 0.0f;
    std::string font_family = "system-ui";
    std::string font_style = "normal";
    std::string font_weight = "400";
    std::string language = "en";
    std::string direction = "ltr";
    std::vector<std::string> fallback_families{};
    std::vector<std::string> font_features{};
    std::string font_resource_root;
    std::uint64_t font_active_revision = 0;
    std::uint64_t font_asset_fingerprint = 0;
};

struct ButtonStyle {
    float width = 200.0f;
    float height = 48.0f;
    float corner_radius = 6.0f;
    std::array<float, 4> background_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> text_color{1.0f, 1.0f, 1.0f, 1.0f};
    TypographyStyle typography{};
};

struct ButtonState {
    bool enabled = true;
    bool pressed = false;
    bool hovered = false;
    bool focused = false;
};

struct ButtonParams {
    std::string name;
    std::string label;
    ButtonStyle style{};
};

struct WidgetStateScenes {
    ScenePath idle;
    ScenePath hover;
    ScenePath pressed;
    ScenePath disabled;
};

struct ButtonPaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
    ConcretePath label;
};

auto CreateButton(PathSpace& space,
                  AppRootPathView appRoot,
                  ButtonParams const& params) -> SP::Expected<ButtonPaths>;

struct ToggleStyle {
    float width = 56.0f;
    float height = 32.0f;
    std::array<float, 4> track_off_color{0.75f, 0.75f, 0.78f, 1.0f};
    std::array<float, 4> track_on_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> thumb_color{1.0f, 1.0f, 1.0f, 1.0f};
};

struct ToggleState {
    bool enabled = true;
    bool hovered = false;
    bool checked = false;
    bool focused = false;
};

struct ToggleParams {
    std::string name;
    ToggleStyle style{};
};

struct TogglePaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
};

auto CreateToggle(PathSpace& space,
                  AppRootPathView appRoot,
                  ToggleParams const& params) -> SP::Expected<TogglePaths>;

auto UpdateButtonState(PathSpace& space,
                       ButtonPaths const& paths,
                       ButtonState const& new_state) -> SP::Expected<bool>;

auto SetExclusiveButtonFocus(PathSpace& space,
                             std::span<ButtonPaths const> buttons,
                             std::optional<std::size_t> focused_index) -> SP::Expected<void>;

auto UpdateToggleState(PathSpace& space,
                       TogglePaths const& paths,
                       ToggleState const& new_state) -> SP::Expected<bool>;

struct ButtonPreviewOptions {
    std::string authoring_root;
    std::string label;
    bool pulsing_highlight = true;
};

auto BuildButtonPreview(ButtonStyle const& style,
                        ButtonState const& state,
                        ButtonPreviewOptions const& options = {}) -> SP::UI::Scene::DrawableBucketSnapshot;

struct LabelBuildParams {
    std::string text;
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    TypographyStyle typography{};
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    std::uint64_t drawable_id = 0;
    std::string authoring_id;
    float z_value = 0.0f;

    [[nodiscard]] static auto Make(std::string text_value, TypographyStyle typography_value) -> LabelBuildParams {
        LabelBuildParams params{};
        params.text = std::move(text_value);
        params.typography = std::move(typography_value);
        return params;
    }

    auto WithOrigin(float x, float y) & -> LabelBuildParams& {
        origin_x = x;
        origin_y = y;
        return *this;
    }

    auto WithOrigin(float x, float y) && -> LabelBuildParams {
        origin_x = x;
        origin_y = y;
        return std::move(*this);
    }

    auto WithColor(std::array<float, 4> value) & -> LabelBuildParams& {
        color = value;
        return *this;
    }

    auto WithColor(std::array<float, 4> value) && -> LabelBuildParams {
        color = value;
        return std::move(*this);
    }

    auto WithDrawable(std::uint64_t id, std::string authoring, float z = 0.0f) & -> LabelBuildParams& {
        drawable_id = id;
        authoring_id = std::move(authoring);
        z_value = z;
        return *this;
    }

    auto WithDrawable(std::uint64_t id, std::string authoring, float z = 0.0f) && -> LabelBuildParams {
        drawable_id = id;
        authoring_id = std::move(authoring);
        z_value = z;
        return std::move(*this);
    }
};

auto BuildLabel(LabelBuildParams const& params) -> std::optional<Text::BuildResult>;

auto LabelBounds(Text::BuildResult const& result) -> std::optional<Input::WidgetBounds>;

inline auto MakeMouseEvent(SP::MouseEventType type,
                           int x = 0,
                           int y = 0,
                           SP::MouseButton button = SP::MouseButton::Left,
                           int dx = 0,
                           int dy = 0,
                           int wheel = 0) -> SP::PathIOMouse::Event {
    SP::PathIOMouse::Event event{};
    event.type = type;
    event.x = x;
    event.y = y;
    event.dx = dx;
    event.dy = dy;
    event.button = button;
    event.wheel = wheel;
    return event;
}

inline auto MakeLocalKeyEvent(SP::UI::LocalKeyEventType type,
                              unsigned int keycode,
                              unsigned int modifiers,
                              char32_t character,
                              bool repeat) -> SP::UI::LocalKeyEvent {
    SP::UI::LocalKeyEvent event{};
    event.type = type;
    event.keycode = keycode;
    event.modifiers = modifiers;
    event.character = character;
    event.repeat = repeat;
    return event;
}

struct TogglePreviewOptions {
    std::string authoring_root;
    bool pulsing_highlight = true;
};

auto BuildTogglePreview(ToggleStyle const& style,
                        ToggleState const& state,
                        TogglePreviewOptions const& options = {}) -> SP::UI::Scene::DrawableBucketSnapshot;

struct SliderStyle {
    float width = 240.0f;
    float height = 32.0f;
    float track_height = 6.0f;
    float thumb_radius = 10.0f;
    std::array<float, 4> track_color{0.75f, 0.75f, 0.78f, 1.0f};
    std::array<float, 4> fill_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> thumb_color{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> label_color{0.90f, 0.92f, 0.96f, 1.0f};
    TypographyStyle label_typography{
        .font_size = 24.0f,
        .line_height = 28.0f,
        .letter_spacing = 1.0f,
        .baseline_shift = 0.0f,
    };
};

struct SliderState {
    bool enabled = true;
    bool hovered = false;
    bool dragging = false;
    bool focused = false;
    float value = 0.0f;
};

struct SliderParams {
    std::string name;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float value = 0.5f;
    float step = 0.0f; // 0 => continuous
    SliderStyle style{};
};

struct SliderRange {
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.0f;
};

struct SliderPaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
    ConcretePath range;
};

auto CreateSlider(PathSpace& space,
                  AppRootPathView appRoot,
                  SliderParams const& params) -> SP::Expected<SliderPaths>;

auto UpdateSliderState(PathSpace& space,
                       SliderPaths const& paths,
                       SliderState const& new_state) -> SP::Expected<bool>;

struct SliderPreviewOptions {
    std::string authoring_root;
    bool pulsing_highlight = true;
};

auto BuildSliderPreview(SliderStyle const& style,
                        SliderRange const& range,
                        SliderState const& state,
                        SliderPreviewOptions const& options = {}) -> SP::UI::Scene::DrawableBucketSnapshot;

struct ListStyle {
    float width = 240.0f;
    float item_height = 36.0f;
    float corner_radius = 8.0f;
    float border_thickness = 1.0f;
    std::array<float, 4> background_color{0.121f, 0.129f, 0.145f, 1.0f};
    std::array<float, 4> border_color{0.239f, 0.247f, 0.266f, 1.0f};
    std::array<float, 4> item_color{0.176f, 0.184f, 0.204f, 1.0f};
    std::array<float, 4> item_hover_color{0.247f, 0.278f, 0.349f, 1.0f};
    std::array<float, 4> item_selected_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> separator_color{0.224f, 0.231f, 0.247f, 1.0f};
    std::array<float, 4> item_text_color{0.94f, 0.96f, 0.99f, 1.0f};
    TypographyStyle item_typography{
        .font_size = 21.0f,
        .line_height = 24.0f,
        .letter_spacing = 1.0f,
        .baseline_shift = 0.0f,
    };
};

struct ListItem {
    std::string id;
    std::string label;
    bool enabled = true;
};

struct ListState {
    bool enabled = true;
    bool focused = false;
    std::int32_t hovered_index = -1;
    std::int32_t selected_index = -1;
    float scroll_offset = 0.0f;
};

struct ListParams {
    std::string name;
    std::vector<ListItem> items;
    ListStyle style{};
};

struct ListPaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
    ConcretePath items;
};

auto CreateList(PathSpace& space,
                AppRootPathView appRoot,
                ListParams const& params) -> SP::Expected<ListPaths>;

auto UpdateListState(PathSpace& space,
                     ListPaths const& paths,
                     ListState const& new_state) -> SP::Expected<bool>;

struct ListPreviewRect {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    [[nodiscard]] auto width() const -> float {
        return std::max(0.0f, max_x - min_x);
    }

    [[nodiscard]] auto height() const -> float {
        return std::max(0.0f, max_y - min_y);
    }
};

struct ListPreviewRowLayout {
    std::string id;
    bool enabled = true;
    bool hovered = false;
    bool selected = false;
    ListPreviewRect row_bounds{};
    ListPreviewRect label_bounds{};
    float label_baseline = 0.0f;
};

struct ListPreviewLayout {
    ListPreviewRect bounds{};
    float content_top = 0.0f;
    float item_height = 0.0f;
    float border_thickness = 0.0f;
    float label_inset = 0.0f;
    ListStyle style{};
    ListState state{};
    std::vector<ListPreviewRowLayout> rows;
};

struct ListPreviewOptions {
    std::string authoring_root;
    float label_inset = 16.0f;
    bool pulsing_highlight = true;
};

struct ListPreviewResult {
    SP::UI::Scene::DrawableBucketSnapshot bucket;
    ListPreviewLayout layout;
};

auto BuildListPreview(ListStyle const& style,
                      std::span<ListItem const> items,
                      ListState const& state,
                      ListPreviewOptions const& options = {}) -> ListPreviewResult;

struct TreeStyle {
    float width = 280.0f;
    float row_height = 32.0f;
    float corner_radius = 8.0f;
    float border_thickness = 1.0f;
    float indent_per_level = 18.0f;
    float toggle_icon_size = 12.0f;
    std::array<float, 4> background_color{0.121f, 0.129f, 0.145f, 1.0f};
    std::array<float, 4> border_color{0.239f, 0.247f, 0.266f, 1.0f};
    std::array<float, 4> row_color{0.176f, 0.184f, 0.204f, 1.0f};
    std::array<float, 4> row_hover_color{0.247f, 0.278f, 0.349f, 1.0f};
    std::array<float, 4> row_selected_color{0.176f, 0.353f, 0.914f, 1.0f};
    std::array<float, 4> row_disabled_color{0.145f, 0.149f, 0.162f, 1.0f};
    std::array<float, 4> connector_color{0.224f, 0.231f, 0.247f, 1.0f};
    std::array<float, 4> toggle_color{0.90f, 0.92f, 0.96f, 1.0f};
    std::array<float, 4> text_color{0.94f, 0.96f, 0.99f, 1.0f};
    TypographyStyle label_typography{
        .font_size = 20.0f,
        .line_height = 24.0f,
        .letter_spacing = 0.8f,
        .baseline_shift = 0.0f,
    };
};

struct TreeNode {
    std::string id;
    std::string parent_id;
    std::string label;
    bool enabled = true;
    bool expandable = false;
    bool loaded = true;
};

struct TreeState {
    bool enabled = true;
    bool focused = false;
    std::string hovered_id;
    std::string selected_id;
    std::vector<std::string> expanded_ids;
    std::vector<std::string> loading_ids;
    float scroll_offset = 0.0f;
};

struct TreeParams {
    std::string name;
    std::vector<TreeNode> nodes;
    TreeStyle style{};
};

struct TreePaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
    ConcretePath nodes;
};

auto CreateTree(PathSpace& space,
                AppRootPathView appRoot,
                TreeParams const& params) -> SP::Expected<TreePaths>;

auto UpdateTreeState(PathSpace& space,
                     TreePaths const& paths,
                     TreeState const& new_state) -> SP::Expected<bool>;

struct TreePreviewRect {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    [[nodiscard]] auto width() const -> float {
        return std::max(0.0f, max_x - min_x);
    }

    [[nodiscard]] auto height() const -> float {
        return std::max(0.0f, max_y - min_y);
    }
};

struct TreePreviewRowLayout {
    std::string id;
    std::string label;
    int depth = 0;
    bool expandable = false;
    bool expanded = false;
    bool loading = false;
    bool enabled = true;
    TreePreviewRect row_bounds{};
    TreePreviewRect toggle_bounds{};
};

struct TreePreviewLayout {
    TreePreviewRect bounds{};
    float content_top = 0.0f;
    float row_height = 0.0f;
    TreeStyle style{};
    TreeState state{};
    std::vector<TreePreviewRowLayout> rows;
};

struct TreePreviewOptions {
    std::string authoring_root;
    bool pulsing_highlight = true;
};

struct TreePreviewResult {
    SP::UI::Scene::DrawableBucketSnapshot bucket;
    TreePreviewLayout layout;
};

auto BuildTreePreview(TreeStyle const& style,
                      std::span<TreeNode const> nodes,
                      TreeState const& state,
                      TreePreviewOptions const& options = {}) -> TreePreviewResult;

enum class StackAxis : std::uint8_t {
    Horizontal = 0,
    Vertical = 1,
};

enum class StackAlignMain : std::uint8_t {
    Start = 0,
    Center = 1,
    End = 2,
};

enum class StackAlignCross : std::uint8_t {
    Start = 0,
    Center = 1,
    End = 2,
    Stretch = 3,
};

struct StackChildConstraints {
    float weight = 0.0f;
    float min_main = 0.0f;
    float max_main = 0.0f;
    float min_cross = 0.0f;
    float max_cross = 0.0f;
    float margin_main_start = 0.0f;
    float margin_main_end = 0.0f;
    float margin_cross_start = 0.0f;
    float margin_cross_end = 0.0f;
    bool has_min_main = false;
    bool has_max_main = false;
    bool has_min_cross = false;
    bool has_max_cross = false;
};

struct StackChildSpec {
    std::string id;
    std::string widget_path;
    std::string scene_path;
    StackChildConstraints constraints{};
};

struct StackLayoutStyle {
    StackAxis axis = StackAxis::Vertical;
    float spacing = 16.0f;
    StackAlignMain align_main = StackAlignMain::Start;
    StackAlignCross align_cross = StackAlignCross::Stretch;
    float padding_main_start = 0.0f;
    float padding_main_end = 0.0f;
    float padding_cross_start = 0.0f;
    float padding_cross_end = 0.0f;
    float width = 0.0f;  // 0 => derive from children
    float height = 0.0f; // 0 => derive from children
    bool clip_contents = false;
};

struct StackLayoutComputedChild {
    std::string id;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

struct StackLayoutState {
    float width = 0.0f;
    float height = 0.0f;
    std::vector<StackLayoutComputedChild> children;
};

struct StackPreviewRect {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    [[nodiscard]] auto width() const -> float {
        return std::max(0.0f, max_x - min_x);
    }

    [[nodiscard]] auto height() const -> float {
        return std::max(0.0f, max_y - min_y);
    }
};

struct StackPreviewLayout {
    StackPreviewRect bounds{};
    StackLayoutStyle style{};
    StackLayoutState state{};
    std::vector<StackPreviewRect> child_bounds;
};

struct StackPreviewOptions {
    std::string authoring_root = "widgets/stack_preview";
    std::array<float, 4> background_color{0.10f, 0.12f, 0.16f, 1.0f};
    std::array<float, 4> child_start_color{0.85f, 0.88f, 0.95f, 1.0f};
    std::array<float, 4> child_end_color{0.93f, 0.95f, 0.98f, 1.0f};
    float child_opacity = 0.85f;
    float mix_scale = 1.0f;
};

struct StackPreviewResult {
    SP::UI::Scene::DrawableBucketSnapshot bucket;
    StackPreviewLayout layout;
};

auto BuildStackPreview(StackLayoutStyle const& style,
                       StackLayoutState const& state,
                       StackPreviewOptions const& options = {}) -> StackPreviewResult;

struct StackLayoutParams {
    std::string name;
    StackLayoutStyle style{};
    std::vector<StackChildSpec> children;
};

struct StackPaths {
    ScenePath scene;
    WidgetPath root;
    ConcretePath style;
    ConcretePath children;
    ConcretePath computed;
};

auto CreateStack(PathSpace& space,
                 AppRootPathView appRoot,
                 StackLayoutParams const& params) -> SP::Expected<StackPaths>;

auto UpdateStackLayout(PathSpace& space,
                       StackPaths const& paths,
                       StackLayoutParams const& params) -> SP::Expected<bool>;

auto DescribeStack(PathSpace const& space,
                   StackPaths const& paths) -> SP::Expected<StackLayoutParams>;

auto ReadStackLayout(PathSpace const& space,
                     StackPaths const& paths) -> SP::Expected<StackLayoutState>;

struct TextFieldStyle {
    float width = 320.0f;
    float height = 48.0f;
    float corner_radius = 6.0f;
    float border_thickness = 1.5f;
    std::array<float, 4> background_color{0.121f, 0.129f, 0.145f, 1.0f};
    std::array<float, 4> border_color{0.239f, 0.247f, 0.266f, 1.0f};
    std::array<float, 4> text_color{0.94f, 0.96f, 0.99f, 1.0f};
    std::array<float, 4> placeholder_color{0.58f, 0.60f, 0.66f, 1.0f};
    std::array<float, 4> selection_color{0.247f, 0.278f, 0.349f, 0.65f};
    std::array<float, 4> composition_color{0.353f, 0.388f, 0.458f, 0.55f};
    std::array<float, 4> caret_color{0.94f, 0.96f, 0.99f, 1.0f};
    float padding_x = 12.0f;
    float padding_y = 10.0f;
    TypographyStyle typography{
        .font_size = 24.0f,
        .line_height = 28.0f,
        .letter_spacing = 0.5f,
        .baseline_shift = 0.0f,
    };
    bool submit_on_enter = true;
};

struct TextAreaStyle {
    float width = 320.0f;
    float height = 180.0f;
    float corner_radius = 6.0f;
    float border_thickness = 1.5f;
    std::array<float, 4> background_color{0.121f, 0.129f, 0.145f, 1.0f};
    std::array<float, 4> border_color{0.239f, 0.247f, 0.266f, 1.0f};
    std::array<float, 4> text_color{0.94f, 0.96f, 0.99f, 1.0f};
    std::array<float, 4> placeholder_color{0.58f, 0.60f, 0.66f, 1.0f};
    std::array<float, 4> selection_color{0.247f, 0.278f, 0.349f, 0.65f};
    std::array<float, 4> composition_color{0.353f, 0.388f, 0.458f, 0.55f};
    std::array<float, 4> caret_color{0.94f, 0.96f, 0.99f, 1.0f};
    float padding_x = 12.0f;
    float padding_y = 10.0f;
    TypographyStyle typography{
        .font_size = 24.0f,
        .line_height = 28.0f,
        .letter_spacing = 0.5f,
        .baseline_shift = 0.0f,
    };
    float min_height = 160.0f;
    float line_spacing = 6.0f;
    bool wrap_lines = true;
};

struct TextFieldState {
    bool enabled = true;
    bool read_only = false;
    bool hovered = false;
    bool focused = false;
    std::string text;
    std::string placeholder;
    std::uint32_t cursor = 0;
    std::uint32_t selection_start = 0;
    std::uint32_t selection_end = 0;
    bool composition_active = false;
    std::string composition_text;
    std::uint32_t composition_start = 0;
    std::uint32_t composition_end = 0;
    bool submit_pending = false;
};

struct TextAreaState {
    bool enabled = true;
    bool read_only = false;
    bool hovered = false;
    bool focused = false;
    std::string text;
    std::string placeholder;
    std::uint32_t cursor = 0;
    std::uint32_t selection_start = 0;
    std::uint32_t selection_end = 0;
    bool composition_active = false;
    std::string composition_text;
    std::uint32_t composition_start = 0;
    std::uint32_t composition_end = 0;
    float scroll_x = 0.0f;
    float scroll_y = 0.0f;
};

struct TextFieldParams {
    std::string name;
    TextFieldStyle style{};
    TextFieldState state{};
};

struct TextAreaParams {
    std::string name;
    TextAreaStyle style{};
    TextAreaState state{};
};

struct TextFieldPaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
};

struct TextAreaPaths {
    ScenePath scene;
    WidgetStateScenes states;
    WidgetPath root;
    ConcretePath state;
};

auto CreateTextField(PathSpace& space,
                     AppRootPathView appRoot,
                     TextFieldParams const& params) -> SP::Expected<TextFieldPaths>;

auto CreateTextArea(PathSpace& space,
                    AppRootPathView appRoot,
                    TextAreaParams const& params) -> SP::Expected<TextAreaPaths>;

auto UpdateTextFieldState(PathSpace& space,
                          TextFieldPaths const& paths,
                          TextFieldState const& new_state) -> SP::Expected<bool>;

auto UpdateTextAreaState(PathSpace& space,
                         TextAreaPaths const& paths,
                         TextAreaState const& new_state) -> SP::Expected<bool>;

enum class WidgetKind {
    Button,
    Toggle,
    Slider,
    List,
    Stack,
    Tree,
    TextField,
    TextArea,
    Label,
    InputField,
    PaintSurface,
};

struct HitTarget {
    WidgetPath widget;
    std::string component;
};

auto ResolveHitTarget(Scene::HitTestResult const& hit) -> std::optional<HitTarget>;

namespace Bindings {

using WidgetActionCallback = std::function<void(Reducers::WidgetAction const&)>;

enum class WidgetOpKind : std::uint32_t {
    HoverEnter = 0,
   HoverExit,
    Press,
    Release,
    Activate,
    Toggle,
    SliderBegin,
    SliderUpdate,
    SliderCommit,
    ListHover,
    ListSelect,
    ListActivate,
    ListScroll,
    TreeHover,
    TreeSelect,
    TreeToggle,
    TreeExpand,
    TreeCollapse,
    TreeRequestLoad,
    TreeScroll,
    TextHover,
    TextFocus,
    TextInput,
    TextDelete,
    TextMoveCursor,
    TextSetSelection,
    TextCompositionStart,
    TextCompositionUpdate,
    TextCompositionCommit,
    TextCompositionCancel,
    TextClipboardCopy,
    TextClipboardCut,
    TextClipboardPaste,
    TextScroll,
    TextSubmit,
    StackSelect,
    PaintStrokeBegin,
    PaintStrokeUpdate,
    PaintStrokeCommit,
};

struct PointerInfo {
    float scene_x = 0.0f;
    float scene_y = 0.0f;
    bool inside = false;
    bool primary = true;
    float local_x = 0.0f;
    float local_y = 0.0f;
    bool has_local = false;

    [[nodiscard]] static auto Make(float x, float y) -> PointerInfo {
        PointerInfo info{};
        info.scene_x = x;
        info.scene_y = y;
        return info;
    }

    auto WithInside(bool value) & -> PointerInfo& {
        inside = value;
        return *this;
    }

    auto WithInside(bool value) && -> PointerInfo {
        inside = value;
        return std::move(*this);
    }

    auto WithPrimary(bool value) & -> PointerInfo& {
        primary = value;
        return *this;
    }

    auto WithPrimary(bool value) && -> PointerInfo {
        primary = value;
        return std::move(*this);
    }

    auto WithLocal(float x, float y) & -> PointerInfo& {
        local_x = x;
        local_y = y;
        has_local = true;
        return *this;
    }

    auto WithLocal(float x, float y) && -> PointerInfo {
        local_x = x;
        local_y = y;
        has_local = true;
        return std::move(*this);
    }
};

struct WidgetOp {
    WidgetOpKind kind = WidgetOpKind::HoverEnter;
    std::string widget_path;
    std::string target_id;
    PointerInfo pointer;
    float value = 0.0f;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
};

struct BindingOptions {
    ConcretePath target;
    ConcretePath ops_queue;
    DirtyRectHint dirty_rect;
    bool auto_render = true;
    ConcretePath focus_state;
    bool focus_enabled = false;
    std::vector<std::shared_ptr<WidgetActionCallback>> action_callbacks;
};

struct ButtonBinding {
    ButtonPaths widget;
    BindingOptions options;
};

struct ToggleBinding {
    TogglePaths widget;
    BindingOptions options;
};

struct SliderBinding {
    SliderPaths widget;
    BindingOptions options;
};

struct ListBinding {
    ListPaths widget;
    BindingOptions options;
};

struct TreeBinding {
    TreePaths widget;
    BindingOptions options;
};

struct StackBinding {
    StackPaths layout;
    BindingOptions options;
};

struct TextFieldBinding {
    TextFieldPaths widget;
    BindingOptions options;
};

struct TextAreaBinding {
    TextAreaPaths widget;
    BindingOptions options;
};

auto CreateButtonBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         ButtonPaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override = std::nullopt,
                         bool auto_render = true) -> SP::Expected<ButtonBinding>;

auto CreateToggleBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         TogglePaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override = std::nullopt,
                         bool auto_render = true) -> SP::Expected<ToggleBinding>;

auto CreateSliderBinding(PathSpace& space,
                         AppRootPathView appRoot,
                         SliderPaths const& paths,
                         ConcretePathView targetPath,
                         DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override = std::nullopt,
                         bool auto_render = true) -> SP::Expected<SliderBinding>;

auto DispatchButton(PathSpace& space,
                    ButtonBinding const& binding,
                    ButtonState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer = {}) -> SP::Expected<bool>;

auto DispatchToggle(PathSpace& space,
                    ToggleBinding const& binding,
                    ToggleState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer = {}) -> SP::Expected<bool>;

auto DispatchSlider(PathSpace& space,
                    SliderBinding const& binding,
                    SliderState const& new_state,
                    WidgetOpKind op_kind,
                    PointerInfo const& pointer = {}) -> SP::Expected<bool>;

auto CreateListBinding(PathSpace& space,
                       AppRootPathView appRoot,
                       ListPaths const& paths,
                       ConcretePathView targetPath,
                       DirtyRectHint footprint,
                       std::optional<DirtyRectHint> dirty_override = std::nullopt,
                       bool auto_render = true) -> SP::Expected<ListBinding>;

auto CreateTreeBinding(PathSpace& space,
                       AppRootPathView appRoot,
                       TreePaths const& paths,
                       ConcretePathView targetPath,
                       DirtyRectHint footprint,
                       std::optional<DirtyRectHint> dirty_override = std::nullopt,
                       bool auto_render = true) -> SP::Expected<TreeBinding>;

auto CreateStackBinding(PathSpace& space,
                        AppRootPathView appRoot,
                        StackPaths const& paths,
                        ConcretePathView targetPath,
                        DirtyRectHint footprint,
                         std::optional<DirtyRectHint> dirty_override = std::nullopt,
                         bool auto_render = true) -> SP::Expected<StackBinding>;

auto CreateTextFieldBinding(PathSpace& space,
                            AppRootPathView appRoot,
                            TextFieldPaths const& paths,
                            ConcretePathView targetPath,
                            DirtyRectHint footprint,
                            std::optional<DirtyRectHint> dirty_override = std::nullopt,
                            bool auto_render = true) -> SP::Expected<TextFieldBinding>;

auto CreateTextAreaBinding(PathSpace& space,
                           AppRootPathView appRoot,
                           TextAreaPaths const& paths,
                           ConcretePathView targetPath,
                           DirtyRectHint footprint,
                           std::optional<DirtyRectHint> dirty_override = std::nullopt,
                           bool auto_render = true) -> SP::Expected<TextAreaBinding>;

namespace ActionCallbacks {

inline auto add_action_callback(BindingOptions& options,
                                WidgetActionCallback callback) -> void {
    if (!callback) {
        return;
    }
    options.action_callbacks.emplace_back(
        std::make_shared<WidgetActionCallback>(std::move(callback)));
}

inline auto clear_action_callbacks(BindingOptions& options) -> void {
    options.action_callbacks.clear();
}

} // namespace ActionCallbacks

inline auto AddActionCallback(ButtonBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(ToggleBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(SliderBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(ListBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(TreeBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(TextFieldBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto AddActionCallback(TextAreaBinding& binding,
                              WidgetActionCallback callback) -> void {
    ActionCallbacks::add_action_callback(binding.options, std::move(callback));
}

inline auto ClearActionCallbacks(ButtonBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(ToggleBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(SliderBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(ListBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(TreeBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(TextFieldBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

inline auto ClearActionCallbacks(TextAreaBinding& binding) -> void {
    ActionCallbacks::clear_action_callbacks(binding.options);
}

auto DispatchList(PathSpace& space,
                  ListBinding const& binding,
                  ListState const& new_state,
                  WidgetOpKind op_kind,
                  PointerInfo const& pointer = {},
                  std::int32_t item_index = -1,
                  float scroll_delta = 0.0f) -> SP::Expected<bool>;

auto DispatchTree(PathSpace& space,
                  TreeBinding const& binding,
                  TreeState const& new_state,
                  WidgetOpKind op_kind,
                  std::string_view node_id = {},
                  PointerInfo const& pointer = {},
                  float scroll_delta = 0.0f) -> SP::Expected<bool>;

auto DispatchTextField(PathSpace& space,
                       TextFieldBinding const& binding,
                       TextFieldState const& new_state,
                       WidgetOpKind op_kind,
                       PointerInfo const& pointer = {}) -> SP::Expected<bool>;

auto DispatchTextArea(PathSpace& space,
                      TextAreaBinding const& binding,
                      TextAreaState const& new_state,
                      WidgetOpKind op_kind,
                      PointerInfo const& pointer = {},
                      float scroll_delta_y = 0.0f) -> SP::Expected<bool>;

auto UpdateStack(PathSpace& space,
                 StackBinding const& binding,
                 StackLayoutParams const& params) -> SP::Expected<bool>;

auto PointerFromHit(Scene::HitTestResult const& hit) -> PointerInfo;

} // namespace Bindings

namespace Focus {

enum class Direction { Forward, Backward };

struct Config {
    ConcretePath focus_state;
    std::optional<ConcretePath> auto_render_target;
    std::optional<bool> pulsing_highlight;
};

struct FocusTransitionInfo {
    bool wrapped = false;
};

struct UpdateResult {
    WidgetPath widget;
    bool changed = false;
};

auto FocusStatePath(AppRootPathView appRoot) -> ConcretePath;

auto MakeConfig(AppRootPathView appRoot,
                std::optional<ConcretePath> auto_render_target = std::nullopt,
                std::optional<bool> pulsing_highlight = std::nullopt) -> Config;

auto Current(PathSpace const& space,
             ConcretePathView focus_state) -> SP::Expected<std::optional<std::string>>;

auto Set(PathSpace& space,
         Config const& config,
         WidgetPath const& widget,
         std::optional<FocusTransitionInfo> telemetry = std::nullopt) -> SP::Expected<UpdateResult>;

auto Clear(PathSpace& space,
           Config const& config) -> SP::Expected<bool>;

auto BuildWindowOrder(PathSpace& space,
                      WindowPath const& window_path) -> SP::Expected<std::vector<WidgetPath>>;

auto Move(PathSpace& space,
          Config const& config,
          std::span<WidgetPath const> order,
          Direction direction) -> SP::Expected<std::optional<UpdateResult>>;

auto Move(PathSpace& space,
          Config const& config,
          Direction direction) -> SP::Expected<std::optional<UpdateResult>>;

auto ApplyHit(PathSpace& space,
              Config const& config,
              Scene::HitTestResult const& hit) -> SP::Expected<std::optional<UpdateResult>>;

auto SetPulsingHighlight(PathSpace& space,
                         AppRootPathView appRoot,
                         bool enabled) -> SP::Expected<void>;

auto PulsingHighlightEnabled(PathSpace& space,
                             AppRootPathView appRoot) -> SP::Expected<bool>;

} // namespace Focus

namespace Input {

struct WidgetBounds {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    constexpr void normalize() {
        if (max_x < min_x) {
            std::swap(max_x, min_x);
        }
        if (max_y < min_y) {
            std::swap(max_y, min_y);
        }
    }

    [[nodiscard]] constexpr auto width() const -> float {
        return std::max(0.0f, max_x - min_x);
    }

    [[nodiscard]] constexpr auto height() const -> float {
        return std::max(0.0f, max_y - min_y);
    }

    [[nodiscard]] constexpr auto contains(float x, float y) const -> bool {
        return x >= min_x && x <= max_x && y >= min_y && y <= max_y;
    }

    constexpr auto include(WidgetBounds const& other) -> void {
        WidgetBounds normalized_other = other;
        normalized_other.normalize();
        if (!std::isfinite(min_x) || !std::isfinite(min_y)
            || !std::isfinite(max_x) || !std::isfinite(max_y)) {
            *this = normalized_other;
            return;
        }
        min_x = std::min(min_x, normalized_other.min_x);
        min_y = std::min(min_y, normalized_other.min_y);
        max_x = std::max(max_x, normalized_other.max_x);
        max_y = std::max(max_y, normalized_other.max_y);
    }

    [[nodiscard]] constexpr auto is_valid() const -> bool {
        return std::isfinite(min_x) && std::isfinite(min_y)
            && std::isfinite(max_x) && std::isfinite(max_y)
            && max_x >= min_x && max_y >= min_y;
    }
};

struct SliderLayout {
    WidgetBounds bounds{};
    WidgetBounds track{};
};

struct ListLayout {
    WidgetBounds bounds{};
    std::vector<WidgetBounds> item_bounds;
    float content_top = 0.0f;
    float item_height = 0.0f;
};

struct TreeRowLayout {
    WidgetBounds bounds{};
    WidgetBounds toggle{};
    std::string node_id;
    std::string label;
    int depth = 0;
    bool expandable = false;
    bool expanded = false;
    bool loading = false;
    bool enabled = true;
};

struct TreeLayout {
    WidgetBounds bounds{};
    float content_top = 0.0f;
    float row_height = 0.0f;
    std::vector<TreeRowLayout> rows;
};

struct LayoutSnapshot {
    WidgetBounds button{};
    WidgetBounds button_footprint{};
    WidgetBounds toggle{};
    WidgetBounds toggle_footprint{};
    std::optional<SliderLayout> slider{};
    WidgetBounds slider_footprint{};
    std::optional<ListLayout> list{};
    WidgetBounds list_footprint{};
    std::optional<TreeLayout> tree{};
    WidgetBounds tree_footprint{};
};

enum class FocusTarget {
    Button,
    Toggle,
    Slider,
    List,
    Tree,
};

struct FocusBindings {
    Focus::Config* config = nullptr;
    FocusTarget* current = nullptr;
    std::span<FocusTarget const> order{};
    std::optional<WidgetPath> button{};
    std::optional<WidgetPath> toggle{};
    std::optional<WidgetPath> slider{};
    std::optional<WidgetPath> list{};
    std::optional<WidgetPath> tree{};
    int* focus_list_index = nullptr;
    int* focus_tree_index = nullptr;
};

struct WidgetInputContext {
    PathSpace* space = nullptr;
    LayoutSnapshot layout{};
    FocusBindings focus{};
    Bindings::ButtonBinding* button_binding = nullptr;
    Widgets::ButtonPaths const* button_paths = nullptr;
    Widgets::ButtonState* button_state = nullptr;
    Bindings::ToggleBinding* toggle_binding = nullptr;
    Widgets::TogglePaths const* toggle_paths = nullptr;
    Widgets::ToggleState* toggle_state = nullptr;
    Bindings::SliderBinding* slider_binding = nullptr;
    Widgets::SliderPaths const* slider_paths = nullptr;
    Widgets::SliderState* slider_state = nullptr;
    Widgets::SliderStyle const* slider_style = nullptr;
    Widgets::SliderRange const* slider_range = nullptr;
    Bindings::ListBinding* list_binding = nullptr;
    Widgets::ListPaths const* list_paths = nullptr;
    Widgets::ListState* list_state = nullptr;
    Widgets::ListStyle const* list_style = nullptr;
    std::vector<Widgets::ListItem>* list_items = nullptr;
    Bindings::TreeBinding* tree_binding = nullptr;
    Widgets::TreePaths const* tree_paths = nullptr;
    Widgets::TreeState* tree_state = nullptr;
    Widgets::TreeStyle const* tree_style = nullptr;
    std::vector<Widgets::TreeNode>* tree_nodes = nullptr;
    float* pointer_x = nullptr;
    float* pointer_y = nullptr;
    bool* pointer_down = nullptr;
    bool* slider_dragging = nullptr;
    std::string* tree_pointer_down_id = nullptr;
    bool* tree_pointer_toggle = nullptr;
};

struct InputUpdate {
    bool state_changed = false;
    bool focus_changed = false;
};

struct SliderStepOptions {
    float percent_of_range = 0.05f;
    float minimum_step = 0.0f;
    bool respect_range_step = true;
};

struct SliderAnalogOptions {
    SliderStepOptions step_options{};
    float deadzone = 0.1f;
    float scale = 1.0f;
};

auto HandlePointerMove(WidgetInputContext& ctx, float x, float y) -> InputUpdate;
auto HandlePointerDown(WidgetInputContext& ctx) -> InputUpdate;
auto HandlePointerUp(WidgetInputContext& ctx) -> InputUpdate;
auto HandlePointerWheel(WidgetInputContext& ctx, int wheel_delta) -> InputUpdate;

auto RefreshFocusTargetFromSpace(WidgetInputContext& ctx) -> bool;
auto SetFocusTarget(WidgetInputContext& ctx,
                    FocusTarget target,
                    bool update_visuals = true) -> InputUpdate;
auto CycleFocus(WidgetInputContext& ctx, bool forward) -> InputUpdate;
auto ActivateFocusedWidget(WidgetInputContext& ctx) -> InputUpdate;
auto MoveListFocus(WidgetInputContext& ctx, int direction) -> InputUpdate;
auto MoveTreeFocus(WidgetInputContext& ctx, int direction) -> InputUpdate;
auto TreeApplyOp(WidgetInputContext& ctx, Bindings::WidgetOpKind op) -> InputUpdate;
auto AdjustSliderValue(WidgetInputContext& ctx, float delta) -> InputUpdate;
auto SliderStep(WidgetInputContext const& ctx,
                SliderStepOptions const& options = {}) -> float;
auto AdjustSliderByStep(WidgetInputContext& ctx,
                        int steps,
                        SliderStepOptions const& options = {}) -> InputUpdate;
auto AdjustSliderAnalog(WidgetInputContext& ctx,
                        float axis_value,
                        SliderAnalogOptions const& options = {}) -> InputUpdate;

// Build pointer metadata for keyboard/gamepad driven widget interactions.
auto ProgrammaticPointer(float scene_x, float scene_y, bool inside = true) -> Bindings::PointerInfo;

auto SliderPointerForValue(WidgetInputContext const& ctx, float value) -> std::pair<float, float>;
auto SliderThumbPosition(WidgetInputContext const& ctx, float value) -> std::pair<float, float>;
auto ListItemCenter(WidgetInputContext const& ctx, int index) -> std::pair<float, float>;
auto TreeRowCenter(WidgetInputContext const& ctx, int index) -> std::pair<float, float>;
auto TreeParentIndex(WidgetInputContext const& ctx, int index) -> int;

auto BoundsFromRect(Widgets::ListPreviewRect const& rect) -> WidgetBounds;
auto BoundsFromRect(Widgets::TreePreviewRect const& rect) -> WidgetBounds;
auto BoundsFromRect(Widgets::TreePreviewRect const& rect,
                    float dx,
                    float dy) -> WidgetBounds;

auto MakeListLayout(Widgets::ListPreviewLayout const& layout) -> std::optional<ListLayout>;
auto MakeTreeLayout(Widgets::TreePreviewLayout const& layout) -> std::optional<TreeLayout>;

auto ExpandForFocusHighlight(WidgetBounds& bounds) -> void;
auto FocusHighlightPadding() -> float;
auto MakeDirtyHint(WidgetBounds const& bounds) -> Builders::DirtyRectHint;
auto TranslateTreeLayout(TreeLayout& layout, float dx, float dy) -> void;

} // namespace Input

inline constexpr std::array<std::array<float, 4>, 6> kDefaultPaletteSwatches{{
    {0.905f, 0.173f, 0.247f, 1.0f},
    {0.972f, 0.545f, 0.192f, 1.0f},
    {0.995f, 0.847f, 0.207f, 1.0f},
    {0.172f, 0.701f, 0.368f, 1.0f},
    {0.157f, 0.407f, 0.933f, 1.0f},
    {0.560f, 0.247f, 0.835f, 1.0f},
}};

struct WidgetTheme {
    ButtonStyle button{};
    ToggleStyle toggle{};
    SliderStyle slider{};
    ListStyle list{};
    TreeStyle tree{};
    TextFieldStyle text_field{};
    TextAreaStyle text_area{};
    TypographyStyle heading{
        .font_size = 32.0f,
        .line_height = 36.0f,
        .letter_spacing = 1.0f,
        .baseline_shift = 0.0f,
    };
    TypographyStyle caption{
        .font_size = 24.0f,
        .line_height = 28.0f,
        .letter_spacing = 1.0f,
        .baseline_shift = 0.0f,
    };
    std::array<float, 4> heading_color{0.93f, 0.95f, 0.98f, 1.0f};
    std::array<float, 4> caption_color{0.90f, 0.92f, 0.96f, 1.0f};
    std::array<float, 4> accent_text_color{0.85f, 0.88f, 0.95f, 1.0f};
    std::array<float, 4> muted_text_color{0.70f, 0.72f, 0.78f, 1.0f};
    std::array<float, 4> palette_text_on_light{0.10f, 0.12f, 0.16f, 1.0f};
    std::array<float, 4> palette_text_on_dark{1.0f, 1.0f, 1.0f, 1.0f};
    std::array<std::array<float, 4>, 6> palette_swatches{kDefaultPaletteSwatches};
};

struct ThemeSelection {
    WidgetTheme theme{};
    std::string canonical_name;
    bool recognized = true;
};

auto MakeDefaultWidgetTheme() -> WidgetTheme;
auto MakeSunsetWidgetTheme() -> WidgetTheme;
auto LoadTheme(PathSpace& space,
               AppRootPathView appRoot,
               std::string_view requested_name) -> SP::Expected<ThemeSelection>;
auto ApplyTheme(WidgetTheme const& theme, ButtonParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, ToggleParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, SliderParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, ListParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, TreeParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, TextFieldParams& params) -> void;
auto ApplyTheme(WidgetTheme const& theme, TextAreaParams& params) -> void;

struct ButtonParamsBuilder {
    ButtonParams value{};

    static auto Make(std::string name, std::string label = {}) -> ButtonParamsBuilder {
        ButtonParamsBuilder builder;
        builder.value.name = std::move(name);
        builder.value.label = std::move(label);
        return builder;
    }

    auto WithName(std::string name) -> ButtonParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithLabel(std::string label) -> ButtonParamsBuilder& {
        value.label = std::move(label);
        return *this;
    }

    auto WithStyle(ButtonStyle style) -> ButtonParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> ButtonParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> ButtonParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> ButtonParams { return value; }
    auto Build() && -> ButtonParams { return std::move(value); }
};

struct ToggleParamsBuilder {
    ToggleParams value{};

    static auto Make(std::string name) -> ToggleParamsBuilder {
        ToggleParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> ToggleParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithStyle(ToggleStyle style) -> ToggleParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> ToggleParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> ToggleParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> ToggleParams { return value; }
    auto Build() && -> ToggleParams { return std::move(value); }
};

struct SliderParamsBuilder {
    SliderParams value{};

    static auto Make(std::string name) -> SliderParamsBuilder {
        SliderParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> SliderParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithMinimum(float minimum) -> SliderParamsBuilder& {
        value.minimum = minimum;
        return *this;
    }

    auto WithMaximum(float maximum) -> SliderParamsBuilder& {
        value.maximum = maximum;
        return *this;
    }

    auto WithValue(float current) -> SliderParamsBuilder& {
        value.value = current;
        return *this;
    }

    auto WithStep(float step) -> SliderParamsBuilder& {
        value.step = step;
        return *this;
    }

    auto WithRange(float minimum, float maximum) -> SliderParamsBuilder& {
        value.minimum = minimum;
        value.maximum = maximum;
        return *this;
    }

    auto WithStyle(SliderStyle style) -> SliderParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> SliderParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> SliderParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> SliderParams { return value; }
    auto Build() && -> SliderParams { return std::move(value); }
};

struct ListParamsBuilder {
    ListParams value{};

    static auto Make(std::string name) -> ListParamsBuilder {
        ListParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> ListParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithItems(std::vector<ListItem> items) -> ListParamsBuilder& {
        value.items = std::move(items);
        return *this;
    }

    auto AddItem(ListItem item) -> ListParamsBuilder& {
        value.items.push_back(std::move(item));
        return *this;
    }

    auto WithStyle(ListStyle style) -> ListParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> ListParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> ListParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> ListParams { return value; }
    auto Build() && -> ListParams { return std::move(value); }
};

struct TreeParamsBuilder {
    TreeParams value{};

    static auto Make(std::string name) -> TreeParamsBuilder {
        TreeParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> TreeParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithNodes(std::vector<TreeNode> nodes) -> TreeParamsBuilder& {
        value.nodes = std::move(nodes);
        return *this;
    }

    auto WithStyle(TreeStyle style) -> TreeParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> TreeParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithTheme(WidgetTheme const& theme) -> TreeParamsBuilder& {
        ApplyTheme(theme, value);
        return *this;
    }

    auto Build() const -> TreeParams { return value; }
    auto Build() && -> TreeParams { return std::move(value); }
};

struct StackLayoutParamsBuilder {
    StackLayoutParams value{};

    static auto Make(std::string name) -> StackLayoutParamsBuilder {
        StackLayoutParamsBuilder builder;
        builder.value.name = std::move(name);
        return builder;
    }

    auto WithName(std::string name) -> StackLayoutParamsBuilder& {
        value.name = std::move(name);
        return *this;
    }

    auto WithStyle(StackLayoutStyle style) -> StackLayoutParamsBuilder& {
        value.style = std::move(style);
        return *this;
    }

    template <typename Fn>
    auto ModifyStyle(Fn&& fn) -> StackLayoutParamsBuilder& {
        std::forward<Fn>(fn)(value.style);
        return *this;
    }

    auto WithChildren(std::vector<StackChildSpec> children) -> StackLayoutParamsBuilder& {
        value.children = std::move(children);
        return *this;
    }

    auto AddChild(StackChildSpec child) -> StackLayoutParamsBuilder& {
        value.children.push_back(std::move(child));
        return *this;
    }

    auto Build() const -> StackLayoutParams { return value; }
    auto Build() && -> StackLayoutParams { return std::move(value); }
};

struct ButtonStateBuilder {
    ButtonState value{};

    static auto Make() -> ButtonStateBuilder { return ButtonStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> ButtonStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithPressed(bool pressed = true) -> ButtonStateBuilder& {
        value.pressed = pressed;
        return *this;
    }

    auto WithHovered(bool hovered = true) -> ButtonStateBuilder& {
        value.hovered = hovered;
        return *this;
    }

    auto WithFocused(bool focused = true) -> ButtonStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto Build() const -> ButtonState { return value; }
    auto Build() && -> ButtonState { return std::move(value); }
};

struct ToggleStateBuilder {
    ToggleState value{};

    static auto Make() -> ToggleStateBuilder { return ToggleStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> ToggleStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithHovered(bool hovered = true) -> ToggleStateBuilder& {
        value.hovered = hovered;
        return *this;
    }

    auto WithChecked(bool checked = true) -> ToggleStateBuilder& {
        value.checked = checked;
        return *this;
    }

    auto WithFocused(bool focused = true) -> ToggleStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto Build() const -> ToggleState { return value; }
    auto Build() && -> ToggleState { return std::move(value); }
};

struct SliderStateBuilder {
    SliderState value{};

    static auto Make() -> SliderStateBuilder { return SliderStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> SliderStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithHovered(bool hovered = true) -> SliderStateBuilder& {
        value.hovered = hovered;
        return *this;
    }

    auto WithDragging(bool dragging = true) -> SliderStateBuilder& {
        value.dragging = dragging;
        return *this;
    }

    auto WithFocused(bool focused = true) -> SliderStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto WithValue(float current) -> SliderStateBuilder& {
        value.value = current;
        return *this;
    }

    auto Build() const -> SliderState { return value; }
    auto Build() && -> SliderState { return std::move(value); }
};

struct ListStateBuilder {
    ListState value{};

    static auto Make() -> ListStateBuilder { return ListStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> ListStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithFocused(bool focused = true) -> ListStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto WithHoveredIndex(std::int32_t index) -> ListStateBuilder& {
        value.hovered_index = index;
        return *this;
    }

    auto WithSelectedIndex(std::int32_t index) -> ListStateBuilder& {
        value.selected_index = index;
        return *this;
    }

    auto WithScrollOffset(float offset) -> ListStateBuilder& {
        value.scroll_offset = offset;
        return *this;
    }

    auto Build() const -> ListState { return value; }
    auto Build() && -> ListState { return std::move(value); }
};

struct TreeStateBuilder {
    TreeState value{};

    static auto Make() -> TreeStateBuilder { return TreeStateBuilder{}; }

    auto WithEnabled(bool enabled = true) -> TreeStateBuilder& {
        value.enabled = enabled;
        return *this;
    }

    auto WithFocused(bool focused = true) -> TreeStateBuilder& {
        value.focused = focused;
        return *this;
    }

    auto WithHoveredId(std::string id) -> TreeStateBuilder& {
        value.hovered_id = std::move(id);
        return *this;
    }

    auto WithSelectedId(std::string id) -> TreeStateBuilder& {
        value.selected_id = std::move(id);
        return *this;
    }

    auto WithExpandedIds(std::vector<std::string> ids) -> TreeStateBuilder& {
        value.expanded_ids = std::move(ids);
        return *this;
    }

    auto WithLoadingIds(std::vector<std::string> ids) -> TreeStateBuilder& {
        value.loading_ids = std::move(ids);
        return *this;
    }

    auto WithScrollOffset(float offset) -> TreeStateBuilder& {
        value.scroll_offset = offset;
        return *this;
    }

    auto Build() const -> TreeState { return value; }
    auto Build() && -> TreeState { return std::move(value); }
};

[[nodiscard]] inline auto MakeButtonParams(std::string name, std::string label = {}) -> ButtonParamsBuilder {
    return ButtonParamsBuilder::Make(std::move(name), std::move(label));
}

[[nodiscard]] inline auto MakeToggleParams(std::string name) -> ToggleParamsBuilder {
    return ToggleParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeSliderParams(std::string name) -> SliderParamsBuilder {
    return SliderParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeListParams(std::string name) -> ListParamsBuilder {
    return ListParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeTreeParams(std::string name) -> TreeParamsBuilder {
    return TreeParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeStackLayoutParams(std::string name) -> StackLayoutParamsBuilder {
    return StackLayoutParamsBuilder::Make(std::move(name));
}

[[nodiscard]] inline auto MakeButtonState() -> ButtonStateBuilder {
    return ButtonStateBuilder::Make();
}

[[nodiscard]] inline auto MakeToggleState() -> ToggleStateBuilder {
    return ToggleStateBuilder::Make();
}

[[nodiscard]] inline auto MakeSliderState() -> SliderStateBuilder {
    return SliderStateBuilder::Make();
}

[[nodiscard]] inline auto MakeListState() -> ListStateBuilder {
    return ListStateBuilder::Make();
}

[[nodiscard]] inline auto MakeTreeState() -> TreeStateBuilder {
    return TreeStateBuilder::Make();
}

namespace Reducers {

struct WidgetAction {
    Bindings::WidgetOpKind kind = Bindings::WidgetOpKind::HoverEnter;
    std::string widget_path;
    std::string target_id;
    Bindings::PointerInfo pointer{};
    float analog_value = 0.0f;
    std::int32_t discrete_index = -1;
    std::uint64_t sequence = 0;
    std::uint64_t timestamp_ns = 0;
};

auto MakeWidgetAction(Bindings::WidgetOp const& op) -> WidgetAction;

struct ProcessActionsResult {
    ConcretePath ops_queue;
    ConcretePath actions_queue;
    std::vector<WidgetAction> actions;
};

auto WidgetOpsQueue(WidgetPath const& widget_root) -> ConcretePath;

auto DefaultActionsQueue(WidgetPath const& widget_root) -> ConcretePath;

auto ReducePending(PathSpace& space,
                   ConcretePathView ops_queue,
                   std::size_t max_actions = std::numeric_limits<std::size_t>::max()) -> SP::Expected<std::vector<WidgetAction>>;

auto PublishActions(PathSpace& space,
                    ConcretePathView actions_queue,
                    std::span<WidgetAction const> actions) -> SP::Expected<void>;

auto ProcessPendingActions(PathSpace& space,
                           WidgetPath const& widget_root,
                           std::size_t max_actions = std::numeric_limits<std::size_t>::max()) -> SP::Expected<ProcessActionsResult>;

} // namespace Reducers

} // namespace Widgets

} // namespace SP::UI::Builders
