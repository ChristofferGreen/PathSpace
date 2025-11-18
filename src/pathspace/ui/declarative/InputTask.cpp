#include <pathspace/ui/declarative/InputTask.hpp>

#include "../BuildersDetail.hpp"
#include "widgets/Common.hpp"

#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>

#include <pathspace/ui/Builders.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace SP::UI::Declarative {

namespace {

using namespace SP::UI::Builders;
using namespace SP::UI::Builders::Detail;
namespace WidgetReducers = SP::UI::Builders::Widgets::Reducers;
using SP::UI::Declarative::ButtonContext;
using SP::UI::Declarative::HandlerBinding;
using SP::UI::Declarative::HandlerKind;
using SP::UI::Declarative::HandlerVariant;
using SP::UI::Declarative::InputFieldContext;
using SP::UI::Declarative::ListChildContext;
using SP::UI::Declarative::PaintSurfaceContext;
using SP::UI::Declarative::SliderContext;
using SP::UI::Declarative::StackPanelContext;
using SP::UI::Declarative::ToggleContext;
using SP::UI::Declarative::TreeNodeContext;
namespace PaintRuntime = SP::UI::Declarative::PaintRuntime;

constexpr std::string_view kRuntimeBase = "/system/widgets/runtime/input";
constexpr std::string_view kStateRunning = "/system/widgets/runtime/input/state/running";
constexpr std::string_view kMetricsBase = "/system/widgets/runtime/input/metrics";
constexpr std::string_view kMetricsLastPump = "/system/widgets/runtime/input/metrics/last_pump_ns";
constexpr std::string_view kMetricsWidgets = "/system/widgets/runtime/input/metrics/widgets_processed_total";
constexpr std::string_view kMetricsActions = "/system/widgets/runtime/input/metrics/actions_published_total";
constexpr std::string_view kMetricsActive = "/system/widgets/runtime/input/metrics/widgets_with_work_total";
constexpr std::string_view kMetricsHandlersInvoked = "/system/widgets/runtime/input/metrics/handlers_invoked_total";
constexpr std::string_view kMetricsHandlerFailures = "/system/widgets/runtime/input/metrics/handler_failures_total";
constexpr std::string_view kMetricsHandlerMissing = "/system/widgets/runtime/input/metrics/handler_missing_total";
constexpr std::string_view kMetricsLastHandler = "/system/widgets/runtime/input/metrics/last_handler_ns";
constexpr std::string_view kMetricsEventsEnqueued = "/system/widgets/runtime/input/metrics/events_enqueued_total";
constexpr std::string_view kMetricsEventsDropped = "/system/widgets/runtime/input/metrics/events_dropped_total";
constexpr std::string_view kLogErrors = "/system/widgets/runtime/input/log/errors/queue";

template <typename T>
auto ensure_value(PathSpace& space,
                  std::string const& path,
                  T const& value) -> SP::Expected<void> {
    auto existing = read_optional<T>(space, path);
    if (!existing) {
        return std::unexpected(existing.error());
    }
    if (existing->has_value()) {
        return {};
    }
    return replace_single<T>(space, path, value);
}

auto ensure_runtime_roots(PathSpace& space) -> SP::Expected<void> {
    if (auto status = ensure_value<bool>(space, std::string{kStateRunning}, false); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsWidgets}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsActions}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsActive}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsLastPump}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsHandlersInvoked}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsHandlerFailures}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsHandlerMissing}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsLastHandler}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsEventsEnqueued}, 0); !status) {
        return status;
    }
    if (auto status = ensure_value<std::uint64_t>(space, std::string{kMetricsEventsDropped}, 0); !status) {
        return status;
    }
    return {};
}

auto enqueue_error(PathSpace& space, std::string message) -> void {
    auto inserted = space.insert(std::string{kLogErrors}, std::move(message));
    (void)inserted;
}

auto list_children(PathSpace& space, std::string const& path) -> std::vector<std::string> {
    return space.listChildren(SP::ConcretePathStringView{path});
}

struct PumpStats {
    std::size_t widgets_processed = 0;
    std::size_t widgets_with_work = 0;
    std::size_t actions_published = 0;
    std::size_t handlers_invoked = 0;
    std::size_t handler_failures = 0;
    std::size_t handler_missing = 0;
    std::uint64_t last_handler_ns = 0;
    std::size_t events_enqueued = 0;
    std::size_t events_dropped = 0;
};

struct WidgetHandlerCounters {
    std::uint64_t invoked = 0;
    std::uint64_t failures = 0;
    std::uint64_t missing = 0;
    bool dirty = false;
};

