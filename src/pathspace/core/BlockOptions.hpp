#pragma once

#include <chrono>
#include <optional>

struct BlockOptions {
    enum class BlockBehavior {
        DontWait,
        WaitForExecution,
        WaitForExistence,
        WaitForExecutionAndExistence
    };

    BlockBehavior behavior = BlockBehavior::DontWait;
    std::optional<std::chrono::milliseconds> timeout;
};