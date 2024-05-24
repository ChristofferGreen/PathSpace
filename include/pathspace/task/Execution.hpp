#pragma once
#include "pathspace/task/ExecutionBase.hpp"

namespace SP {

struct PathSpace;

template<typename ReturnType>
class Execution : public ExecutionBase {
public:
    Execution(std::function<ReturnType(ConcretePathString const&, PathSpace&, std::atomic<bool>&)> func) : func(std::move(func)) {}

    void execute(ConcretePathString const &path, PathSpace &space, std::atomic<bool> &alive) override {
        if constexpr (!std::is_void_v<ReturnType>) {
            ReturnType result = func(path, space, alive);
            if (alive.load()) {
                //space.insert(path, result); // Insert the result into PathSpace
            }
        } else {
            func(path, space, alive);
        }
        //space.removeExecution(path); // Remove the execution after it completes
    }

private:
    std::function<ReturnType(ConcretePathString const&, PathSpace&, std::atomic<bool>&)> func;
};


}