using WidgetMetricsMap = std::unordered_map<std::string, WidgetHandlerCounters>;

struct PumpResult {
    PumpStats stats;
    WidgetMetricsMap widget_metrics;
};

auto now_ns() -> std::uint64_t;

enum class HandlerMetricKind {
    Invoked,
    Failure,
    Missing
};

auto record_handler_metric(WidgetMetricsMap& metrics,
                           std::string const& widget_path,
                           HandlerMetricKind kind) -> void {
    if (widget_path.empty()) {
        return;
    }
    auto& counters = metrics[widget_path];
    counters.dirty = true;
    switch (kind) {
    case HandlerMetricKind::Invoked:
        counters.invoked += 1;
        break;
    case HandlerMetricKind::Failure:
        counters.failures += 1;
        break;
    case HandlerMetricKind::Missing:
        counters.missing += 1;
        break;
    }
}

struct HandlerRoute {
    std::string_view event;
    HandlerKind kind = HandlerKind::None;
};

auto handler_binding_path(std::string const& widget_path,
                          std::string_view event) -> std::string {
    std::string path = widget_path;
    path.append("/events/");
    path.append(event);
    path.append("/handler");
    return path;
}

auto component_suffix(std::string_view component) -> std::string_view {
    auto pos = component.find_last_of('/');
    if (pos == std::string_view::npos) {
        return component;
    }
    return component.substr(pos + 1);
}

auto route_for_action(SP::UI::Builders::Widgets::Bindings::WidgetOpKind kind)
    -> std::optional<HandlerRoute> {
    using SP::UI::Builders::Widgets::Bindings::WidgetOpKind;
    switch (kind) {
    case WidgetOpKind::Activate:
        return HandlerRoute{"press", HandlerKind::ButtonPress};
    case WidgetOpKind::Toggle:
        return HandlerRoute{"toggle", HandlerKind::Toggle};
    case WidgetOpKind::SliderCommit:
        return HandlerRoute{"change", HandlerKind::Slider};
    case WidgetOpKind::SliderUpdate:
        return HandlerRoute{"change", HandlerKind::Slider};
    case WidgetOpKind::ListActivate:
    case WidgetOpKind::ListSelect:
        return HandlerRoute{"child_event", HandlerKind::ListChild};
    case WidgetOpKind::TreeSelect:
    case WidgetOpKind::TreeToggle:
    case WidgetOpKind::TreeExpand:
    case WidgetOpKind::TreeCollapse:
    case WidgetOpKind::TreeRequestLoad:
        return HandlerRoute{"node_event", HandlerKind::TreeNode};
    case WidgetOpKind::TextInput:
    case WidgetOpKind::TextDelete:
    case WidgetOpKind::TextMoveCursor:
    case WidgetOpKind::TextSetSelection:
    case WidgetOpKind::TextCompositionStart:
    case WidgetOpKind::TextCompositionUpdate:
    case WidgetOpKind::TextCompositionCommit:
    case WidgetOpKind::TextCompositionCancel:
    case WidgetOpKind::TextClipboardCopy:
    case WidgetOpKind::TextClipboardCut:
    case WidgetOpKind::TextClipboardPaste:
    case WidgetOpKind::TextScroll:
        return HandlerRoute{"change", HandlerKind::InputChange};
    case WidgetOpKind::TextSubmit:
        return HandlerRoute{"submit", HandlerKind::InputSubmit};
    case WidgetOpKind::StackSelect:
        return HandlerRoute{"panel_select", HandlerKind::StackPanel};
    case WidgetOpKind::PaintStrokeBegin:
    case WidgetOpKind::PaintStrokeUpdate:
    case WidgetOpKind::PaintStrokeCommit:
        return HandlerRoute{"draw", HandlerKind::PaintDraw};
    default:
        return std::nullopt;
    }
}

auto format_handler_error(SP::UI::Builders::Widgets::Reducers::WidgetAction const& action,
                          std::string_view event,
                          std::string_view message) -> std::string {
    std::ostringstream oss;
    oss << "InputTask handler error for " << action.widget_path
        << " event '" << event << "': " << message;
    return oss.str();
}

auto format_event_error(SP::UI::Builders::Widgets::Reducers::WidgetAction const& action,
                        std::string_view event,
                        std::string_view message) -> std::string {
    std::ostringstream oss;
    oss << "InputTask event enqueue error for " << action.widget_path
        << " event '" << event << "': " << message;
    return oss.str();
}

auto event_inbox_path(std::string const& widget_path) -> std::string {
    std::string path = widget_path;
    path.append("/events/inbox/queue");
    return path;
}

