#pragma once

#include <cstdint>
#include <string>

namespace SP::UI::Declarative {

struct HistoryBindingTelemetryCard {
    std::string state;
    std::uint64_t state_timestamp_ns = 0;
    bool buttons_enabled = false;
    std::uint64_t buttons_enabled_last_change_ns = 0;
    std::uint64_t undo_total = 0;
    std::uint64_t undo_failures_total = 0;
    std::uint64_t redo_total = 0;
    std::uint64_t redo_failures_total = 0;
    std::string last_error_context;
    std::string last_error_message;
    std::string last_error_code;
    std::uint64_t last_error_timestamp_ns = 0;
};

} // namespace SP::UI::Declarative
