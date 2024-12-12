#include "core/WaitMap.hpp"
#include "path/utils.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {

WaitMap::Guard::Guard(WaitMap& waitMap, std::string_view path, std::unique_lock<std::mutex> lock)
    : waitMap(waitMap), path(path), lock(std::move(lock)) {}

auto WaitMap::Guard::wait_until(std::chrono::time_point<std::chrono::system_clock> timeout) -> std::cv_status {
    return this->waitMap.getCv(path).wait_until(lock, timeout);
}

auto WaitMap::wait(std::string_view path) -> Guard {
    sp_log("WaitMap::wait for path: " + std::string(path), "WaitMap");
    return Guard(*this, path, std::unique_lock<std::mutex>(mutex));
}

auto WaitMap::notify(std::string_view path) -> void {
    sp_log("notify(glob view): " + std::string(path) + " concrete=" + std::to_string(is_concrete(path)), "WaitMap");

    std::lock_guard<std::mutex> lock(mutex);

    sp_log("Currently registered waiters:", "WaitMap");
    for (auto& [rp, _] : this->cvMap) {
        sp_log("  - " + rp, "WaitMap");
    }

    // First, handle the concrete path case - both directions
    if (is_concrete(path)) {
        // Look for exact match first
        auto it = cvMap.find(path);
        if (it != cvMap.end()) {
            sp_log("Found matching concrete path", "WaitMap");
            it->second.notify_all();
        }

        // Check if any registered glob patterns match our concrete path
        for (auto& [registeredPath, cv] : this->cvMap) {
            if (is_glob(registeredPath) && match_paths(registeredPath, path)) {
                sp_log("Found matching glob pattern: " + registeredPath, "WaitMap");
                cv.notify_all();
            }
        }
        return;
    }

    for (auto& [registeredPath, cv] : this->cvMap) {
        sp_log("Checking if glob '" + std::string(path) + "' matches registered path '" + registeredPath + "'", "WaitMap");
        if (bool const isCompleteMatch = match_paths(path, registeredPath)) {
            sp_log("Notifying waiters for matching path: " + registeredPath, "WaitMap");
            cv.notify_all();
        }
    }
}

auto WaitMap::notifyAll() -> void {
    sp_log("notifyAll called", "WaitMap");
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [path, cv] : this->cvMap) {
        sp_log("Notifying path: " + path, "WaitMap");
        cv.notify_all();
    }
}

auto WaitMap::clear() -> void {
    std::lock_guard<std::mutex> lock(mutex);
    this->cvMap.clear();
}

auto WaitMap::getCv(std::string_view path) -> std::condition_variable& {
    return this->cvMap[std::string(path)];
}

} // namespace SP