auto event_specific_path(std::string const& widget_path, std::string_view event) -> std::string {
    std::string path = widget_path;
    path.append("/events/");
    path.append(event);
    path.append("/queue");
    return path;
}

void enqueue_widget_event(PathSpace& space,
                          SP::UI::Builders::Widgets::Reducers::WidgetAction const& action,
                          HandlerRoute const& route,
                          PumpStats& stats) {
    bool dropped = false;
    auto insert_event = [&](std::string const& path) {
        auto inserted = space.insert(path, action);
        if (!inserted.errors.empty()) {
            auto const& error = inserted.errors.front();
            auto message = error.message.value_or("unknown error");
            enqueue_error(space,
                          format_event_error(action,
                                             route.event,
                                             std::string(message).append(" (path: ").append(path).append(")")));
            dropped = true;
        }
    };

    insert_event(event_inbox_path(action.widget_path));
    insert_event(event_specific_path(action.widget_path, route.event));

    if (dropped) {
        stats.events_dropped++;
    } else {
        stats.events_enqueued++;
    }
}

auto invoke_handler(PathSpace& space,
                    HandlerKind kind,
                    HandlerVariant const& handler,
                    SP::UI::Builders::Widgets::Reducers::WidgetAction const& action)
    -> std::optional<std::string> {
    using namespace SP::UI::Declarative;
    try {
        switch (kind) {
        case HandlerKind::ButtonPress: {
            auto fn = std::get_if<ButtonHandler>(&handler);
            if (!fn || !(*fn)) {
                return std::string{"button handler not registered"};
            }
            ButtonContext ctx{space, SP::UI::Builders::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return std::nullopt;
        }
        case HandlerKind::Toggle: {
            auto fn = std::get_if<ToggleHandler>(&handler);
            if (!fn || !(*fn)) {
                return std::string{"toggle handler not registered"};
            }
            ToggleContext ctx{space, SP::UI::Builders::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return std::nullopt;
        }
        case HandlerKind::Slider: {
            auto fn = std::get_if<SliderHandler>(&handler);
            if (!fn || !(*fn)) {
                return std::string{"slider handler not registered"};
            }
            SliderContext ctx{space, SP::UI::Builders::WidgetPath{action.widget_path}};
            ctx.value = action.analog_value;
            (*fn)(ctx);
            return std::nullopt;
        }
        case HandlerKind::ListChild: {
            auto fn = std::get_if<ListChildHandler>(&handler);
            if (!fn || !(*fn)) {
                return std::string{"list handler not registered"};
            }
            ListChildContext ctx{space, SP::UI::Builders::WidgetPath{action.widget_path}};
            ctx.child_id = action.target_id;
            (*fn)(ctx);
            return std::nullopt;
        }
        case HandlerKind::TreeNode: {
            auto fn = std::get_if<TreeNodeHandler>(&handler);
            if (!fn || !(*fn)) {
                return std::string{"tree handler not registered"};
            }
            TreeNodeContext ctx{space, SP::UI::Builders::WidgetPath{action.widget_path}};
            ctx.node_id = action.target_id;
            (*fn)(ctx);
            return std::nullopt;
        }
        case HandlerKind::InputChange: {
            auto fn = std::get_if<InputFieldHandler>(&handler);
            if (!fn || !(*fn)) {
                return std::string{"input change handler not registered"};
            }
            InputFieldContext ctx{space, SP::UI::Builders::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return std::nullopt;
        }
        case HandlerKind::InputSubmit: {
            auto fn = std::get_if<InputFieldHandler>(&handler);
            if (!fn || !(*fn)) {
                return std::string{"input submit handler not registered"};
            }
            InputFieldContext ctx{space, SP::UI::Builders::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return std::nullopt;
        }
        case HandlerKind::StackPanel:
            {
                auto fn = std::get_if<StackPanelHandler>(&handler);
                if (!fn || !(*fn)) {
                    return std::string{"stack handler not registered"};
                }
                StackPanelContext ctx{space, SP::UI::Builders::WidgetPath{action.widget_path}};
                auto suffix = component_suffix(action.target_id);
                ctx.panel_id = std::string{suffix};
                (*fn)(ctx);
                return std::nullopt;
            }
        case HandlerKind::LabelActivate:
            return std::string{"handler kind not supported by InputTask"};
        case HandlerKind::PaintDraw: {
            auto fn = std::get_if<PaintSurfaceHandler>(&handler);
            if (!fn || !(*fn)) {
                return std::string{"paint handler not registered"};
            }
            PaintSurfaceContext ctx{space, SP::UI::Builders::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return std::nullopt;
        }
        case HandlerKind::None:
            return std::string{"handler kind not supported by InputTask"};
        }
    } catch (std::exception const& ex) {
        return std::string{"handler threw exception: "}.append(ex.what());
    } catch (...) {
        return std::string{"handler threw unknown exception"};
    }
    return std::string{"handler kind not supported"};
}

void dispatch_action(PathSpace& space,
                     SP::UI::Builders::Widgets::Reducers::WidgetAction const& action,
                     PumpStats& stats,
                     WidgetMetricsMap& widget_metrics) {
    auto route = route_for_action(action.kind);
    if (!route) {
        return;
    }

    enqueue_widget_event(space, action, *route, stats);

    if (route->kind == HandlerKind::PaintDraw) {
        auto runtime_status = PaintRuntime::HandleAction(space, action);
        if (!runtime_status) {
            auto message = runtime_status.error().message.value_or("paint runtime failure");
            enqueue_error(space,
                          format_handler_error(action,
                                               route->event,
                                               std::string{"runtime error: "}.append(message)));
        }
    }

    auto path = handler_binding_path(action.widget_path, route->event);
    auto binding = space.read<HandlerBinding, std::string>(path);
    if (!binding) {
        auto const& error = binding.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            return;
        }
        stats.handler_failures++;
        record_handler_metric(widget_metrics,
                              action.widget_path,
                              HandlerMetricKind::Failure);
        enqueue_error(space, format_handler_error(action, route->event, "failed to read handler binding"));
        return;
    }

    if (binding->kind != route->kind) {
        stats.handler_failures++;
        record_handler_metric(widget_metrics,
                              action.widget_path,
                              HandlerMetricKind::Failure);
        enqueue_error(space,
                      format_handler_error(action,
                                           route->event,
                                           "handler kind mismatch"));
        return;
    }

    auto handler = SP::UI::Declarative::Detail::resolve_handler(binding->registry_key);
    if (!handler || std::holds_alternative<std::monostate>(*handler)) {
        stats.handler_missing++;
        record_handler_metric(widget_metrics,
                              action.widget_path,
                              HandlerMetricKind::Missing);
        enqueue_error(space,
                      format_handler_error(action,
                                           route->event,
                                           "handler registry entry missing"));
        return;
    }

    auto error = invoke_handler(space, route->kind, *handler, action);
    if (error) {
        stats.handler_failures++;
        record_handler_metric(widget_metrics,
                              action.widget_path,
                              HandlerMetricKind::Failure);
        enqueue_error(space, format_handler_error(action, route->event, *error));
        return;
    }

    stats.handlers_invoked++;
    stats.last_handler_ns = now_ns();
    record_handler_metric(widget_metrics,
                          action.widget_path,
                          HandlerMetricKind::Invoked);
}

auto pump_widget(PathSpace& space,
                 std::string const& widget_root,
                 std::size_t max_actions,
                 PumpStats& stats,
                 WidgetMetricsMap& widget_metrics) -> void {
    auto widget_path = WidgetPath{widget_root};
    auto processed = WidgetReducers::ProcessPendingActions(space, widget_path, max_actions);
    if (!processed) {
        std::ostringstream oss;
        oss << "ProcessPendingActions failed for " << widget_root << ": ";
        if (processed.error().message) {
            oss << *processed.error().message;
        } else {
            oss << "unknown error";
        }
        enqueue_error(space, oss.str());
        return;
    }
    stats.widgets_processed++;
    if (!processed->actions.empty()) {
        stats.widgets_with_work++;
        stats.actions_published += processed->actions.size();
        for (auto const& action : processed->actions) {
            dispatch_action(space, action, stats, widget_metrics);
        }
    }
}

auto pump_once(PathSpace& space, InputTaskOptions const& options) -> PumpResult {
    PumpResult result{};

    auto apps = list_children(space, "/system/applications");
    for (auto const& app : apps) {
        std::string widgets_root = "/system/applications/";
        widgets_root.append(app);
        widgets_root.append("/widgets");

        auto widgets = list_children(space, widgets_root);
        for (auto const& widget : widgets) {
            std::string widget_root = widgets_root;
            widget_root.push_back('/');
            widget_root.append(widget);
            pump_widget(space,
                        widget_root,
                        options.max_actions_per_widget,
                        result.stats,
                        result.widget_metrics);
        }
    }

    return result;
}

auto now_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

class InputRuntimeWorker {
public:
    InputRuntimeWorker(PathSpace& space, InputTaskOptions options)
        : space_(space)
        , options_(options) {
        running_.store(true, std::memory_order_release);
        worker_ = std::thread([this] { this->run(); });
    }

    ~InputRuntimeWorker() {
        stop();
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) {
            return;
        }
        stop_flag_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        (void)replace_single<bool>(space_, std::string{kStateRunning}, false);
    }

private:
    void run() {
        (void)replace_single<bool>(space_, std::string{kStateRunning}, true);
        auto sleep_interval = options_.poll_interval.count() <= 0
            ? std::chrono::milliseconds{1}
            : options_.poll_interval;
        while (!stop_flag_.load(std::memory_order_acquire)) {
            auto result = pump_once(space_, options_);
            publish_metrics(result.stats);
            publish_widget_metrics(result.widget_metrics);
            std::this_thread::sleep_for(sleep_interval);
        }
    }

    void publish_metrics(PumpStats const& stats) {
        total_widgets_ += stats.widgets_processed;
        total_actions_ += stats.actions_published;
        total_active_widgets_ += stats.widgets_with_work;
        total_handlers_ += stats.handlers_invoked;
        total_handler_failures_ += stats.handler_failures;
        total_handler_missing_ += stats.handler_missing;
        total_events_enqueued_ += stats.events_enqueued;
        total_events_dropped_ += stats.events_dropped;
        if (stats.last_handler_ns != 0) {
            last_handler_ns_ = stats.last_handler_ns;
        }

        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsWidgets}, total_widgets_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsActions}, total_actions_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsActive}, total_active_widgets_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsLastPump}, now_ns());
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsHandlersInvoked}, total_handlers_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsHandlerFailures}, total_handler_failures_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsHandlerMissing}, total_handler_missing_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsLastHandler}, last_handler_ns_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsEventsEnqueued}, total_events_enqueued_);
        (void)replace_single<std::uint64_t>(space_, std::string{kMetricsEventsDropped}, total_events_dropped_);
    }

    void write_widget_handler_metric(std::string const& widget_path,
                                     std::string_view name,
                                     std::uint64_t value) {
        std::string path = widget_path;
        path.append("/metrics/handlers/");
        path.append(name);
        (void)replace_single<std::uint64_t>(space_, path, value);
    }

    void publish_widget_metrics(WidgetMetricsMap const& metrics) {
        for (auto const& [widget, counters] : metrics) {
            if (!counters.dirty) {
                continue;
            }
            auto& totals = widget_handler_totals_[widget];
            totals.invoked += counters.invoked;
            totals.failures += counters.failures;
            totals.missing += counters.missing;
            write_widget_handler_metric(widget, "invoked_total", totals.invoked);
            write_widget_handler_metric(widget, "failures_total", totals.failures);
            write_widget_handler_metric(widget, "missing_total", totals.missing);
        }
    }

