#pragma once
#include "pathspace/path/ConcretePath.hpp"
#include "pathspace/task/ExecutionBase.hpp"
#include "pathspace/task/PoolDeleter.hpp"

#include <memory>

struct PathSpace;

namespace SP {

struct Task {
    Task(ConcretePathString const& path, std::unique_ptr<ExecutionBase, PoolDeleter<ExecutionBase>> exec, PathSpace &space)
        : path(path), exec(std::move(exec)), space(&space) {}

    Task(Task&& other) noexcept
        : path(std::move(other.path)), exec(std::move(other.exec)), space(other.space) {}

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            path = std::move(other.path);
            exec = std::move(other.exec);
            space = other.space;
        }
        return *this;
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ConcretePathString path;
    std::unique_ptr<ExecutionBase, PoolDeleter<ExecutionBase>> exec;
    PathSpace *space;
};

}