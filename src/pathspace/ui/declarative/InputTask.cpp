#include <pathspace/ui/declarative/InputTask.hpp>
#include <pathspace/ui/declarative/Detail.hpp>
#include "widgets/Common.hpp"

#include <pathspace/ui/declarative/HistoryBinding.hpp>
#include <pathspace/ui/declarative/PaintSurfaceRuntime.hpp>
#include <pathspace/ui/declarative/Reducers.hpp>
#include <pathspace/ui/declarative/Telemetry.hpp>

#include <pathspace/ui/runtime/UIRuntime.hpp>
#include <pathspace/ui/declarative/Widgets.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>
#include <pathspace/runtime/IOPump.hpp>

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

using namespace SP::UI::Runtime;
using namespace SP::UI::Declarative::Detail;
namespace WidgetReducers = SP::UI::Declarative::Reducers;
using SP::UI::Runtime::Widgets::WidgetSpacePath;
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
namespace Telemetry = SP::UI::Declarative::Telemetry;

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
constexpr std::string_view kWindowMetricsBase = "/system/widgets/runtime/input/windows";
constexpr std::string_view kAppMetricsBase = "/system/widgets/runtime/input/apps";

auto now_ns() -> std::uint64_t;

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

auto widget_child_roots(PathSpace& space, std::string const& widget_root) -> std::vector<std::string> {
    return SP::UI::Runtime::Widgets::WidgetChildRoots(space, widget_root);
}

auto derive_app_component(std::string const& window_path) -> std::optional<std::string> {
    constexpr std::string_view kPrefix = "/system/applications/";
    if (!window_path.starts_with(kPrefix)) {
        return std::nullopt;
    }
    auto remainder = window_path.substr(kPrefix.size());
    auto pos = remainder.find('/');
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    return remainder.substr(0, pos);
}

auto make_window_widgets_root(std::string const& window_path, std::string_view view_name) -> std::string {
    std::string root = window_path;
    root.append("/views/");
    root.append(view_name);
    root.append("/widgets");
    return root;
}

auto ensure_metric_counter(PathSpace& space,
                           std::string const& path,
                           std::uint64_t delta) -> void {
    auto current = space.read<std::uint64_t, std::string>(path);
    std::uint64_t value = 0;
    if (current) {
        value = *current;
    }
    value += delta;
    (void)replace_single<std::uint64_t>(space, path, value);
}

auto ensure_metric_flag(PathSpace& space,
                        std::string const& path,
                        std::uint64_t value) -> void {
    (void)replace_single<std::uint64_t>(space, path, value);
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
    std::uint64_t loop_latency_ns = 0;
    std::size_t op_backlog = 0;
};

void publish_manual_metrics(PathSpace& space,
                            std::string const& window_token,
                            std::string const& app_component,
                            PumpStats const& stats) {
    auto window_base = std::string(kWindowMetricsBase) + "/" + window_token + "/metrics";
    auto app_base = std::string(kAppMetricsBase) + "/" + app_component + "/metrics";

    auto ensure_root = [&](std::string const& base) {
        (void)ensure_value<std::uint64_t>(space, base + "/widgets_processed_total", 0);
        (void)ensure_value<std::uint64_t>(space, base + "/actions_published_total", 0);
        (void)ensure_value<std::uint64_t>(space, base + "/manual_pumps_total", 0);
        (void)ensure_value<std::uint64_t>(space, base + "/last_manual_pump_ns", 0);
    };

    ensure_root(window_base);
    ensure_root(app_base);

    ensure_metric_counter(space, window_base + "/widgets_processed_total", stats.widgets_processed);
    ensure_metric_counter(space, window_base + "/actions_published_total", stats.actions_published);
    ensure_metric_counter(space, window_base + "/manual_pumps_total", 1);
    ensure_metric_flag(space, window_base + "/last_manual_pump_ns", now_ns());

    ensure_metric_counter(space, app_base + "/widgets_processed_total", stats.widgets_processed);
    ensure_metric_counter(space, app_base + "/actions_published_total", stats.actions_published);
    ensure_metric_counter(space, app_base + "/manual_pumps_total", 1);
    ensure_metric_flag(space, app_base + "/last_manual_pump_ns", now_ns());
}

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
    auto path = WidgetSpacePath(widget_path, "/events/");
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