private:
    PathSpace& space_;
    InputTaskOptions options_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::uint64_t total_widgets_ = 0;
    std::uint64_t total_actions_ = 0;
    std::uint64_t total_active_widgets_ = 0;
    std::uint64_t total_handlers_ = 0;
    std::uint64_t total_handler_failures_ = 0;
    std::uint64_t total_handler_missing_ = 0;
    std::uint64_t last_handler_ns_ = 0;
    std::uint64_t total_events_enqueued_ = 0;
    std::uint64_t total_events_dropped_ = 0;
    std::unordered_map<std::string, WidgetHandlerCounters> widget_handler_totals_;
};

std::mutex g_runtime_mutex;
std::unordered_map<PathSpace*, std::shared_ptr<InputRuntimeWorker>> g_runtime_workers;

} // namespace

auto CreateInputTask(PathSpace& space,
                     InputTaskOptions const& options) -> SP::Expected<bool> {
    if (auto ensured = ensure_runtime_roots(space); !ensured) {
        return std::unexpected(ensured.error());
    }

    std::lock_guard<std::mutex> lock(g_runtime_mutex);
    auto it = g_runtime_workers.find(&space);
    if (it != g_runtime_workers.end()) {
        if (it->second) {
            return false;
        }
    }

    auto worker = std::make_shared<InputRuntimeWorker>(space, options);
    g_runtime_workers[&space] = worker;
    return true;
}

auto ShutdownInputTask(PathSpace& space) -> void {
    std::shared_ptr<InputRuntimeWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_runtime_mutex);
        auto it = g_runtime_workers.find(&space);
        if (it != g_runtime_workers.end()) {
            worker = std::move(it->second);
            g_runtime_workers.erase(it);
        }
    }
    if (worker) {
        worker->stop();
    }
}

} // namespace SP::UI::Declarative
