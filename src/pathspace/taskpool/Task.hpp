#pragma once
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"

#include <cassert>
#include <functional>
#include <future>

namespace SP {
struct PathSpace;

struct Task {
    PathSpace* space = nullptr;                  // Returned values from the execution will be inserted here
    ConstructiblePath pathToInsertReturnValueTo; // On this path, the return value will be inserted.
    ExecutionOptions executionOptions;

    std::function<void(Task const& task, void* obj, bool isOut)> function;
    mutable std::shared_ptr<std::future<void>> executionFuture;
    mutable std::vector<uint8_t> resultStorage;
};

} // namespace SP