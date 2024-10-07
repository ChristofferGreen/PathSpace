#pragma once

#include <chrono>
#include <optional>

struct BlockOptions {
    enum class Behavior {
        DontWait,
        WaitForExecution,
        WaitForExistence,
        Wait
    };

    Behavior behavior = Behavior::DontWait;
    std::optional<std::chrono::milliseconds> timeout;
};