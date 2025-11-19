#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/WidgetEventTrellis.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace SP::UI::Declarative {

namespace BuilderWidgets = SP::UI::Builders::Widgets;
namespace WidgetBindings = SP::UI::Builders::Widgets::Bindings;
namespace BuildersScene = SP::UI::Builders::Scene;

inline constexpr std::string_view kWidgetEventsLogQueue =
    "/system/widgets/runtime/events/log/errors/queue";

enum class TargetKind {
    Unknown = 0,
    Button,
    Toggle,
    Slider,
    List,
    TreeRow,
    TreeToggle,
    InputField,
    StackPanel,
    PaintSurface
};

struct TargetInfo {
    std::string widget_path;
    std::string component;
    TargetKind kind = TargetKind::Unknown;
    std::optional<std::int32_t> list_index;
    std::optional<std::string> list_item_id;
    std::optional<std::string> tree_node_id;
    std::optional<std::string> stack_panel_id;
    float local_x = 0.0f;
    float local_y = 0.0f;
    bool has_local = false;

    [[nodiscard]] auto valid() const -> bool {
        return !widget_path.empty() && kind != TargetKind::Unknown;
    }
};

struct PointerState {
    float x = 0.0f;
    float y = 0.0f;
    bool have_position = false;
    bool primary_down = false;
    std::optional<TargetInfo> hover_target;
    std::optional<TargetInfo> active_target;
    std::optional<std::string> slider_active_widget;
    float slider_active_value = 0.0f;
    std::optional<std::string> list_press_widget;
    std::optional<std::int32_t> list_press_index;
    std::optional<std::string> list_hover_widget;
    std::optional<std::int32_t> list_hover_index;
    std::optional<std::string> tree_press_widget;
    std::optional<std::string> tree_press_node;
    bool tree_press_toggle = false;
    std::optional<std::string> tree_hover_widget;
    std::optional<std::string> tree_hover_node;
    std::optional<std::string> text_focus_widget;
    std::optional<std::string> stack_press_widget;
    std::optional<std::string> stack_press_panel;
    std::optional<std::string> paint_active_widget;
    std::optional<std::uint64_t> paint_active_stroke_id;
    std::uint64_t paint_stroke_sequence = 0;
    float paint_last_local_x = 0.0f;
    float paint_last_local_y = 0.0f;
    bool paint_has_last_local = false;
    std::optional<TargetInfo> focus_press_target;
};

struct WindowBinding {
    std::string token;
    std::string window_path;
    std::string app_root;
    std::string pointer_queue;
    std::string button_queue;
    std::string text_queue;
    std::string scene_path;
};

struct SliderData {
    BuilderWidgets::SliderState state;
    BuilderWidgets::SliderStyle style;
    BuilderWidgets::SliderRange range;
};

struct ListData {
    BuilderWidgets::ListState state;
    BuilderWidgets::ListStyle style;
    std::vector<BuilderWidgets::ListItem> items;
};

struct TreeData {
    BuilderWidgets::TreeState state;
    BuilderWidgets::TreeStyle style;
    std::vector<BuilderWidgets::TreeNode> nodes;
};

struct TreeRowInfo {
    std::string id;
    std::string parent_id;
    bool expandable = false;
    bool expanded = false;
    bool enabled = true;
    int depth = 0;
};

enum class FocusDirection {
    None = 0,
    Left,
    Right,
    Up,
    Down
};

enum class FocusCommand {
    None = 0,
    Submit,
    DeleteBackward,
    DeleteForward
};

struct FocusNavEvent {
    FocusDirection direction = FocusDirection::None;
    FocusCommand command = FocusCommand::None;
    bool pressed = false;
    bool repeat = false;
    bool from_keyboard = false;
    bool from_gamepad = false;
};

inline constexpr std::uint32_t kKeycodeLeft = 0x7B;
inline constexpr std::uint32_t kKeycodeRight = 0x7C;
inline constexpr std::uint32_t kKeycodeDown = 0x7D;
inline constexpr std::uint32_t kKeycodeUp = 0x7E;
inline constexpr std::uint32_t kKeycodeReturn = 0x24;
inline constexpr std::uint32_t kKeycodeEnter = 0x4C;
inline constexpr std::uint32_t kKeycodeDeleteBackward = 0x33;
inline constexpr std::uint32_t kKeycodeDeleteForward = 0x75;

inline constexpr int kGamepadButtonA = 0;
inline constexpr int kGamepadButtonB = 1;
inline constexpr int kGamepadLeftShoulder = 4;
inline constexpr int kGamepadRightShoulder = 5;
inline constexpr int kGamepadDpadUp = 12;
inline constexpr int kGamepadDpadDown = 13;
inline constexpr int kGamepadDpadLeft = 14;
inline constexpr int kGamepadDpadRight = 15;

auto now_ns() -> std::uint64_t;
auto enqueue_error(PathSpace& space, std::string const& message) -> void;
auto normalize_root(std::string root) -> std::string;
auto list_children(PathSpace& space, std::string const& path) -> std::vector<std::string>;
auto mark_widget_dirty(PathSpace& space, std::string const& widget_path) -> void;

auto read_slider_data(PathSpace& space, std::string const& widget_path)
    -> std::optional<SliderData>;
auto write_slider_state(PathSpace& space,
                        std::string const& widget_path,
                        BuilderWidgets::SliderState const& state) -> bool;
auto update_slider_hover(PathSpace& space,
                         std::string const& widget_path,
                         bool hovered) -> void;

auto read_list_data(PathSpace& space, std::string const& widget_path)
    -> std::optional<ListData>;
auto read_tree_data(PathSpace& space, std::string const& widget_path)
    -> std::optional<TreeData>;
auto build_tree_rows(TreeData const& data) -> std::vector<TreeRowInfo>;
auto tree_row_index(std::vector<TreeRowInfo> const& rows,
                    std::string const& node_id) -> std::optional<std::size_t>;

auto read_text_state(PathSpace& space, std::string const& widget_path)
    -> std::optional<BuilderWidgets::TextFieldState>;
auto write_text_state(PathSpace& space,
                      std::string const& widget_path,
                      BuilderWidgets::TextFieldState const& state) -> bool;

auto default_focus_pointer() -> WidgetBindings::PointerInfo;
auto focus_pointer_with_local(float local_x, float local_y) -> WidgetBindings::PointerInfo;

auto classify_focus_nav(SP::IO::ButtonEvent const& event) -> std::optional<FocusNavEvent>;

auto focused_widget_path(PathSpace& space,
                         WindowBinding const& binding) -> std::optional<std::string>;

auto slider_value_from_local(SliderData const& data, float local_x) -> float;
auto clamp_slider_value(SliderData const& data, float value) -> float;
auto slider_local_from_value(SliderData const& data, float value) -> float;
auto slider_step_size(SliderData const& data) -> float;
auto list_index_from_local(ListData const& data, float local_y) -> std::optional<std::int32_t>;
auto list_local_center(ListData const& data, std::int32_t index) -> std::pair<float, float>;
auto list_item_id(ListData const& data, std::int32_t index) -> std::optional<std::string>;

auto write_stack_active_panel(PathSpace& space,
                              std::string const& widget_path,
                              std::string const& panel_id) -> bool;

} // namespace SP::UI::Declarative
