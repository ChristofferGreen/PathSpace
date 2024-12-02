#pragma once
#include "path/ConcretePath.hpp"
#include "path/GlobPath.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace SP {

struct WaitMap {
    struct Guard {
        Guard(WaitMap& waitMap, ConcretePathString const& path, std::unique_lock<std::mutex> lock);

        auto wait_until(std::chrono::time_point<std::chrono::system_clock> timeout) -> std::cv_status;
        template <typename Pred>
        bool wait_until(std::chrono::time_point<std::chrono::system_clock> timeout, Pred pred) {
            return waitMap.getCv(path).wait_until(lock, timeout, std::move(pred));
        }

    private:
        WaitMap&                     waitMap;
        ConcretePathString           path;
        std::unique_lock<std::mutex> lock;
    };

    auto wait(ConcretePathStringView const& path) -> Guard;
    auto wait(ConcretePathString const& path) -> Guard;
    auto wait(GlobPathString const& path) -> Guard;
    auto wait(GlobPathStringView const& path) -> Guard;
    auto notify(ConcretePathStringView const& path) -> void;
    auto notify(ConcretePathString const& path) -> void;
    auto notify(GlobPathStringView const& path) -> void;
    auto notify(GlobPathString const& path) -> void;
    auto notifyAll() -> void;
    auto clear() -> void;

    auto hasWaiters() const -> bool {
        std::lock_guard<std::mutex> lock(mutex);
        return !this->cvMap.empty() || !this->cvMapGlob.empty();
    }

private:
    friend struct Guard;
    auto getCv(ConcretePathString const& path) -> std::condition_variable&;

    mutable std::mutex                                              mutex;
    std::unordered_map<ConcretePathString, std::condition_variable> cvMap;
    std::unordered_map<GlobPathString, std::condition_variable>     cvMapGlob;
};

} // namespace SP