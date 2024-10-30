#pragma once
#include "TaskState.hpp"
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"

#include <any>
#include <cassert>
#include <functional>
#include <future>

namespace SP {
struct PathSpace;

struct Task {
    TaskStateAtomic state;

    PathSpace* space = nullptr;                  // Returned values from lazy executions will be inserted here
    ConstructiblePath pathToInsertReturnValueTo; // On this path, the return value will be inserted.
    std::optional<ExecutionOptions> executionOptions;

    std::function<void(Task const& task, void* obj, bool const objIsData)> function;
    std::shared_ptr<std::future<void>> executionFuture;

    // When using a timeout on a ReadExtract execution we need a safe space to store the value.
    std::any resultStorage;
    void* resultPtr = nullptr;
    std::function<void(void const* const from, void* const to)> resultCopy;
};

} // namespace SP