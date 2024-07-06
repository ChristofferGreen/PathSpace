#pragma once
#include <chrono>

namespace SP {

struct ExecutionOptions {
    enum class Priority {
        Low,
        Medium,
        High
    };

    bool executeImmediately;
    std::chrono::milliseconds executionInterval;
    Priority priority;

    ExecutionOptions() = default;
    ExecutionOptions(bool executeImmediately, std::chrono::milliseconds interval, Priority prio)
        : executeImmediately(executeImmediately), executionInterval(interval), priority(prio) {}
};

}