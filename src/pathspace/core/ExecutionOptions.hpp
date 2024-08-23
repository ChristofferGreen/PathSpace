#pragma once
#include <chrono>

namespace SP {

struct ExecutionOptions {
    enum class ExecutionTime {
        Immediate,
        OnRead,
        Periodic
    };

    ExecutionTime executionTime = ExecutionTime::OnRead;
    std::optional<std::chrono::milliseconds> updateInterval;
    std::optional<int> maxExecutions;
    // bool cacheResult = false; // how to do this?
};

} // namespace SP