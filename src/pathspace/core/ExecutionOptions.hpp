#pragma once
#include <chrono>

namespace SP {

struct ExecutionOptions {
    enum class Category {
        Immediate,
        Lazy
    };

    Category                                 category = Category::Immediate;
    std::optional<std::chrono::milliseconds> updateInterval;   // ToDo: Implement
    std::optional<uint32_t>                  maxNbrExecutions; // ToDo: Implement
};

} // namespace SP