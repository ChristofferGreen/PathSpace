#include <pathspace/ui/declarative/WidgetEventTrellis.hpp>

#include "../BuildersDetail.hpp"
#include "WidgetStateMutators.hpp"
#include "widgets/Common.hpp"

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/log/TaggedLogger.hpp>
#include <pathspace/runtime/IOPump.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SP::UI::Declarative {
namespace {

using namespace SP::UI::Builders;
using namespace SP::UI::Builders::Detail;
namespace WidgetBindings = SP::UI::Builders::Widgets::Bindings;
namespace BuilderWidgets = SP::UI::Builders::Widgets;
namespace BuildersScene = SP::UI::Builders::Scene;
namespace DeclarativeDetail = SP::UI::Declarative::Detail;

constexpr std::string_view kLogErrorsQueue = "/system/widgets/runtime/events/log/errors/queue";

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

auto now_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

auto enqueue_error(PathSpace& space, std::string const& message) -> void {
    auto inserted = space.insert(std::string{kLogErrorsQueue}, message);
    (void)inserted;
}

auto normalize_root(std::string root) -> std::string {
    if (root.empty()) {
        return std::string{"/"};
    }
    while (!root.empty() && root.back() == '/') {
        root.pop_back();
    }
    if (root.empty()) {
        return std::string{"/"};
    }
    return root;
}

auto list_children(PathSpace& space, std::string const& path) -> std::vector<std::string> {
    SP::ConcretePathStringView view{path};
    return space.listChildren(view);
}

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


auto mark_widget_dirty(PathSpace& space, std::string const& widget_path) -> void {
    auto status = DeclarativeDetail::mark_render_dirty(space, widget_path);
    if (!status) {
        enqueue_error(space,
                      "WidgetEventTrellis failed to mark render dirty for " + widget_path);
    }
}

template <typename State, typename MutateFn>
auto mutate_widget_state(PathSpace& space,
                         std::string const& widget_path,
                         std::string_view state_name,
                         MutateFn&& mutate) -> bool {
    auto state_path = widget_path + "/state";
    State updated{};
    auto stored = space.read<State, std::string>(state_path);
    if (!stored) {
        auto const& error = stored.error();
        if (error.code != SP::Error::Code::NoObjectFound
            && error.code != SP::Error::Code::NoSuchPath) {
            enqueue_error(space,
                          "WidgetEventTrellis failed to read "
                              + std::string(state_name) + " for " + widget_path);
            return false;
        }
    } else {
        updated = *stored;
    }
    bool changed = mutate(updated);
    if (!changed) {
        return true;
    }
    auto status = replace_single<State>(space, state_path, updated);
    if (!status) {
        enqueue_error(space,
                      "WidgetEventTrellis failed to write "
                          + std::string(state_name) + " for " + widget_path);
        return false;
    }
    mark_widget_dirty(space, widget_path);
    return true;
}

} // namespace

namespace Detail {

auto SetButtonHovered(PathSpace& space,
                      std::string const& widget_path,
                      bool hovered) -> void {
    (void)mutate_widget_state<BuilderWidgets::ButtonState>(space,
                                                           widget_path,
                                                           "button state",
                                                           [hovered](auto& state) {
                                                               if (!state.enabled
                                                                   || state.hovered == hovered) {
                                                                   return false;
                                                               }
                                                               state.hovered = hovered;
                                                               if (!hovered) {
                                                                   state.pressed = false;
                                                               }
                                                               return true;
                                                           });
}

auto SetButtonPressed(PathSpace& space,
                      std::string const& widget_path,
                      bool pressed) -> void {
    (void)mutate_widget_state<BuilderWidgets::ButtonState>(space,
                                                           widget_path,
                                                           "button state",
                                                           [pressed](auto& state) {
                                                               if (!state.enabled
                                                                   || state.pressed == pressed) {
                                                                   return false;
                                                               }
                                                               state.pressed = pressed;
                                                               return true;
                                                           });
}

auto SetToggleHovered(PathSpace& space,
                      std::string const& widget_path,
                      bool hovered) -> void {
    (void)mutate_widget_state<BuilderWidgets::ToggleState>(space,
                                                           widget_path,
                                                           "toggle state",
                                                           [hovered](auto& state) {
                                                               if (!state.enabled
                                                                   || state.hovered == hovered) {
                                                                   return false;
                                                               }
                                                               state.hovered = hovered;
                                                               return true;
                                                           });
}

auto ToggleToggleChecked(PathSpace& space, std::string const& widget_path) -> void {
    (void)mutate_widget_state<BuilderWidgets::ToggleState>(space,
                                                           widget_path,
                                                           "toggle state",
                                                           [](auto& state) {
                                                               if (!state.enabled) {
                                                                   return false;
                                                               }
                                                               state.checked = !state.checked;
                                                               return true;
                                                           });
}

auto SetListHoverIndex(PathSpace& space,
                       std::string const& widget_path,
                       std::optional<std::int32_t> index) -> void {
    (void)mutate_widget_state<BuilderWidgets::ListState>(space,
                                                         widget_path,
                                                         "list state",
                                                         [index](auto& state) {
                                                             auto desired = index.value_or(-1);
                                                             if (state.hovered_index == desired) {
                                                                 return false;
                                                             }
                                                             state.hovered_index = desired;
                                                             return true;
                                                         });
}

auto SetListSelectionIndex(PathSpace& space,
                           std::string const& widget_path,
                           std::int32_t index) -> void {
    (void)mutate_widget_state<BuilderWidgets::ListState>(space,
                                                         widget_path,
                                                         "list state",
                                                         [index](auto& state) {
                                                             if (!state.enabled
                                                                 || state.selected_index == index) {
                                                                 return false;
                                                             }
                                                             state.selected_index = index;
                                                             return true;
                                                         });
}

auto SetTreeHoveredNode(PathSpace& space,
                        std::string const& widget_path,
                        std::optional<std::string> node_id) -> void {
    (void)mutate_widget_state<BuilderWidgets::TreeState>(space,
                                                         widget_path,
                                                         "tree state",
                                                         [&node_id](auto& state) {
                                                             auto desired = node_id.value_or(std::string{});
                                                             if (state.hovered_id == desired) {
                                                                 return false;
                                                             }
                                                             state.hovered_id = desired;
                                                             return true;
                                                         });
}

auto SetTreeSelectedNode(PathSpace& space,
                         std::string const& widget_path,
                         std::string const& node_id) -> void {
    (void)mutate_widget_state<BuilderWidgets::TreeState>(space,
                                                         widget_path,
                                                         "tree state",
                                                         [&node_id](auto& state) {
                                                             if (!state.enabled
                                                                 || state.selected_id == node_id) {
                                                                 return false;
                                                             }
                                                             state.selected_id = node_id;
                                                             return true;
                                                         });
}

auto ToggleTreeExpanded(PathSpace& space,
                        std::string const& widget_path,
                        std::string const& node_id) -> void {
    (void)mutate_widget_state<BuilderWidgets::TreeState>(space,
                                                         widget_path,
                                                         "tree state",
                                                         [&node_id](auto& state) {
                                                             if (!state.enabled) {
                                                                 return false;
                                                             }
                                                             auto it = std::find(state.expanded_ids.begin(),
                                                                                state.expanded_ids.end(),
                                                                                node_id);
                                                             if (it == state.expanded_ids.end()) {
                                                                 state.expanded_ids.push_back(node_id);
                                                             } else {
                                                                 state.expanded_ids.erase(it);
                                                             }
                                                             return true;
                                                         });
}

} // namespace Detail

