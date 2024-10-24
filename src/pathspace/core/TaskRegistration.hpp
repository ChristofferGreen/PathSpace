#pragma once
#include "core/TaskToken.hpp"

namespace SP {

class PathSpace;

// In TaskRegistration.hpp
class TaskRegistration {
public:
    explicit TaskRegistration(TaskToken* t) : token(t) {
        if (token) {
            token->registerTask();
            registered = true;
        }
    }

    ~TaskRegistration() {
        if (token && registered) {
            token->unregisterTask();
        }
    }

    // Add move operations
    TaskRegistration(TaskRegistration&& other) noexcept : token(other.token), registered(other.registered) {
        other.token = nullptr;
        other.registered = false;
    }

    TaskRegistration& operator=(TaskRegistration&& other) noexcept {
        if (this != &other) {
            if (token && registered) {
                token->unregisterTask();
            }
            token = other.token;
            registered = other.registered;
            other.token = nullptr;
            other.registered = false;
        }
        return *this;
    }

    // Prevent copies
    TaskRegistration(const TaskRegistration&) = delete;
    TaskRegistration& operator=(const TaskRegistration&) = delete;

private:
    TaskToken* token;
    bool registered = false;
};

} // namespace SP