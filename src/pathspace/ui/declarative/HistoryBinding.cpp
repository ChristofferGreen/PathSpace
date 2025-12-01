#include <pathspace/ui/declarative/HistoryBinding.hpp>

#include <pathspace/PathSpace.hpp>
#include <pathspace/history/UndoableSpace.hpp>
#include <pathspace/layer/PathAlias.hpp>
#include <pathspace/path/ConcretePath.hpp>

#include <chrono>
#include <iostream>
#include <utility>

namespace {

auto now_timestamp_ns() -> std::uint64_t {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

template <typename T>
auto replace_value(SP::PathSpace& space, std::string const& path, T const& value) -> SP::Expected<void> {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }

    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

void log_metric_error(std::string_view label, SP::Error const& error) {
    std::cerr << "HistoryBinding: " << label << " failed (" << SP::describeError(error) << ")\n";
}

template <typename T>
void write_history_metric(SP::PathSpace& space,
                          std::string const& metrics_root,
                          std::string_view leaf,
                          T const& value) {
    if (metrics_root.empty()) {
        return;
    }
    std::string path = metrics_root;
    path.push_back('/');
    path.append(leaf.begin(), leaf.end());
    auto status = replace_value(space, path, value);
    if (!status) {
        log_metric_error(std::string{"metric:"}.append(leaf.begin(), leaf.end()), status.error());
    }
}

} // namespace

namespace SP::UI::Declarative {

auto HistoryMetricsRoot(std::string const& widget_path) -> std::string {
    if (widget_path.empty()) {
        return {};
    }
    return widget_path + "/metrics/history_binding";
}

void InitializeHistoryMetrics(SP::PathSpace& space, std::string const& widget_path) {
    auto metrics_root = HistoryMetricsRoot(widget_path);
    if (metrics_root.empty()) {
        return;
    }
    auto timestamp = now_timestamp_ns();
    WriteHistoryBindingState(space, metrics_root, "pending");
    write_history_metric(space, metrics_root, "buttons_enabled", false);
    write_history_metric(space, metrics_root, "buttons_enabled_last_change_ns", timestamp);
    write_history_metric(space, metrics_root, "undo_total", static_cast<std::uint64_t>(0));
    write_history_metric(space, metrics_root, "undo_failures_total", static_cast<std::uint64_t>(0));
    write_history_metric(space, metrics_root, "redo_total", static_cast<std::uint64_t>(0));
    write_history_metric(space, metrics_root, "redo_failures_total", static_cast<std::uint64_t>(0));
    write_history_metric(space, metrics_root, "last_error_context", std::string{});
    write_history_metric(space, metrics_root, "last_error_message", std::string{});
    write_history_metric(space, metrics_root, "last_error_code", std::string{});
    write_history_metric(space, metrics_root, "last_error_timestamp_ns", static_cast<std::uint64_t>(0));
    HistoryBindingTelemetryCard card{};
    card.state = "pending";
    card.state_timestamp_ns = timestamp;
    card.buttons_enabled = false;
    card.buttons_enabled_last_change_ns = timestamp;
    write_history_metric(space, metrics_root, "card", card);
}

void WriteHistoryBindingState(SP::PathSpace& space,
                              std::string const& metrics_root,
                              std::string_view state) {
    write_history_metric(space, metrics_root, "state", std::string{state});
    write_history_metric(space, metrics_root, "state_timestamp_ns", now_timestamp_ns());
}

void WriteHistoryBindingButtonsEnabled(SP::PathSpace& space,
                                       std::string const& metrics_root,
                                       bool enabled) {
    auto timestamp = now_timestamp_ns();
    write_history_metric(space, metrics_root, "buttons_enabled", enabled);
    write_history_metric(space, metrics_root, "buttons_enabled_last_change_ns", timestamp);
}

HistoryBindingErrorInfo RecordHistoryBindingError(SP::PathSpace& space,
                                                  std::string const& metrics_root,
                                                  std::string_view context,
                                                  SP::Error const* error) {
    HistoryBindingErrorInfo info{};
    info.context.assign(context.begin(), context.end());
    if (error != nullptr) {
        info.message = SP::describeError(*error);
        info.code = std::string(SP::errorCodeToString(error->code));
    }
    info.timestamp_ns = now_timestamp_ns();
    if (metrics_root.empty()) {
        return info;
    }
    write_history_metric(space, metrics_root, "last_error_context", info.context);
    write_history_metric(space, metrics_root, "last_error_message", info.message);
    write_history_metric(space, metrics_root, "last_error_code", info.code);
    write_history_metric(space, metrics_root, "last_error_timestamp_ns", info.timestamp_ns);
    return info;
}

auto CreateHistoryBinding(SP::PathSpace& space,
                          HistoryBindingOptions const& options) -> SP::Expected<std::shared_ptr<HistoryBinding>> {
    if (options.history_root.empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidPath, "history_root missing"});
    }
    auto metrics_root = options.metrics_root;
    if (metrics_root.empty()) {
        metrics_root = HistoryMetricsRoot(options.history_root);
    }
    if (metrics_root.empty()) {
        return std::unexpected(SP::Error{SP::Error::Code::InvalidPath, "metrics_root missing"});
    }