auto route_for_action(SP::UI::Runtime::Widgets::Bindings::WidgetOpKind kind)
    -> std::optional<HandlerRoute> {
    using SP::UI::Runtime::Widgets::Bindings::WidgetOpKind;
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

auto format_handler_error(WidgetReducers::WidgetAction const& action,
                          std::string_view event,
                          std::string_view message) -> std::string {
    std::ostringstream oss;
    oss << "InputTask handler error for " << action.widget_path
        << " event '" << event << "': " << message;
    return oss.str();
}

auto format_event_error(WidgetReducers::WidgetAction const& action,
                        std::string_view event,
                        std::string_view message) -> std::string {
    std::ostringstream oss;
    oss << "InputTask event enqueue error for " << action.widget_path
        << " event '" << event << "': " << message;
    return oss.str();
}

auto event_inbox_path(std::string const& widget_path) -> std::string {
    return WidgetSpacePath(widget_path, "/events/inbox/queue");
}

auto event_specific_path(std::string const& widget_path, std::string_view event) -> std::string {
    auto path = WidgetSpacePath(widget_path, "/events/");
    path.append(event);
    path.append("/queue");
    return path;
}

void enqueue_widget_event(PathSpace& space,
                          WidgetReducers::WidgetAction const& action,
                          HandlerRoute const& route,
                          PumpStats& stats) {
    bool dropped = false;
    auto insert_event = [&](std::string const& path) {
        auto inserted = space.insert(path, action);
        if (!inserted.errors.empty()) {
            auto const& error = inserted.errors.front();
            auto message = error.message.value_or("unknown error");
            auto formatted = format_event_error(action,
                                                route.event,
                                                std::string(message).append(" (path: ").append(path).append(")"));
            enqueue_error(space, formatted);
            Telemetry::AppendWidgetLog(space, action.widget_path, formatted);
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
                    WidgetReducers::WidgetAction const& action,
                    std::uint64_t& duration_ns) -> std::optional<std::string> {
    duration_ns = 0;
    auto handler_start = now_ns();
    auto finish = [&](std::optional<std::string> value) -> std::optional<std::string> {
        duration_ns = now_ns() - handler_start;
        return value;
    };
    using namespace SP::UI::Declarative;
    try {
        switch (kind) {
        case HandlerKind::ButtonPress: {
            auto fn = std::get_if<ButtonHandler>(&handler);
            if (!fn || !(*fn)) {
                return finish(std::string{"button handler not registered"});
            }
            ButtonContext ctx{space, SP::UI::Runtime::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return finish(std::nullopt);
        }
        case HandlerKind::Toggle: {
            auto fn = std::get_if<ToggleHandler>(&handler);
            if (!fn || !(*fn)) {
                return finish(std::string{"toggle handler not registered"});
            }
            ToggleContext ctx{space, SP::UI::Runtime::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return finish(std::nullopt);
        }
        case HandlerKind::Slider: {
            auto fn = std::get_if<SliderHandler>(&handler);
            if (!fn || !(*fn)) {
                return finish(std::string{"slider handler not registered"});
            }
            SliderContext ctx{space, SP::UI::Runtime::WidgetPath{action.widget_path}};
            ctx.value = action.analog_value;
            (*fn)(ctx);
            return finish(std::nullopt);
        }
        case HandlerKind::ListChild: {
            auto fn = std::get_if<ListChildHandler>(&handler);
            if (!fn || !(*fn)) {
                return finish(std::string{"list handler not registered"});
            }
            ListChildContext ctx{space, SP::UI::Runtime::WidgetPath{action.widget_path}};
            ctx.child_id = action.target_id;
            (*fn)(ctx);
            return finish(std::nullopt);
        }
        case HandlerKind::TreeNode: {
            auto fn = std::get_if<TreeNodeHandler>(&handler);
            if (!fn || !(*fn)) {
                return finish(std::string{"tree handler not registered"});
            }
            TreeNodeContext ctx{space, SP::UI::Runtime::WidgetPath{action.widget_path}};
            ctx.node_id = action.target_id;
            (*fn)(ctx);
            return finish(std::nullopt);
        }
        case HandlerKind::InputChange: {
            auto fn = std::get_if<InputFieldHandler>(&handler);
            if (!fn || !(*fn)) {
                return finish(std::string{"input change handler not registered"});
            }
            InputFieldContext ctx{space, SP::UI::Runtime::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return finish(std::nullopt);
        }
        case HandlerKind::InputSubmit: {
            auto fn = std::get_if<InputFieldHandler>(&handler);
            if (!fn || !(*fn)) {
                return finish(std::string{"input submit handler not registered"});
            }
            InputFieldContext ctx{space, SP::UI::Runtime::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return finish(std::nullopt);
        }
        case HandlerKind::StackPanel:
            {
                auto fn = std::get_if<StackPanelHandler>(&handler);
                if (!fn || !(*fn)) {
                    return finish(std::string{"stack handler not registered"});
                }
                StackPanelContext ctx{space, SP::UI::Runtime::WidgetPath{action.widget_path}};
                auto suffix = component_suffix(action.target_id);
                ctx.panel_id = std::string{suffix};
                (*fn)(ctx);
                return finish(std::nullopt);
            }
        case HandlerKind::LabelActivate:
            return finish(std::string{"handler kind not supported by InputTask"});
        case HandlerKind::PaintDraw: {
            auto fn = std::get_if<PaintSurfaceHandler>(&handler);
            if (!fn || !(*fn)) {
                return finish(std::string{"paint handler not registered"});
            }
            PaintSurfaceContext ctx{space, SP::UI::Runtime::WidgetPath{action.widget_path}};
            (*fn)(ctx);
            return finish(std::nullopt);
        }
        case HandlerKind::None:
            return finish(std::string{"handler kind not supported by InputTask"});
        }
    } catch (std::exception const& ex) {
        return finish(std::string{"handler threw exception: "}.append(ex.what()));
    } catch (...) {
        return finish(std::string{"handler threw unknown exception"});
    }
    return finish(std::string{"handler kind not supported"});
}

void dispatch_action(PathSpace& space,
                     WidgetReducers::WidgetAction const& action,
                     PumpStats& stats,
                     WidgetMetricsMap& widget_metrics,
                     std::chrono::nanoseconds slow_threshold) {
    auto route = route_for_action(action.kind);
    if (!route) {
        return;
    }

    enqueue_widget_event(space, action, *route, stats);

    if (route->kind == HandlerKind::PaintDraw) {
        auto binding = LookupHistoryBinding(action.widget_path);
        auto runtime_status = (binding && binding->undo)
                                  ? PaintRuntime::HandleAction(*binding->undo, action)
                                  : PaintRuntime::HandleAction(space, action);
        if (!runtime_status) {
            auto message = runtime_status.error().message.value_or("paint runtime failure");
            auto formatted = format_handler_error(action,
                                                  route->event,
                                                  std::string{"runtime error: "}.append(message));
            enqueue_error(space, formatted);
            Telemetry::AppendWidgetLog(space, action.widget_path, formatted);
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
        auto message = format_handler_error(action, route->event, "failed to read handler binding");
        enqueue_error(space, message);
        Telemetry::AppendWidgetLog(space, action.widget_path, message);
        return;
    }

    if (binding->kind != route->kind) {
        stats.handler_failures++;
        record_handler_metric(widget_metrics,
                              action.widget_path,
                              HandlerMetricKind::Failure);
        auto message = format_handler_error(action,
                                            route->event,
                                            "handler kind mismatch");
        enqueue_error(space, message);
        Telemetry::AppendWidgetLog(space, action.widget_path, message);
        return;
    }

    auto handler = SP::UI::Declarative::Detail::resolve_handler(binding->registry_key);
    if (!handler || std::holds_alternative<std::monostate>(*handler)) {
        stats.handler_missing++;
        record_handler_metric(widget_metrics,
                              action.widget_path,
                              HandlerMetricKind::Missing);
        auto message = format_handler_error(action,
                                            route->event,
                                            "handler registry entry missing");
        enqueue_error(space, message);
        Telemetry::AppendWidgetLog(space, action.widget_path, message);
        return;
    }

    std::uint64_t handler_duration_ns = 0;
    auto error = invoke_handler(space, route->kind, *handler, action, handler_duration_ns);
    if (error) {
        stats.handler_failures++;
        record_handler_metric(widget_metrics,
                              action.widget_path,
                              HandlerMetricKind::Failure);
        auto message = format_handler_error(action, route->event, *error);
        enqueue_error(space, message);
        Telemetry::AppendWidgetLog(space, action.widget_path, message);
        return;
    }

    stats.handlers_invoked++;
    stats.last_handler_ns = now_ns();
    record_handler_metric(widget_metrics,
                          action.widget_path,
                          HandlerMetricKind::Invoked);

    auto slow_threshold_ns = static_cast<std::uint64_t>(slow_threshold.count());
    if (slow_threshold_ns > 0 && handler_duration_ns > slow_threshold_ns) {
        std::ostringstream oss;
        oss << "slow handler event=" << route->event << " duration_ns=" << handler_duration_ns;
        Telemetry::AppendWidgetLog(space, action.widget_path, oss.str());
    }
}

auto pump_widget(PathSpace& space,
                 std::string const& widget_root,
                 std::size_t max_actions,
                 PumpStats& stats,
                 WidgetMetricsMap& widget_metrics,
                 std::chrono::nanoseconds slow_threshold) -> void {
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
        auto message = oss.str();
        enqueue_error(space, message);
        Telemetry::AppendWidgetLog(space, widget_root, message);
        return;
    }
    stats.widgets_processed++;
    if (!processed->actions.empty()) {
        stats.widgets_with_work++;
        stats.actions_published += processed->actions.size();
        for (auto const& action : processed->actions) {
            dispatch_action(space, action, stats, widget_metrics, slow_threshold);
        }
    }
}

void pump_widget_tree(PathSpace& space,
                      std::string const& widget_root,
                      std::size_t max_actions,
                      PumpStats& stats,
                      WidgetMetricsMap& widget_metrics,
                      std::chrono::nanoseconds slow_threshold) {
    pump_widget(space, widget_root, max_actions, stats, widget_metrics, slow_threshold);

    auto child_roots = widget_child_roots(space, widget_root);
    for (auto const& child_root : child_roots) {
        pump_widget_tree(space, child_root, max_actions, stats, widget_metrics, slow_threshold);
    }
}

void pump_widgets_in_root(PathSpace& space,
                          std::string const& widgets_root,
                          std::size_t max_actions,
                          PumpStats& stats,
                          WidgetMetricsMap& widget_metrics,
                          std::chrono::nanoseconds slow_threshold) {
    auto widgets = list_children(space, widgets_root);
    for (auto const& widget : widgets) {
        std::string widget_root = widgets_root;
        widget_root.push_back('/');
        widget_root.append(widget);
        pump_widget_tree(space, widget_root, max_actions, stats, widget_metrics, slow_threshold);
    }
}

auto pump_once(PathSpace& space, InputTaskOptions const& options) -> PumpResult {
    PumpResult result{};
    auto loop_start = now_ns();
    auto slow_threshold = std::chrono::duration_cast<std::chrono::nanoseconds>(options.slow_handler_threshold);

    auto apps = list_children(space, "/system/applications");
    for (auto const& app : apps) {
        std::string app_root = "/system/applications/";
        app_root.append(app);

        pump_widgets_in_root(space,
                             app_root + "/widgets",
                             options.max_actions_per_widget,
                             result.stats,
                             result.widget_metrics,
                             slow_threshold);

        auto windows_root = app_root + "/windows";
        auto windows = list_children(space, windows_root);
        for (auto const& window_name : windows) {
            auto views_root = windows_root + "/" + window_name + "/views";
            auto views = list_children(space, views_root);
            for (auto const& view_name : views) {
                pump_widgets_in_root(space,
                                     views_root + "/" + view_name + "/widgets",
                                     options.max_actions_per_widget,
                                     result.stats,
                                     result.widget_metrics,
                                     slow_threshold);
            }
        }
    }

    result.stats.loop_latency_ns = now_ns() - loop_start;
    result.stats.op_backlog = result.stats.actions_published;
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
        Telemetry::RecordInputLatency(space_,
                                      Telemetry::InputLatencySample{
                                          .latency_ns = stats.loop_latency_ns,
                                          .backlog = stats.op_backlog,
                                      });
    }

    void write_widget_handler_metric(std::string const& widget_path,
                                     std::string_view name,
                                     std::uint64_t value) {
        auto path = WidgetSpacePath(widget_path, "/metrics/handlers/");
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

auto PumpWindowWidgetsOnce(PathSpace& space,
                           SP::UI::WindowPath const& window,
                           std::string_view view_name,
                           ManualPumpOptions const& options) -> SP::Expected<ManualPumpResult> {
    if (view_name.empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidPath, "view name must not be empty"});
    }

    PumpResult result{};
    auto slow_threshold = std::chrono::duration_cast<std::chrono::nanoseconds>(options.slow_handler_threshold);
    auto window_path = std::string(window.getPath());
    auto window_widgets_root = make_window_widgets_root(window_path, view_name);

    pump_widgets_in_root(space,
                         window_widgets_root,
                         options.max_actions_per_widget,
                         result.stats,
                         result.widget_metrics,
                         slow_threshold);

    std::optional<std::string> app_component = derive_app_component(window_path);
    if (options.include_app_widgets && app_component) {
        std::string app_widgets_root = "/system/applications/";
        app_widgets_root.append(*app_component);
        app_widgets_root.append("/widgets");
        pump_widgets_in_root(space,
                             app_widgets_root,
                             options.max_actions_per_widget,
                             result.stats,
                             result.widget_metrics,
                             slow_threshold);
    }

    if (options.publish_window_metrics && app_component) {
        auto window_token = SP::Runtime::MakeRuntimeWindowToken(window.getPath());
        publish_manual_metrics(space, window_token, *app_component, result.stats);
    }

    ManualPumpResult output{
        .widgets_processed = result.stats.widgets_processed,
        .actions_published = result.stats.actions_published,
    };
    return output;
}

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
