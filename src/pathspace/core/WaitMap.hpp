#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

template <typename... Bases>
struct overload : Bases... {
    using is_transparent = void;
    using Bases::operator()...;
};

struct char_pointer_hash {
    auto operator()(const char* ptr) const noexcept {
        return std::hash<std::string_view>{}(ptr);
    }
};

using transparent_string_hash = overload<
        std::hash<std::string>,
        std::hash<std::string_view>,
        char_pointer_hash>;

namespace SP {

struct WaitMap {
    struct Guard {
        Guard(WaitMap& waitMap, std::string_view path, std::unique_lock<std::mutex> lock);

        auto wait_until(std::chrono::time_point<std::chrono::system_clock> timeout) -> std::cv_status;
        template <typename Pred>
        bool wait_until(std::chrono::time_point<std::chrono::system_clock> timeout, Pred pred) {
            return waitMap.getCv(path).wait_until(lock, timeout, std::move(pred));
        }

    private:
        WaitMap&                     waitMap;
        std::string                  path;
        std::unique_lock<std::mutex> lock;
    };

    auto wait(std::string_view path) -> Guard;
    auto notify(std::string_view path) -> void;
    auto notifyAll() -> void;
    auto clear() -> void;

    auto hasWaiters() const -> bool {
        std::lock_guard<std::mutex> lock(mutex);
        return !this->cvMap.empty();
    }

private:
    friend struct Guard;
    auto getCv(std::string_view path) -> std::condition_variable&;

    mutable std::mutex                                                                                 mutex;
    std::unordered_map<std::string, std::condition_variable, transparent_string_hash, std::equal_to<>> cvMap;
};

} // namespace SP