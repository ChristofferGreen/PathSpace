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
    enum class Location {
        Any,
        MainThread
    };
    enum class Priority {
        Low,
        Middle,
        High
    };

    Category category = Category::Immediate;
    Location location = Location::Any;
    Priority priority = Priority::Middle;
    std::optional<std::chrono::milliseconds> updateInterval;
    std::optional<uint32_t> maxNbrExecutions;
    bool cacheResult = false; // Converts function pointer/object to stored value for future read/grab operations
};

} // namespace SP