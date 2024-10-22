#pragma once
#include "path/ConcretePath.hpp"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace SP {

struct WaitMap {
    struct Guard {
        Guard(WaitMap& waitMap, ConcretePathString const& path, std::unique_lock<std::mutex> lock)
            : waitMap(waitMap), path(path), lock(std::move(lock)) {
        }

        template <typename Pred>
        bool wait_until(std::chrono::time_point<std::chrono::system_clock> timeout, Pred pred) {
            return waitMap.getCv(path).wait_until(lock, timeout, std::move(pred));
        }

    private:
        WaitMap& waitMap;
        ConcretePathString path;
        std::unique_lock<std::mutex> lock;
    };

    auto wait(ConcretePathStringView const& path) -> Guard {
        return Guard(*this, ConcretePathString{path.getPath()}, std::unique_lock<std::mutex>(mutex));
    }

    auto notify(ConcretePathStringView const& path) -> void {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cvMap.find(ConcretePathString{path.getPath()});
        if (it != cvMap.end()) {
            it->second.notify_all();
        }
    }

    auto notifyAll() -> void {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& [_, cv] : cvMap) {
            cv.notify_all();
        }
    }

private:
    friend struct Guard;

    auto getCv(ConcretePathString const& path) -> std::condition_variable& {
        return cvMap[path];
    }

    mutable std::mutex mutex;
    mutable std::unordered_map<ConcretePathString, std::condition_variable> cvMap;
};

} // namespace SP