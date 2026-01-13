#include "task/Future.hpp"
#include "task/Task.hpp"

#include <chrono>
#include <thread>

namespace SP {

Future::Future() = default;

Future::Future(std::weak_ptr<Task> task)
    : taskWeak(std::move(task)) {}

auto Future::FromShared(const std::shared_ptr<Task>& task) -> Future {
    return Future(std::weak_ptr<Task>(task));
}

auto Future::valid() const -> bool {
    return !this->taskWeak.expired();
}

auto Future::ready() const -> bool {
    if (auto t = this->taskWeak.lock()) {
        return t->isCompleted();
    }
    return false;
}

auto Future::wait() const -> void {
    if (auto t = this->taskWeak.lock()) {
        while (!t->isCompleted()) {
            std::this_thread::yield();
        }
    }
}

auto Future::wait_until_steady(std::chrono::time_point<std::chrono::steady_clock> deadline) const -> bool {
    if (auto t = this->taskWeak.lock()) {
        while (!t->isCompleted()) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
            std::this_thread::yield();
        }
        return true;
    }
    return false;
}

auto Future::try_copy_result_to(void* dest) const -> bool {
    if (!this->ready())
        return false;
    if (auto t = this->taskWeak.lock()) {
        t->resultCopy(dest);
        return true;
    }
    return false;
}

auto Future::copy_result_to(void* dest) const -> bool {
    this->wait();
    if (auto t = this->taskWeak.lock()) {
        t->resultCopy(dest);
        return true;
    }
    return false;
}

auto Future::weak_task() const -> std::weak_ptr<Task> {
    return this->taskWeak;
}

} // namespace SP