namespace {

auto read_slider_data(PathSpace& space, std::string const& widget_path)
    -> std::optional<SliderData> {
    SliderData data{};
    auto state = space.read<BuilderWidgets::SliderState, std::string>(widget_path + "/state");
    if (!state) {
        enqueue_error(space, "WidgetEventTrellis failed to read slider state for " + widget_path);
        return std::nullopt;
    }
    data.state = *state;
    auto style = space.read<BuilderWidgets::SliderStyle, std::string>(widget_path + "/meta/style");
    if (!style) {
        enqueue_error(space, "WidgetEventTrellis failed to read slider style for " + widget_path);
        return std::nullopt;
    }
    data.style = *style;
    auto range = space.read<BuilderWidgets::SliderRange, std::string>(widget_path + "/meta/range");
    if (!range) {
        enqueue_error(space, "WidgetEventTrellis failed to read slider range for " + widget_path);
        return std::nullopt;
    }
    data.range = *range;
    return data;
}

auto write_slider_state(PathSpace& space,
                        std::string const& widget_path,
                        BuilderWidgets::SliderState const& state) -> bool {
    auto status = replace_single<BuilderWidgets::SliderState>(space,
                                                             widget_path + "/state",
                                                             state);
    if (!status) {
        enqueue_error(space,
                      "WidgetEventTrellis failed to write slider state for " + widget_path);
        return false;
    }
    mark_widget_dirty(space, widget_path);
    return true;
}

auto read_list_data(PathSpace& space, std::string const& widget_path)
    -> std::optional<ListData> {
    ListData data{};
    auto state = space.read<BuilderWidgets::ListState, std::string>(widget_path + "/state");
    if (!state) {
        enqueue_error(space, "WidgetEventTrellis failed to read list state for " + widget_path);
        return std::nullopt;
    }
    data.state = *state;
    auto style = space.read<BuilderWidgets::ListStyle, std::string>(widget_path + "/meta/style");
    if (!style) {
        enqueue_error(space, "WidgetEventTrellis failed to read list style for " + widget_path);
        return std::nullopt;
    }
    data.style = *style;
    auto items = space.read<std::vector<BuilderWidgets::ListItem>, std::string>(widget_path + "/meta/items");
    if (!items) {
        enqueue_error(space, "WidgetEventTrellis failed to read list items for " + widget_path);
        return std::nullopt;
    }
    data.items = *items;
    return data;
}

auto update_slider_hover(PathSpace& space,
                         std::string const& widget_path,
                         bool hovered) -> void {
    auto data = read_slider_data(space, widget_path);
    if (!data) {
        return;
    }
    if (data->state.hovered == hovered) {
        return;
    }
    data->state.hovered = hovered;
    write_slider_state(space, widget_path, data->state);
}


auto clamp_slider_value(BuilderWidgets::SliderRange const& range, float value) -> float {
    float minimum = std::min(range.minimum, range.maximum);
    float maximum = std::max(range.minimum, range.maximum);
    if (minimum == maximum) {
        maximum = minimum + 1.0f;
    }
    float clamped = std::clamp(value, minimum, maximum);
    if (range.step > 0.0f) {
        float steps = std::round((clamped - minimum) / range.step);
        clamped = minimum + steps * range.step;
        clamped = std::clamp(clamped, minimum, maximum);
    }
    return clamped;
}

auto slider_value_from_local(SliderData const& data, float local_x) -> float {
    float width = std::max(data.style.width, 1.0f);
    float clamped_x = std::clamp(local_x, 0.0f, width);
    float progress = clamped_x / width;
    float value = data.range.minimum + (data.range.maximum - data.range.minimum) * progress;
    return clamp_slider_value(data.range, value);
}

auto slider_thumb_radius(SliderData const& data) -> float {
    return std::clamp(data.style.thumb_radius,
                      data.style.track_height * 0.5f,
                      data.style.height * 0.5f);
}

auto slider_hover_from_local(SliderData const& data, float local_x, float local_y) -> bool {
    float height = std::max(data.style.height, 1.0f);
    float thumb_r = slider_thumb_radius(data);
    float thumb_x = (data.state.value - data.range.minimum)
        / std::max(data.range.maximum - data.range.minimum, 1e-6f);
    thumb_x = std::clamp(thumb_x, 0.0f, 1.0f);
    float thumb_center_x = thumb_x * std::max(data.style.width, 1.0f);
    float thumb_center_y = height * 0.5f;
    float dx = local_x - thumb_center_x;
    float dy = local_y - thumb_center_y;
    return (dx * dx + dy * dy) <= (thumb_r * thumb_r);
}

auto list_index_from_local(ListData const& data, float local_y) -> std::optional<std::int32_t> {
    if (data.items.empty()) {
        return std::nullopt;
    }
    float row_height = std::max(data.style.item_height, 1.0f);
    float relative = local_y + data.state.scroll_offset - data.style.border_thickness;
    if (relative < 0.0f) {
        return std::nullopt;
    }
    auto index = static_cast<std::int32_t>(std::floor(relative / row_height));
    if (index < 0 || index >= static_cast<std::int32_t>(data.items.size())) {
        return std::nullopt;
    }
    return index;
}

auto list_item_id(ListData const& data, std::int32_t index) -> std::optional<std::string> {
    if (index < 0 || index >= static_cast<std::int32_t>(data.items.size())) {
        return std::nullopt;
    }
    return data.items[static_cast<std::size_t>(index)].id;
}

auto focused_widget_path(PathSpace& space,
                         WindowBinding const& binding) -> std::optional<std::string> {
    if (!binding.app_root.empty()) {
        auto app_focus = space.read<std::string, std::string>(binding.app_root + "/widgets/focus/current");
        if (app_focus && !app_focus->empty()) {
            return *app_focus;
        }
    }

    auto component = window_component_for(binding.window_path);
    if (!component) {
        enqueue_error(space, "WidgetEventTrellis failed to derive window component for focus path");
        return std::nullopt;
    }
    auto make_focus_path = [&](std::string const& root) {
        return root + "/structure/window/" + *component + "/focus/current";
    };

    auto read_focus = [&](std::string const& path) -> std::optional<std::string> {
        auto value = space.read<std::string, std::string>(path);
        if (!value || value->empty()) {
            return std::nullopt;
        }
        return *value;
    };

    if (!binding.scene_path.empty()) {
        if (auto value = read_focus(make_focus_path(binding.scene_path))) {
            return value;
        }
    }
    return std::nullopt;
}

class WidgetEventTrellisWorker : public std::enable_shared_from_this<WidgetEventTrellisWorker> {
public:
    WidgetEventTrellisWorker(PathSpace& space, WidgetEventTrellisOptions options)
        : space_(space)
        , options_(std::move(options))
        , windows_root_(normalize_root(options_.windows_root))
        , events_root_(normalize_root(options_.events_root))
        , metrics_root_(options_.metrics_root.empty()
                  ? std::string{"/system/widgets/runtime/events/metrics"}
                  : options_.metrics_root)
        , log_root_(options_.log_root.empty()
              ? std::string{"/system/widgets/runtime/events/log"}
              : options_.log_root)
        , state_path_(options_.state_path.empty()
              ? std::string{"/system/widgets/runtime/events/state/running"}
              : options_.state_path) {}

