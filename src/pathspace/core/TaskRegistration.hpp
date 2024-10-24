#pragma once
#include "core/TaskToken.hpp"

namespace SP {

class PathSpace;

class TaskRegistration {
public:
    explicit TaskRegistration(TaskToken* t) : token(t) {
        if (token)
            token->registerTask();
    }

    // Non-copyable
    TaskRegistration(const TaskRegistration&) = delete;
    TaskRegistration& operator=(const TaskRegistration&) = delete;

    // Moveable
    TaskRegistration(TaskRegistration&& other) noexcept : token(other.token) {
        other.token = nullptr;
    }

    TaskRegistration& operator=(TaskRegistration&& other) noexcept {
        if (this != &other) {
            if (token)
                token->unregisterTask();
            token = other.token;
            other.token = nullptr;
        }
        return *this;
    }

    ~TaskRegistration() {
        if (token)
            token->unregisterTask();
    }

private:
    TaskToken* token;
};

} // namespace SP