    WriteHistoryBindingState(space, metrics_root, "binding");
    auto upstream = std::shared_ptr<SP::PathSpaceBase>(&space, [](SP::PathSpaceBase*) {});
    auto alias = std::make_unique<SP::PathAlias>(upstream, "/");

    SP::History::HistoryOptions defaults{};
    defaults.allowNestedUndo = true;
    defaults.maxEntries = 1024;
    defaults.ramCacheEntries = 64;
    defaults.useMutationJournal = true;
    auto chosen_options = options.history_options ? *options.history_options : defaults;

    auto undo_space = std::make_shared<SP::History::UndoableSpace>(std::move(alias), chosen_options);
    auto enable = undo_space->enableHistory(SP::ConcretePathStringView{options.history_root});
    if (!enable) {
        WriteHistoryBindingState(space, metrics_root, "error");
        RecordHistoryBindingError(space, metrics_root, "UndoableSpace::enableHistory", &enable.error());
        return std::unexpected(enable.error());
    }

    auto binding = std::make_shared<HistoryBinding>();
    binding->undo = std::move(undo_space);
    binding->root = options.history_root;
    binding->metrics_root = std::move(metrics_root);
    binding->buttons_enabled = false;
    binding->buttons_enabled_last_change_ns = now_timestamp_ns();
    SetHistoryBindingState(space, *binding, "ready");
    return binding;
}

void PublishHistoryBindingCard(SP::PathSpace& space, HistoryBinding const& binding) {
    if (binding.metrics_root.empty()) {
        return;
    }
    HistoryBindingTelemetryCard card{};
    card.state = binding.state;
    card.state_timestamp_ns = binding.state_timestamp_ns;
    card.buttons_enabled = binding.buttons_enabled;
    card.buttons_enabled_last_change_ns = binding.buttons_enabled_last_change_ns;
    card.undo_total = binding.undo_total;
    card.undo_failures_total = binding.undo_failures;
    card.redo_total = binding.redo_total;
    card.redo_failures_total = binding.redo_failures;
    card.last_error_context = binding.last_error_context;
    card.last_error_message = binding.last_error_message;
    card.last_error_code = binding.last_error_code;
    card.last_error_timestamp_ns = binding.last_error_timestamp_ns;
    write_history_metric(space, binding.metrics_root, "card", card);
}

void SetHistoryBindingState(SP::PathSpace& space, HistoryBinding& binding, std::string_view state) {
    if (binding.metrics_root.empty()) {
        return;
    }
    binding.state.assign(state.begin(), state.end());
    binding.state_timestamp_ns = now_timestamp_ns();
    WriteHistoryBindingState(space, binding.metrics_root, state);
    PublishHistoryBindingCard(space, binding);
}

void SetHistoryBindingButtonsEnabled(SP::PathSpace& space, HistoryBinding& binding, bool enabled) {
    if (binding.metrics_root.empty()) {
        return;
    }
    binding.buttons_enabled = enabled;
    binding.buttons_enabled_last_change_ns = now_timestamp_ns();
    WriteHistoryBindingButtonsEnabled(space, binding.metrics_root, enabled);
    PublishHistoryBindingCard(space, binding);
}

void RecordHistoryBindingActionResult(SP::PathSpace& space,
                                      HistoryBinding& binding,
                                      HistoryBindingAction action,
                                      bool success) {
    if (binding.metrics_root.empty()) {
        return;
    }
    auto update_metric = [&](std::string_view leaf, std::uint64_t value) {
        write_history_metric(space, binding.metrics_root, leaf, value);
    };

    switch (action) {
    case HistoryBindingAction::Undo:
        if (success) {
            ++binding.undo_total;
            update_metric("undo_total", binding.undo_total);
        } else {
            ++binding.undo_failures;
            update_metric("undo_failures_total", binding.undo_failures);
        }
        break;
    case HistoryBindingAction::Redo:
        if (success) {
            ++binding.redo_total;
            update_metric("redo_total", binding.redo_total);
        } else {
            ++binding.redo_failures;
            update_metric("redo_failures_total", binding.redo_failures);
        }
        break;
    }

    PublishHistoryBindingCard(space, binding);
}

} // namespace SP::UI::Declarative