    ~WidgetEventTrellisWorker() {
        stop();
    }

    auto start() -> SP::Expected<void> {
        if (auto ensured = ensure_roots(); !ensured) {
            return ensured;
        }
        worker_ = std::thread([self = shared_from_this()] { self->run(); });
        return {};
    }

    void stop() {
        bool expected = false;
        if (!stop_requested_.compare_exchange_strong(expected, true)) {
            return;
        }
        stop_flag_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    auto ensure_roots() -> SP::Expected<void> {
        if (auto status = replace_single<bool>(space_, state_path_, false); !status) {
            return status;
        }
        auto pointer_path = metrics_root_ + "/pointer_events_total";
        auto button_path = metrics_root_ + "/button_events_total";
        auto ops_path = metrics_root_ + "/widget_ops_total";
        auto hit_path = metrics_root_ + "/hit_test_failures_total";
        auto dispatch_path = metrics_root_ + "/last_dispatch_ns";
        if (auto status = replace_single<std::uint64_t>(space_, pointer_path, 0); !status) {
            return status;
        }
        if (auto status = replace_single<std::uint64_t>(space_, button_path, 0); !status) {
            return status;
        }
        if (auto status = replace_single<std::uint64_t>(space_, ops_path, 0); !status) {
            return status;
        }
        if (auto status = replace_single<std::uint64_t>(space_, hit_path, 0); !status) {
            return status;
        }
        if (auto status = replace_single<std::uint64_t>(space_, dispatch_path, 0); !status) {
            return status;
        }
        return {};
    }

    void publish_running(bool value) {
        (void)replace_single<bool>(space_, state_path_, value);
    }

    void run() {
        publish_running(true);
        auto next_refresh = std::chrono::steady_clock::now();

        while (!stop_flag_.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() >= next_refresh) {
                refresh_bindings();
                next_refresh = std::chrono::steady_clock::now() + options_.refresh_interval;
            }

            bool processed = false;
            for (auto& binding : bindings_) {
                processed |= drain_pointer(binding);
                processed |= drain_button(binding);
                processed |= drain_text(binding);
            }

            if (!processed) {
                std::this_thread::sleep_for(options_.idle_sleep);
            } else {
                publish_metrics();
            }
        }

        publish_running(false);
        publish_metrics();
    }

    void refresh_bindings() {
        auto tokens = list_children(space_, windows_root_);
        std::unordered_map<std::string, WindowBinding> updated;

        for (auto const& token : tokens) {
            auto maybe_binding = build_binding(token);
            if (!maybe_binding) {
                continue;
            }
            updated.emplace(token, std::move(*maybe_binding));
        }

        bindings_.clear();
        for (auto& [token, binding] : updated) {
            bindings_.push_back(std::move(binding));
        }
    }

    auto build_binding(std::string const& token) -> std::optional<WindowBinding> {
        std::string base = windows_root_;
        base.push_back('/');
        base.append(token);

        auto window_path = read_optional<std::string>(space_, base + "/window");
        if (!window_path || !window_path->has_value()) {
            return std::nullopt;
        }

        auto app_root = derive_app_root_for(SP::App::ConcretePathView{**window_path});
        if (!app_root) {
            return std::nullopt;
        }

        WindowBinding binding{};
        binding.token = token;
        binding.window_path = **window_path;
        binding.app_root = app_root->getPath();
        binding.pointer_queue = events_root_ + "/" + token + "/pointer/queue";
        binding.button_queue = events_root_ + "/" + token + "/button/queue";
        binding.text_queue = events_root_ + "/" + token + "/text/queue";
        binding.scene_path = resolve_scene_path(binding);
        return binding;
    }

    auto resolve_scene_path(WindowBinding const& binding) -> std::string {
        std::string views_root = binding.window_path + "/views";
        auto views = list_children(space_, views_root);
        for (auto const& view_name : views) {
            auto scene_rel = read_optional<std::string>(
                space_, views_root + "/" + view_name + "/scene");
            if (!scene_rel || !scene_rel->has_value()) {
                continue;
            }
            auto absolute = make_scene_absolute(binding.app_root, **scene_rel);
            if (!absolute.empty()) {
                return absolute;
            }
        }
        return {};
    }

