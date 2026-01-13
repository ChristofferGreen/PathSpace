#pragma once

#include <chrono>
#include <memory>

namespace SP {

class Task;

/**
 * Future — lightweight handle to a Task's eventual result.
 *
 * Notes:
 * - This is a declaration-only header to avoid include cycles with Task.hpp.
 * - Member function definitions live in Future.cpp where the full Task type
 *   is available.
 * - This API provides non-templated result-copy operations to keep the
 *   implementation out of the header; templated convenience wrappers forward
 *   to the non-templated operations.
 */
class Future {
public:
    Future();
    explicit Future(std::weak_ptr<Task> task);

    // Helper to construct from a shared_ptr without exposing the constructor
    // in call sites that already have a shared Task.
    static auto FromShared(const std::shared_ptr<Task>& task) -> Future;

    // Liveness/ready checks
    [[nodiscard]] auto valid() const -> bool;
    [[nodiscard]] auto ready() const -> bool;

    // Blocking waits
    auto wait() const -> void;

    // Timed waits (generic on Clock)
    template <typename Clock, typename Duration>
    auto wait_until(std::chrono::time_point<Clock, Duration> deadline) const -> bool {
        // Convert to steady_clock to avoid issues with system clock adjustments
        auto now = Clock::now();
        if (deadline <= now)
            return this->wait_until_steady(std::chrono::steady_clock::now()); // immediate check
        auto delta = std::chrono::duration_cast<std::chrono::steady_clock::duration>(deadline - now);
        return this->wait_until_steady(std::chrono::steady_clock::now() + delta);
    }

    template <typename Rep, typename Period>
    auto wait_for(std::chrono::duration<Rep, Period> const& d) const -> bool {
        return this->wait_until_steady(std::chrono::steady_clock::now()
                                       + std::chrono::duration_cast<std::chrono::steady_clock::duration>(d));
    }

    // Result copy interfaces (non-templated). Return true on success.
    // - try_copy_result_to: non-blocking; returns false if not ready or invalid.
    // - copy_result_to: blocking; returns false if the task expires unexpectedly.
    // - copy_result_for: timed; returns false on timeout or expiration.
    auto try_copy_result_to(void* dest) const -> bool;
    auto copy_result_to(void* dest) const -> bool;

    template <typename Rep, typename Period>
    auto copy_result_for(void* dest, std::chrono::duration<Rep, Period> const& d) const -> bool {
        if (!this->wait_for(d))
            return false;
        return this->copy_result_to(dest);
    }

    // Access underlying weak task pointer
    [[nodiscard]] auto weak_task() const -> std::weak_ptr<Task>;

private:
    // Steady-clock-based timed wait used by templated wait helpers
    auto wait_until_steady(std::chrono::time_point<std::chrono::steady_clock> deadline) const -> bool;

    std::weak_ptr<Task> taskWeak;
};

} // namespace SP
