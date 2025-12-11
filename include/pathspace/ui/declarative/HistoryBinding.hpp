#pragma once

#include <pathspace/core/Error.hpp>
#include <pathspace/history/UndoableSpace.hpp>
#include <pathspace/ui/declarative/HistoryTelemetry.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace SP {
class PathSpace;
}

namespace SP::UI::Declarative {

struct HistoryBindingErrorInfo {
    std::string context;
    std::string message;
    std::string code;
    std::uint64_t timestamp_ns = 0;
};

enum class HistoryBindingAction {
    Undo,
    Redo,
};

struct HistoryBinding {
    std::shared_ptr<SP::History::UndoableSpace> undo;
    std::string root;
    std::string metrics_root;
    std::uint64_t undo_total = 0;
    std::uint64_t redo_total = 0;
    std::uint64_t undo_failures = 0;
    std::uint64_t redo_failures = 0;
    bool buttons_enabled = false;
    std::uint64_t buttons_enabled_last_change_ns = 0;
    std::string state = "pending";
    std::uint64_t state_timestamp_ns = 0;
    std::string last_error_context;
    std::string last_error_message;
    std::string last_error_code;
    std::uint64_t last_error_timestamp_ns = 0;
};

struct HistoryBindingOptions {
    std::string history_root;
    std::string metrics_root;
    std::optional<SP::History::HistoryOptions> history_options;
};

auto HistoryMetricsRoot(std::string const& widget_path) -> std::string;
void InitializeHistoryMetrics(SP::PathSpace& space, std::string const& widget_path);
void WriteHistoryBindingState(SP::PathSpace& space,
                              std::string const& metrics_root,
                              std::string_view state);
void WriteHistoryBindingButtonsEnabled(SP::PathSpace& space,
                                       std::string const& metrics_root,
                                       bool enabled);
HistoryBindingErrorInfo RecordHistoryBindingError(SP::PathSpace& space,
                                                  std::string const& metrics_root,
                                                  std::string_view context,
                                                  SP::Error const* error);
auto CreateHistoryBinding(SP::PathSpace& space,
                          HistoryBindingOptions const& options) -> SP::Expected<std::shared_ptr<HistoryBinding>>;
void SetHistoryBindingState(SP::PathSpace& space, HistoryBinding& binding, std::string_view state);
void SetHistoryBindingButtonsEnabled(SP::PathSpace& space, HistoryBinding& binding, bool enabled);
void PublishHistoryBindingCard(SP::PathSpace& space, HistoryBinding const& binding);
void RecordHistoryBindingActionResult(SP::PathSpace& space,
                                      HistoryBinding& binding,
                                      HistoryBindingAction action,
                                      bool success);

auto LookupHistoryBinding(std::string const& history_root) -> std::shared_ptr<HistoryBinding>;

} // namespace SP::UI::Declarative