    static auto make_scene_absolute(std::string const& app_root,
                                    std::string const& stored) -> std::string {
        if (stored.empty()) {
            return {};
        }
        if (!stored.empty() && stored.front() == '/') {
            return stored;
        }
        std::string absolute = app_root;
        if (!absolute.empty() && absolute.back() != '/') {
            absolute.push_back('/');
        }
        absolute.append(stored);
        return absolute;
    }

    auto drain_pointer(WindowBinding const& binding) -> bool {
        bool processed = false;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            auto taken = space_.take<SP::IO::PointerEvent, std::string>(binding.pointer_queue);
            if (!taken) {
                auto const& error = taken.error();
                if (error.code == SP::Error::Code::NoObjectFound) {
                    break;
                }
                enqueue_error(space_, "WidgetEventTrellis pointer read failed: "
                        + error.message.value_or("unknown error"));
                break;
            }
            processed = true;
            ++pointer_events_total_;
            handle_pointer_event(binding, *taken);
        }
        return processed;
    }

    auto drain_button(WindowBinding const& binding) -> bool {
        bool processed = false;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            auto taken = space_.take<SP::IO::ButtonEvent, std::string>(binding.button_queue);
            if (!taken) {
                auto const& error = taken.error();
                if (error.code == SP::Error::Code::NoObjectFound) {
                    break;
                }
                enqueue_error(space_, "WidgetEventTrellis button read failed: "
                        + error.message.value_or("unknown error"));
                break;
            }
            processed = true;
            ++button_events_total_;
            handle_button_event(binding, *taken);
        }
        return processed;
    }

    auto drain_text(WindowBinding const& binding) -> bool {
        bool processed = false;
        while (!stop_flag_.load(std::memory_order_relaxed)) {
            auto taken = space_.take<SP::IO::TextEvent, std::string>(binding.text_queue);
            if (!taken) {
                auto const& error = taken.error();
                if (error.code == SP::Error::Code::NoObjectFound) {
                    break;
                }
                enqueue_error(space_, "WidgetEventTrellis text read failed: "
                        + error.message.value_or("unknown error"));
                break;
            }
            processed = true;
            handle_text_event(binding, *taken);
        }
        return processed;
    }

    void handle_pointer_event(WindowBinding const& binding,
                              SP::IO::PointerEvent const& event) {
        auto& state = pointer_state(binding.token);

        if (event.motion.absolute) {
            state.x = event.motion.absolute_x;
            state.y = event.motion.absolute_y;
            state.have_position = true;
        } else if (event.motion.delta_x != 0.0f || event.motion.delta_y != 0.0f) {
            state.x += event.motion.delta_x;
            state.y += event.motion.delta_y;
            state.have_position = true;
        }

        if (!state.have_position) {
            return;
        }

        auto target = resolve_target(binding, state);
        update_hover(binding, state, target);
        if (state.primary_down && state.slider_active_widget && target && target->kind == TargetKind::Slider
            && target->widget_path == *state.slider_active_widget) {
            handle_slider_update(binding, state, *target);
        }
        if (state.primary_down && state.paint_active_widget && target
            && target->kind == TargetKind::PaintSurface
            && target->widget_path == *state.paint_active_widget) {
            handle_paint_update(binding, state, *target);
        }
    }

    void handle_button_event(WindowBinding const& binding,
                             SP::IO::ButtonEvent const& event) {
        switch (event.source) {
        case SP::IO::ButtonSource::Mouse:
            handle_mouse_button_event(binding, event);
            break;
        case SP::IO::ButtonSource::Keyboard:
        case SP::IO::ButtonSource::Gamepad:
            handle_focus_button_event(binding, event);
            break;
        default:
            break;
        }
    }

    void handle_mouse_button_event(WindowBinding const& binding,
                                   SP::IO::ButtonEvent const& event) {
        auto& state = pointer_state(binding.token);
        bool pressed = event.state.pressed;

        if (pressed) {
            state.primary_down = true;
            state.active_target = state.hover_target;
            if (state.active_target && state.active_target->valid()) {
                switch (state.active_target->kind) {
                case TargetKind::Button:
                case TargetKind::Toggle:
                    emit_widget_op(binding,
                                   *state.active_target,
                                   WidgetBindings::WidgetOpKind::Press,
                                   1.0f,
                                   true);
                    if (state.active_target->kind == TargetKind::Button) {
                        DeclarativeDetail::SetButtonPressed(space_, state.active_target->widget_path, true);
                    }
                    break;
                case TargetKind::Slider:
                    handle_slider_begin(binding, state, *state.active_target);
                    break;
                case TargetKind::List:
                    handle_list_press(state, *state.active_target);
                    break;
                case TargetKind::TreeRow:
                case TargetKind::TreeToggle:
                    handle_tree_press(state, *state.active_target);
                    break;
                case TargetKind::StackPanel:
                    handle_stack_press(state, *state.active_target);
                    break;
                case TargetKind::PaintSurface:
                    handle_paint_begin(binding, state, *state.active_target);
                    break;
                default:
                    break;
                }
            }
            return;
        }

        if (!state.primary_down) {
            return;
        }
        state.primary_down = false;

        if (state.active_target && state.active_target->valid()) {
            switch (state.active_target->kind) {
            case TargetKind::Button:
                emit_widget_op(binding,
                               *state.active_target,
                               WidgetBindings::WidgetOpKind::Release,
                               0.0f,
                               true);
                DeclarativeDetail::SetButtonPressed(space_, state.active_target->widget_path, false);
                if (state.hover_target
                    && state.hover_target->widget_path == state.active_target->widget_path) {
                    emit_widget_op(binding,
                                   *state.active_target,
                                   WidgetBindings::WidgetOpKind::Activate,
                                   1.0f,
                                   true);
                }
                break;
            case TargetKind::Toggle:
                emit_widget_op(binding,
                               *state.active_target,
                               WidgetBindings::WidgetOpKind::Release,
                               0.0f,
                               true);
                if (state.hover_target
                    && state.hover_target->widget_path == state.active_target->widget_path) {
                    emit_widget_op(binding,
                                   *state.active_target,
                                   WidgetBindings::WidgetOpKind::Toggle,
                                   1.0f,
                                   true);
                    DeclarativeDetail::ToggleToggleChecked(space_, state.active_target->widget_path);
                }
                break;
            case TargetKind::Slider: {
                bool inside = state.hover_target
                    && state.hover_target->widget_path == state.active_target->widget_path;
                handle_slider_commit(binding, state, inside);
                break;
            }
            case TargetKind::List:
                handle_list_release(binding, state, *state.active_target);
                break;
            case TargetKind::TreeRow:
            case TargetKind::TreeToggle:
                handle_tree_release(binding, state, *state.active_target);
                break;
            case TargetKind::InputField:
                handle_text_focus(binding, state, *state.active_target);
                break;
            case TargetKind::StackPanel:
                handle_stack_release(binding, state, *state.active_target);
                break;
            case TargetKind::PaintSurface: {
                bool inside = state.hover_target
                    && state.hover_target->widget_path == state.active_target->widget_path;
                handle_paint_commit(binding, state, inside);
                break;
            }
            default:
                break;
            }
        }

        state.active_target.reset();
    }

    void handle_focus_button_event(WindowBinding const& binding,
                                   SP::IO::ButtonEvent const& event) {
        auto& state = pointer_state(binding.token);
        bool pressed = event.state.pressed;
        auto focused = focused_widget_path(space_, binding);

        if (pressed) {
            if (!focused || focused->empty()) {
                return;
            }
            auto target = focus_target_for_widget(*focused);
            if (!target) {
                return;
            }
            if (state.focus_press_target
                && state.focus_press_target->widget_path == target->widget_path
                && state.focus_press_target->kind == target->kind) {
                return;
            }
            switch (target->kind) {
            case TargetKind::Button:
                state.focus_press_target = target;
                DeclarativeDetail::SetButtonPressed(space_, target->widget_path, true);
                emit_widget_op(binding,
                               *target,
                               WidgetBindings::WidgetOpKind::Press,
                               1.0f,
                               true);
                break;
            case TargetKind::Toggle:
                state.focus_press_target = target;
                emit_widget_op(binding,
                               *target,
                               WidgetBindings::WidgetOpKind::Press,
                               1.0f,
                               true);
                break;
            default:
                break;
            }
            return;
        }

        if (!state.focus_press_target) {
            return;
        }

        auto target = *state.focus_press_target;
        state.focus_press_target.reset();
        bool inside = focused && !focused->empty() && *focused == target.widget_path;

        switch (target.kind) {
        case TargetKind::Button:
            emit_widget_op(binding,
                           target,
                           WidgetBindings::WidgetOpKind::Release,
                           0.0f,
                           inside);
            DeclarativeDetail::SetButtonPressed(space_, target.widget_path, false);
            if (inside) {
                emit_widget_op(binding,
                               target,
                               WidgetBindings::WidgetOpKind::Activate,
                               1.0f,
                               true);
            }
            break;
        case TargetKind::Toggle:
            emit_widget_op(binding,
                           target,
                           WidgetBindings::WidgetOpKind::Release,
                           0.0f,
                           inside);
            if (inside) {
                emit_widget_op(binding,
                               target,
                               WidgetBindings::WidgetOpKind::Toggle,
                               1.0f,
                               true);
                DeclarativeDetail::ToggleToggleChecked(space_, target.widget_path);
            }
            break;
        default:
            break;
        }
    }

    auto focus_target_for_widget(std::string const& widget_path) -> std::optional<TargetInfo> {
        auto kind = space_.read<std::string, std::string>(widget_path + "/meta/kind");
        if (!kind) {
            auto const& error = kind.error();
            if (error.code != SP::Error::Code::NoObjectFound
                && error.code != SP::Error::Code::NoSuchPath) {
                enqueue_error(space_, "WidgetEventTrellis failed to read widget kind for "
                        + widget_path + ": " + error.message.value_or("unknown error"));
            }
            return std::nullopt;
        }
        TargetInfo info{};
        info.widget_path = widget_path;
        info.component = *kind + "/focus";
        parse_component(info);
        if (!info.valid()) {
            enqueue_error(space_, "WidgetEventTrellis could not derive focus target for "
                    + widget_path + " (kind=" + *kind + ")");
            return std::nullopt;
        }
        return info;
    }

    void handle_slider_begin(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target) {
        if (!target.has_local) {
            return;
        }
        auto data = read_slider_data(space_, target.widget_path);
        if (!data) {
            return;
        }
        auto value = slider_value_from_local(*data, target.local_x);
        state.slider_active_widget = target.widget_path;
        state.slider_active_value = value;
        data->state.dragging = true;
        data->state.value = value;
        write_slider_state(space_, target.widget_path, data->state);
        emit_widget_op(binding,
                       target,
                       WidgetBindings::WidgetOpKind::SliderBegin,
                       value,
                       true);
    }

    void handle_slider_update(WindowBinding const& binding,
                              PointerState& state,
                              TargetInfo const& target) {
        if (!state.slider_active_widget
            || *state.slider_active_widget != target.widget_path
            || !target.has_local) {
            return;
        }
        auto data = read_slider_data(space_, target.widget_path);
        if (!data) {
            return;
        }
        auto value = slider_value_from_local(*data, target.local_x);
        if (std::abs(value - state.slider_active_value) < 1e-4f) {
            return;
        }
        state.slider_active_value = value;
        data->state.dragging = true;
        data->state.value = value;
        write_slider_state(space_, target.widget_path, data->state);
        emit_widget_op(binding,
                       target,
                       WidgetBindings::WidgetOpKind::SliderUpdate,
                       value,
                       true);
    }

    void handle_slider_commit(WindowBinding const& binding,
                              PointerState& state,
                              bool inside) {
        if (!state.slider_active_widget) {
            return;
        }
        TargetInfo info{};
        info.widget_path = *state.slider_active_widget;
        info.component = "slider/thumb";
        info.kind = TargetKind::Slider;
        if (auto data = read_slider_data(space_, info.widget_path)) {
            data->state.dragging = false;
            data->state.value = state.slider_active_value;
            write_slider_state(space_, info.widget_path, data->state);
        }
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::SliderCommit,
                       state.slider_active_value,
                       inside);
        state.slider_active_widget.reset();
    }

    void handle_list_press(PointerState& state, TargetInfo const& target) {
        if (!target.has_local) {
            return;
        }
        auto data = read_list_data(space_, target.widget_path);
        if (!data) {
            return;
        }
        auto index = list_index_from_local(*data, target.local_y);
        state.list_press_widget = target.widget_path;
        state.list_press_index = index;
    }

    void handle_list_release(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target) {
        if (!state.list_press_widget || *state.list_press_widget != target.widget_path) {
            return;
        }
        if (!state.list_press_index || *state.list_press_index < 0) {
            state.list_press_widget.reset();
            state.list_press_index.reset();
            return;
        }
        if (!state.hover_target || state.hover_target->widget_path != target.widget_path) {
            state.list_press_widget.reset();
            state.list_press_index.reset();
            return;
        }
        auto index = *state.list_press_index;
        TargetInfo info = target;
        info.component = "list/item/" + std::to_string(index);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::ListSelect,
                       static_cast<float>(index),
                       true);
        DeclarativeDetail::SetListSelectionIndex(space_, target.widget_path, index);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::ListActivate,
                       static_cast<float>(index),
                       true);
        state.list_press_widget.reset();
        state.list_press_index.reset();
    }

    auto write_stack_active_panel(PathSpace& space,
                                  std::string const& widget_path,
                                  std::string const& panel_id) -> bool {
        auto status = replace_single<std::string>(space,
                                                  widget_path + "/state/active_panel",
                                                  panel_id);
        if (!status) {
            enqueue_error(space,
                          "WidgetEventTrellis failed to write stack active panel for "
                              + widget_path);
            return false;
        }
        mark_widget_dirty(space, widget_path);
        return true;
    }

    void handle_stack_press(PointerState& state, TargetInfo const& target) {
        if (!target.stack_panel_id) {
            return;
        }
        state.stack_press_widget = target.widget_path;
        state.stack_press_panel = target.stack_panel_id;
    }

    void handle_stack_release(WindowBinding const& binding,
                              PointerState& state,
                              TargetInfo const& target) {
        if (!target.stack_panel_id) {
            state.stack_press_widget.reset();
            state.stack_press_panel.reset();
            return;
        }
        if (!state.stack_press_widget || target.widget_path != *state.stack_press_widget) {
            state.stack_press_widget.reset();
            state.stack_press_panel.reset();
            return;
        }
        if (!state.stack_press_panel || *state.stack_press_panel != *target.stack_panel_id) {
            state.stack_press_widget.reset();
            state.stack_press_panel.reset();
            return;
        }
        if (!state.hover_target || state.hover_target->widget_path != target.widget_path
            || !state.hover_target->stack_panel_id
            || *state.hover_target->stack_panel_id != *target.stack_panel_id) {
            state.stack_press_widget.reset();
            state.stack_press_panel.reset();
            return;
        }

        if (write_stack_active_panel(space_, target.widget_path, *target.stack_panel_id)) {
            TargetInfo info = target;
            info.component = "stack/panel/" + *target.stack_panel_id;
            emit_widget_op(binding,
                           info,
                           WidgetBindings::WidgetOpKind::StackSelect,
                           0.0f,
                           true);
        }

        state.stack_press_widget.reset();
        state.stack_press_panel.reset();
    }

    auto format_paint_component(std::uint64_t stroke_id) -> std::string {
        return std::string{"paint_surface/stroke/"}.append(std::to_string(stroke_id));
    }

    void reset_paint_state(PointerState& state) {
        state.paint_active_widget.reset();
        state.paint_active_stroke_id.reset();
        state.paint_has_last_local = false;
    }

    void handle_paint_begin(WindowBinding const& binding,
                            PointerState& state,
                            TargetInfo const& target) {
        if (!target.has_local) {
            return;
        }
        auto stroke_id = ++state.paint_stroke_sequence;
        state.paint_active_widget = target.widget_path;
        state.paint_active_stroke_id = stroke_id;
        state.paint_last_local_x = target.local_x;
        state.paint_last_local_y = target.local_y;
        state.paint_has_last_local = true;

        TargetInfo info = target;
        info.component = format_paint_component(stroke_id);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::PaintStrokeBegin,
                       0.0f,
                       true);
    }

    void handle_paint_update(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target) {
        if (!state.paint_active_widget || !state.paint_active_stroke_id) {
            return;
        }
        if (target.widget_path != *state.paint_active_widget || !target.has_local) {
            return;
        }
        state.paint_last_local_x = target.local_x;
        state.paint_last_local_y = target.local_y;
        state.paint_has_last_local = true;

        TargetInfo info = target;
        info.component = format_paint_component(*state.paint_active_stroke_id);
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::PaintStrokeUpdate,
                       0.0f,
                       true);
    }

    void handle_paint_commit(WindowBinding const& binding,
                             PointerState& state,
                             bool inside) {
        if (!state.paint_active_widget || !state.paint_active_stroke_id) {
            reset_paint_state(state);
            return;
        }

        TargetInfo info{};
        info.widget_path = *state.paint_active_widget;
        info.component = format_paint_component(*state.paint_active_stroke_id);
        info.kind = TargetKind::PaintSurface;
        if (state.paint_has_last_local) {
            info.has_local = true;
            info.local_x = state.paint_last_local_x;
            info.local_y = state.paint_last_local_y;
        }

        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::PaintStrokeCommit,
                       0.0f,
                       inside);
        reset_paint_state(state);
    }

    void handle_tree_press(PointerState& state, TargetInfo const& target) {
        state.tree_press_widget = target.widget_path;
        state.tree_press_node = target.tree_node_id;
        state.tree_press_toggle = (target.kind == TargetKind::TreeToggle);
    }

    void handle_tree_release(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target) {
        if (!state.tree_press_widget || *state.tree_press_widget != target.widget_path) {
            return;
        }
        if (!state.tree_press_node || target.tree_node_id != state.tree_press_node) {
            state.tree_press_widget.reset();
            state.tree_press_node.reset();
            state.tree_press_toggle = false;
            return;
        }
        auto info = target;
        if (state.tree_press_toggle) {
            emit_widget_op(binding,
                           info,
                           WidgetBindings::WidgetOpKind::TreeToggle,
                           0.0f,
                           true);
            if (info.tree_node_id) {
                DeclarativeDetail::ToggleTreeExpanded(space_, target.widget_path, *info.tree_node_id);
            }
        } else {
            emit_widget_op(binding,
                           info,
                           WidgetBindings::WidgetOpKind::TreeSelect,
                           0.0f,
                           true);
            if (info.tree_node_id) {
            DeclarativeDetail::SetTreeSelectedNode(space_, target.widget_path, *info.tree_node_id);
            }
        }
        state.tree_press_widget.reset();
        state.tree_press_node.reset();
        state.tree_press_toggle = false;
    }

    void handle_text_focus(WindowBinding const& binding,
                           PointerState& state,
                           TargetInfo const& target) {
        state.text_focus_widget = target.widget_path;
        emit_widget_op(binding,
                       target,
                       WidgetBindings::WidgetOpKind::TextFocus,
                       0.0f,
                       true);
    }

    void handle_text_event(WindowBinding const& binding,
                           SP::IO::TextEvent const& event) {
        auto& state = pointer_state(binding.token);
        std::optional<std::string> target_widget = state.text_focus_widget;
        if (!target_widget) {
            target_widget = focused_widget_path(space_, binding);
        }
        if (!target_widget || target_widget->empty()) {
            return;
        }
        TargetInfo info{};
        info.widget_path = *target_widget;
        info.component = "input_field/text";
        info.kind = TargetKind::InputField;
        emit_widget_op(binding,
                       info,
                       WidgetBindings::WidgetOpKind::TextInput,
                       static_cast<float>(event.codepoint),
                       true);
    }

    auto resolve_target(WindowBinding const& binding,
                        PointerState const& state) -> std::optional<TargetInfo> {
        if (binding.scene_path.empty()) {
            return std::nullopt;
        }

        auto result = run_hit_test(binding, state);
        if (!result) {
            ++hit_test_failures_;
            return std::nullopt;
        }
        if (!result->hit) {
            return std::nullopt;
        }

        auto target = BuilderWidgets::ResolveHitTarget(*result);
        if (!target) {
            return std::nullopt;
        }

        TargetInfo info{};
        info.widget_path = target->widget.getPath();
        info.component = target->component;
        info.local_x = result->position.local_x;
        info.local_y = result->position.local_y;
        info.has_local = result->position.has_local;
        parse_component(info);
        if (!info.valid()) {
            return std::nullopt;
        }
        return info;
    }

    auto run_hit_test(WindowBinding const& binding,
                      PointerState const& state) -> SP::Expected<BuildersScene::HitTestResult> {
        auto override_hit = options_.hit_test_override;
        if (override_hit) {
            return override_hit(space_, binding.scene_path, state.x, state.y);
        }

        BuildersScene::HitTestRequest request{};
        request.x = state.x;
        request.y = state.y;
        request.max_results = 1;
        auto scene_path = ScenePath{binding.scene_path};
        return BuildersScene::HitTest(space_, scene_path, request);
    }

    static auto parse_component(TargetInfo& info) -> void {
        if (info.component.empty()) {
            info.kind = TargetKind::Unknown;
            return;
        }
        std::vector<std::string> parts;
        std::string current;
        for (char ch : info.component) {
            if (ch == '/') {
                if (!current.empty()) {
                    parts.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(ch);
            }
        }
        if (!current.empty()) {
            parts.push_back(current);
        }
        if (parts.empty()) {
            info.kind = TargetKind::Unknown;
            return;
        }
        auto const& prefix = parts.front();
        if (prefix == "button") {
            info.kind = TargetKind::Button;
            return;
        }
        if (prefix == "toggle") {
            info.kind = TargetKind::Toggle;
            return;
        }
        if (prefix == "slider") {
            info.kind = TargetKind::Slider;
            return;
        }
        if (prefix == "list") {
            info.kind = TargetKind::List;
            if (parts.size() >= 3 && parts[1] == "item") {
                info.list_item_id = parts[2];
                try {
                    info.list_index = std::stoi(parts[2]);
                } catch (...) {
                    info.list_index.reset();
                }
            }
            return;
        }
        if (prefix == "tree") {
            if (parts.size() >= 3 && parts[1] == "toggle") {
                info.kind = TargetKind::TreeToggle;
                info.tree_node_id = parts[2];
                return;
            }
            if (parts.size() >= 3 && parts[1] == "row") {
                info.kind = TargetKind::TreeRow;
                info.tree_node_id = parts[2];
                return;
            }
            info.kind = TargetKind::TreeRow;
            return;
        }
        if (prefix == "stack") {
            if (parts.size() >= 3 && parts[1] == "child") {
                info.kind = TargetKind::StackPanel;
                info.stack_panel_id = parts[2];
                return;
            }
            info.kind = TargetKind::StackPanel;
            return;
        }
        if (prefix == "input_field") {
            info.kind = TargetKind::InputField;
            return;
        }
        if (prefix == "paint_surface") {
            info.kind = TargetKind::PaintSurface;
            return;
        }
        info.kind = TargetKind::Unknown;
    }

    void update_hover(WindowBinding const& binding,
                      PointerState& state,
                      std::optional<TargetInfo> target) {
        bool changed = false;
        if (target && (!state.hover_target || state.hover_target->widget_path != target->widget_path)) {
            changed = true;
        } else if (!target && state.hover_target) {
            changed = true;
        }

        if (!changed) {
            return;
        }

        auto previous = state.hover_target;
        if (state.hover_target && state.hover_target->valid()) {
            emit_widget_op(binding, *state.hover_target, WidgetBindings::WidgetOpKind::HoverExit, 0.0f, false);
        }
        state.hover_target = target;
        if (state.hover_target && state.hover_target->valid()) {
            emit_widget_op(binding, *state.hover_target, WidgetBindings::WidgetOpKind::HoverEnter, 0.0f, true);
        }
        handle_hover_state(binding, state, previous, state.hover_target);
    }

    void handle_hover_state(WindowBinding const& binding,
                            PointerState& state,
                            std::optional<TargetInfo> const& previous,
                            std::optional<TargetInfo> const& current) {
        (void)binding;
        if (previous) {
            switch (previous->kind) {
            case TargetKind::Button:
                DeclarativeDetail::SetButtonHovered(space_, previous->widget_path, false);
                break;
            case TargetKind::Toggle:
                DeclarativeDetail::SetToggleHovered(space_, previous->widget_path, false);
                break;
            case TargetKind::Slider:
                update_slider_hover(space_, previous->widget_path, false);
                break;
            case TargetKind::List:
                state.list_hover_widget.reset();
                state.list_hover_index.reset();
                DeclarativeDetail::SetListHoverIndex(space_, previous->widget_path, std::nullopt);
                break;
            case TargetKind::TreeRow:
            case TargetKind::TreeToggle:
                state.tree_hover_widget.reset();
                state.tree_hover_node.reset();
                DeclarativeDetail::SetTreeHoveredNode(space_, previous->widget_path, std::nullopt);
                break;
            default:
                break;
            }
        }

        if (!current) {
            return;
        }

        switch (current->kind) {
        case TargetKind::Button:
            DeclarativeDetail::SetButtonHovered(space_, current->widget_path, true);
            break;
        case TargetKind::Toggle:
            DeclarativeDetail::SetToggleHovered(space_, current->widget_path, true);
            break;
        case TargetKind::Slider:
            update_slider_hover(space_, current->widget_path, true);
            break;
        case TargetKind::List: {
            if (!current->has_local) {
                state.list_hover_widget.reset();
                state.list_hover_index.reset();
                DeclarativeDetail::SetListHoverIndex(space_, current->widget_path, std::nullopt);
                break;
            }
            auto data = read_list_data(space_, current->widget_path);
            if (!data) {
                state.list_hover_widget.reset();
                state.list_hover_index.reset();
                break;
            }
            auto index = list_index_from_local(*data, current->local_y);
            if (state.list_hover_widget == current->widget_path && state.list_hover_index == index) {
                break;
            }
            state.list_hover_widget = current->widget_path;
            state.list_hover_index = index;
            DeclarativeDetail::SetListHoverIndex(space_, current->widget_path, index);
            if (index) {
                auto hover_target = *current;
                hover_target.component = "list/item/" + std::to_string(*index);
                emit_widget_op(binding,
                               hover_target,
                               WidgetBindings::WidgetOpKind::ListHover,
                               static_cast<float>(*index),
                               true);
            }
            break;
        }
        case TargetKind::TreeRow:
        case TargetKind::TreeToggle:
            if (state.tree_hover_widget == current->widget_path
                && state.tree_hover_node == current->tree_node_id) {
                break;
            }
            state.tree_hover_widget = current->widget_path;
            state.tree_hover_node = current->tree_node_id;
            DeclarativeDetail::SetTreeHoveredNode(space_, current->widget_path, current->tree_node_id);
            if (current->tree_node_id) {
                emit_widget_op(binding,
                               *current,
                               WidgetBindings::WidgetOpKind::TreeHover,
                               0.0f,
                               true);
            }
            break;
        default:
            break;
        }
    }

    void emit_widget_op(WindowBinding const& binding,
                        TargetInfo const& target,
                        WidgetBindings::WidgetOpKind kind,
                        float value,
                        bool inside) {
        if (target.kind == TargetKind::Unknown) {
            return;
        }

        auto const& ptr_state = pointer_state(binding.token);
        WidgetBindings::PointerInfo pointer = WidgetBindings::PointerInfo::Make(ptr_state.x,
                                                                                ptr_state.y)
            .WithInside(inside)
            .WithPrimary(true);
        if (target.has_local) {
            pointer.WithLocal(target.local_x, target.local_y);
        }

        WidgetBindings::WidgetOp op{};
        op.kind = kind;
        op.widget_path = target.widget_path;
        op.target_id = target.component;
        op.pointer = pointer;
        op.value = value;
        op.sequence = g_widget_op_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
        op.timestamp_ns = to_epoch_ns(std::chrono::system_clock::now());

        auto queue_path = target.widget_path + "/ops/inbox/queue";
        auto inserted = space_.insert(queue_path, op);
        if (!inserted.errors.empty()) {
            enqueue_error(space_, "WidgetEventTrellis failed to write WidgetOp for "
                    + target.widget_path + ": " + inserted.errors.front().message.value_or("unknown error"));
            return;
        }

        ++widget_ops_total_;
        last_dispatch_ns_ = now_ns();
    }

    auto pointer_state(std::string const& token) -> PointerState& {
        std::lock_guard<std::mutex> guard(pointer_mutex_);
        return pointer_states_[token];
    }

    void publish_metrics() {
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/pointer_events_total", pointer_events_total_);
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/button_events_total", button_events_total_);
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/widget_ops_total", widget_ops_total_);
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/hit_test_failures_total", hit_test_failures_);
        (void)replace_single<std::uint64_t>(space_, metrics_root_ + "/last_dispatch_ns", last_dispatch_ns_);
    }

private:
    PathSpace& space_;
    WidgetEventTrellisOptions options_;
    std::string windows_root_;
    std::string events_root_;
    std::string metrics_root_;
    std::string log_root_;
    std::string state_path_;
    std::vector<WindowBinding> bindings_;

    std::mutex pointer_mutex_;
    std::unordered_map<std::string, PointerState> pointer_states_;

    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;

    std::uint64_t pointer_events_total_ = 0;
    std::uint64_t button_events_total_ = 0;
    std::uint64_t widget_ops_total_ = 0;
    std::uint64_t hit_test_failures_ = 0;
    std::uint64_t last_dispatch_ns_ = 0;
};

std::mutex g_worker_mutex;
std::unordered_map<PathSpace*, std::shared_ptr<WidgetEventTrellisWorker>> g_workers;

} // namespace

auto CreateWidgetEventTrellis(PathSpace& space,
                              WidgetEventTrellisOptions const& options) -> SP::Expected<bool> {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    auto it = g_workers.find(&space);
    if (it != g_workers.end() && it->second) {
        return false;
    }

    auto worker = std::make_shared<WidgetEventTrellisWorker>(space, options);
    auto started = worker->start();
    if (!started) {
        return std::unexpected(started.error());
    }
    g_workers[&space] = worker;
    return true;
}

auto ShutdownWidgetEventTrellis(PathSpace& space) -> void {
    std::shared_ptr<WidgetEventTrellisWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        auto it = g_workers.find(&space);
        if (it != g_workers.end()) {
            worker = std::move(it->second);
            g_workers.erase(it);
        }
    }
    if (worker) {
        worker->stop();
    }
}

} // namespace SP::UI::Declarative
