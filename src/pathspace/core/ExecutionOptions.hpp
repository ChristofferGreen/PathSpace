#pragma once
#include <chrono>

namespace SP {

struct ExecutionOptions {
    enum class Category {
        Immediate,
        Lazy
    };
    enum class Location { // ToDo: Implement
        Any,
        MainThread
    };
    enum class Priority { // ToDo: Implement
        Low,
        Middle,
        High
    };

    Category                                 category = Category::Immediate;
    Location                                 location = Location::Any;
    Priority                                 priority = Priority::Middle;
    std::optional<std::chrono::milliseconds> updateInterval;   // ToDo: Implement
    std::optional<uint32_t>                  maxNbrExecutions; // ToDo: Implement
};

} // namespace SP