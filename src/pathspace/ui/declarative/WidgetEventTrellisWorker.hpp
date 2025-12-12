#pragma once

#include "WidgetEventCommon.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace SP::UI::Declarative {

class WidgetEventTrellisWorker : public std::enable_shared_from_this<WidgetEventTrellisWorker> {
public:
    WidgetEventTrellisWorker(PathSpace& space, WidgetEventTrellisOptions options);
    ~WidgetEventTrellisWorker();

    auto start() -> SP::Expected<void>;
    void stop();

    PathSpace& space() { return space_; }
    WidgetEventTrellisOptions const& options() const { return options_; }

private:
    auto ensure_roots() -> SP::Expected<void>;
    void publish_running(bool value);
    void run();
    void refresh_bindings();
    auto build_binding(std::string const& token) -> std::optional<WindowBinding>;
    auto resolve_scene_path(WindowBinding const& binding) -> std::string;
    static auto make_scene_absolute(std::string const& app_root,
                                    std::string const& stored) -> std::string;

    auto drain_pointer(WindowBinding const& binding) -> bool;
    auto drain_button(WindowBinding const& binding) -> bool;
    auto drain_text(WindowBinding const& binding) -> bool;

    void handle_pointer_event(WindowBinding const& binding,
                              SP::IO::PointerEvent const& event);
    void handle_button_event(WindowBinding const& binding,
                             SP::IO::ButtonEvent const& event);
    void handle_mouse_button_event(WindowBinding const& binding,
                                   SP::IO::ButtonEvent const& event);
    bool handle_focus_nav_event(WindowBinding const& binding,
                                SP::IO::ButtonEvent const& event);
    bool handle_slider_focus_nav(WindowBinding const& binding,
                                 TargetInfo const& target,
                                 FocusNavEvent const& nav);
    bool handle_list_focus_nav(WindowBinding const& binding,
                               TargetInfo const& target,
                               FocusNavEvent const& nav);
    bool handle_list_submit(WindowBinding const& binding,
                            TargetInfo const& target);
    bool handle_tree_focus_nav(WindowBinding const& binding,
                               TargetInfo const& target,
                               FocusNavEvent const& nav);
    bool handle_text_focus_nav(WindowBinding const& binding,
                               TargetInfo const& target,
                               FocusNavEvent const& nav);
    bool handle_text_cursor_step(WindowBinding const& binding,
                                 TargetInfo const& target,
                                 int delta);
    bool handle_text_delete(WindowBinding const& binding,
                            TargetInfo const& target,
                            bool forward);
    bool handle_text_submit(WindowBinding const& binding,
                            TargetInfo const& target);
    void handle_focus_button_event(WindowBinding const& binding,
                                   SP::IO::ButtonEvent const& event);
    auto focus_target_for_widget(std::string const& widget_path) -> std::optional<TargetInfo>;

    void handle_slider_begin(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target);
    void handle_slider_update(WindowBinding const& binding,
                              PointerState& state,
                              TargetInfo const& target);
    void handle_slider_commit(WindowBinding const& binding,
                              PointerState& state,
                              bool inside);
    void handle_list_press(PointerState& state, TargetInfo const& target);
    void handle_list_release(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target);
    void handle_stack_press(PointerState& state, TargetInfo const& target);
    void handle_stack_release(WindowBinding const& binding,
                              PointerState& state,
                              TargetInfo const& target);
    void handle_paint_begin(WindowBinding const& binding,
                            PointerState& state,
                            TargetInfo const& target);
    void handle_paint_update(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target);
    void handle_paint_commit(WindowBinding const& binding,
                             PointerState& state,
                             bool inside);
    void handle_tree_press(PointerState& state, TargetInfo const& target);
    void handle_tree_release(WindowBinding const& binding,
                             PointerState& state,
                             TargetInfo const& target);
    void handle_text_focus(WindowBinding const& binding,
                           PointerState& state,
                           TargetInfo const& target);
    void handle_text_event(WindowBinding const& binding,
                           SP::IO::TextEvent const& event);

    bool select_tree_row(WindowBinding const& binding,
                         std::string const& widget_path,
                         std::string const& node_id);
    bool emit_tree_toggle(WindowBinding const& binding,
                          std::string const& widget_path,
                          std::string const& node_id);

    auto resolve_target(WindowBinding const& binding,
                        PointerState const& state) -> std::optional<TargetInfo>;
    void handle_hover_state(WindowBinding const& binding,
                            PointerState& state,
                            std::optional<TargetInfo> const& previous,
                            std::optional<TargetInfo> const& current);
    auto run_hit_test(WindowBinding const& binding,
                      PointerState const& state) -> SP::Expected<BuildersScene::HitTestResult>;
    static auto parse_component(TargetInfo& info) -> void;
    void update_hover(WindowBinding const& binding,
                      PointerState& state,
                      std::optional<TargetInfo> target);

    void emit_widget_op(WindowBinding const& binding,
                        TargetInfo const& target,
                        WidgetBindings::WidgetOpKind kind,
                        float value,
                        bool inside,
                        std::optional<WidgetBindings::PointerInfo> pointer_override = std::nullopt);

    auto mailbox_subscriptions(std::string const& widget_path)
        -> std::unordered_set<std::string> const&;
    bool route_mailbox_event(TargetInfo const& target,
                             WidgetBindings::WidgetOpKind kind,
                             float value,
                             WidgetBindings::PointerInfo const& pointer,
                             std::uint64_t sequence,
                             std::uint64_t timestamp_ns);

    PointerState& pointer_state(std::string const& token);
    void publish_metrics();

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
    std::unordered_map<std::string, std::unordered_set<std::string>> mailbox_subscriptions_;

    bool capsules_enabled_ = false;

    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread worker_;

    std::uint64_t pointer_events_total_ = 0;
    std::uint64_t button_events_total_ = 0;
    std::uint64_t widget_ops_total_ = 0;
    std::uint64_t hit_test_failures_ = 0;
    std::uint64_t last_dispatch_ns_ = 0;
};

} // namespace SP::UI::Declarative
