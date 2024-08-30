#pragma once
#include <functional>
#include <variant>

namespace SP {
using FunctionPointerTask = void (*)(void* const, void*);

struct Task {
    std::variant<std::function<void()>, FunctionPointerTask> callable;
    void* functionPointer = nullptr;
    void* returnData = nullptr;
};

} // namespace SP