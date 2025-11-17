#include <pathspace/ui/declarative/WidgetEventTrellis.hpp>

#include "../BuildersDetail.hpp"

#include <pathspace/app/AppPaths.hpp>
#include <pathspace/io/IoEvents.hpp>
#include <pathspace/log/TaggedLogger.hpp>
#include <pathspace/runtime/IOPump.hpp>

#include <atomic>
#include <chrono>
#include <cctype>
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
namespace BuildersScene = SP::UI::Builders::Scene;

constexpr std::string_view kLogErrorsQueue = "/system/widgets/runtime/events/log/errors/queue";

enum class TargetKind {
    Unknown = 0,
    Button,
    Toggle
};

struct TargetInfo {
    std::string widget_path;
    std::string component;
    TargetKind kind = TargetKind::Unknown;

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
        update_hover(binding, state, std::move(target));
    }

    void handle_button_event(WindowBinding const& binding,
                             SP::IO::ButtonEvent const& event) {
        if (event.source != SP::IO::ButtonSource::Mouse) {
            return;
        }

        auto& state = pointer_state(binding.token);
        bool pressed = event.state.pressed;

        if (pressed) {
            state.primary_down = true;
            state.active_target = state.hover_target;
            if (state.active_target && state.active_target->valid()) {
                emit_widget_op(binding, *state.active_target, WidgetBindings::WidgetOpKind::Press, 1.0f, true);
            }
            return;
        }

        if (!state.primary_down) {
            return;
        }
        state.primary_down = false;

        if (state.active_target && state.active_target->valid()) {
            emit_widget_op(binding, *state.active_target, WidgetBindings::WidgetOpKind::Release, 0.0f, true);
            if (state.hover_target
                && state.hover_target->widget_path == state.active_target->widget_path
                && state.active_target->kind == TargetKind::Button) {
                emit_widget_op(binding, *state.active_target, WidgetBindings::WidgetOpKind::Activate, 1.0f, true);
            } else if (state.hover_target
                       && state.hover_target->widget_path == state.active_target->widget_path
                       && state.active_target->kind == TargetKind::Toggle) {
                emit_widget_op(binding, *state.active_target, WidgetBindings::WidgetOpKind::Toggle, 1.0f, true);
            }
        }

        state.active_target.reset();
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

        auto target = Widgets::ResolveHitTarget(*result);
        if (!target) {
            return std::nullopt;
        }

        TargetInfo info{};
        info.widget_path = target->widget.getPath();
        info.component = target->component;
        info.kind = classify_component(target->component);
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

    static auto classify_component(std::string const& component) -> TargetKind {
        if (component.empty()) {
            return TargetKind::Unknown;
        }
        auto slash = component.find('/');
        auto prefix = component.substr(0, slash);
        if (prefix == "button") {
            return TargetKind::Button;
        }
        if (prefix == "toggle") {
            return TargetKind::Toggle;
        }
        return TargetKind::Unknown;
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

        if (state.hover_target && state.hover_target->valid()) {
            emit_widget_op(binding, *state.hover_target, WidgetBindings::WidgetOpKind::HoverExit, 0.0f, false);
        }
        state.hover_target = target;
        if (state.hover_target && state.hover_target->valid()) {
            emit_widget_op(binding, *state.hover_target, WidgetBindings::WidgetOpKind::HoverEnter, 0.0f, true);
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
