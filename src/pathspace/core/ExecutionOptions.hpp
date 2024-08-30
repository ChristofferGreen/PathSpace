#pragma once
#include <chrono>

namespace SP {

struct ExecutionOptions {
    enum class Category {
        Immediate,
        OnReadOrGrab,
        PeriodicImmidiate,
        PeriodicOnRead,
        Never
    };
    enum class ThreadChoice {
        Any,
        Main
    };

    Category category = Category::OnReadOrGrab;
    std::optional<std::chrono::milliseconds> updateInterval;
    std::optional<uint32_t> maxNbrExecutions;
    ThreadChoice threadChoice = ThreadChoice::Any;
    bool cacheResult = false; // Converts function pointer/object to stored value for future read/grab operations
};

} // namespace SP