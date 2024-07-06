#pragma once
#include "pathspace/path/ConcretePath.hpp"

#include <atomic>

namespace SP {

struct PathSpace;

class ExecutionBase {
public:
    virtual ~ExecutionBase() = default;
    virtual void execute(ConcretePathString const &path, PathSpace &space, std::atomic<bool> &alive) = 0;
};

}