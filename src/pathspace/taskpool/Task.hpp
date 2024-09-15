#pragma once
#include "core/ExecutionOptions.hpp"
#include "path/ConstructiblePath.hpp"
#include <cassert>
#include <functional>
#include <variant>

namespace SP {
struct PathSpace;
using FunctionPointerTask = void (*)(PathSpace* space, ConstructiblePath const& path, ExecutionOptions const& executionOptions, void* userSuppliedFunction);

struct Task {
    void* userSuppliedFunction = nullptr;
    FunctionPointerTask wrapperFunction = nullptr;
    PathSpace* space = nullptr;
    ConstructiblePath path;
    ExecutionOptions executionOptions;

    auto execute() -> void {
        assert(this->userSuppliedFunction != nullptr);
        assert(this->space != nullptr);
        this->wrapperFunction(this->space, this->path, this->executionOptions, this->userSuppliedFunction);
    }
};

} // namespace SP