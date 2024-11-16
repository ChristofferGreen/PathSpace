#include "core/WaitMap.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {

WaitMap::Guard::Guard(WaitMap& waitMap, ConcretePathString const& path, std::unique_lock<std::mutex> lock) : waitMap(waitMap), path(path), lock(std::move(lock)) {}

auto WaitMap::Guard::wait_until(std::chrono::time_point<std::chrono::system_clock> timeout) -> std::cv_status {
    return this->waitMap.getCv(path).wait_until(lock, timeout);
}

auto WaitMap::wait(ConcretePathStringView const& path) -> Guard {
    sp_log("WaitMap::wait for path: " + std::string(path.getPath()), "DEBUG");
    return Guard(*this, ConcretePathString{path.getPath()}, std::unique_lock<std::mutex>(mutex));
}

auto WaitMap::notify(ConcretePathStringView const& path) -> void {
    sp_log("notify(concrete view): " + std::string(path.getPath()), "WaitMap");
    this->notify(ConcretePathString{path.getPath()});
}

auto WaitMap::notify(ConcretePathString const& path) -> void {
    sp_log("notify(concrete string): " + std::string(path.getPath()), "WaitMap");
    std::lock_guard<std::mutex> lock(mutex);
    auto                        it = cvMap.find(path);
    if (it != cvMap.end()) {
        sp_log("Found exact match for " + std::string(path.getPath()), "WaitMap");
        it->second.notify_all();
    }
}

auto WaitMap::notify(GlobPathString const& path) -> void {
    sp_log("notify(glob string): " + std::string(path.getPath()), "WaitMap");
    this->notify(GlobPathStringView{path.getPath()});
}

auto WaitMap::notify(GlobPathStringView const& path) -> void {
    sp_log("notify(glob view): " + std::string(path.getPath()) + " concrete=" + std::to_string(path.isConcrete()), "WaitMap");

    if (path.isConcrete()) {
        sp_log("Path is concrete - using direct notification", "WaitMap");
        std::lock_guard<std::mutex> lock(mutex);
        auto                        it = cvMap.find(ConcretePathString{path.getPath()});
        if (it != cvMap.end()) {
            sp_log("Found matching concrete path", "WaitMap");
            it->second.notify_all();
        }
        return;
    }

    sp_log("Path is a glob pattern - checking registered paths", "WaitMap");
    std::lock_guard<std::mutex> lock(mutex);

    // Log all registered waiters for debugging
    sp_log("Currently registered waiters:", "WaitMap");
    for (auto& [rp, _] : cvMap) {
        sp_log("  - " + std::string(rp.getPath()), "WaitMap");
    }

    for (auto& [registeredPath, cv] : cvMap) {
        sp_log("Checking if glob '" + std::string(path.getPath()) + "' matches registered path '" + std::string(registeredPath.getPath()) + "'", "WaitMap");

        auto patternIter = path.begin();
        auto pathIter    = registeredPath.begin();
        bool matches     = true;

        while (patternIter != path.end() && pathIter != registeredPath.end()) {
            auto [isMatch, isSuper] = (*patternIter).match((*pathIter).getName());
            sp_log("Comparing segment: " + std::string((*pathIter).getName()) + " against pattern: " + std::string((*patternIter).getName()) + " -> match=" + std::to_string(isMatch) + ", super=" + std::to_string(isSuper), "WaitMap");

            if (!isMatch) {
                matches = false;
                sp_log("No match on segment, breaking", "WaitMap");
                break;
            }
            ++patternIter;
            ++pathIter;
        }

        bool isCompleteMatch = matches && patternIter == path.end() && pathIter == registeredPath.end();

        sp_log("Complete match result: " + std::to_string(isCompleteMatch), "WaitMap");

        if (isCompleteMatch) {
            sp_log("Notifying waiters for matching path: " + std::string(registeredPath.getPath()), "WaitMap");
            cv.notify_all();
        }
    }
}

auto WaitMap::notifyAll() -> void {
    sp_log("notifyAll called", "WaitMap");
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [path, cv] : cvMap) {
        sp_log("Notifying path: " + std::string(path.getPath()), "WaitMap");
        cv.notify_all();
    }
}

auto WaitMap::clear() -> void {
    std::lock_guard<std::mutex> lock(mutex);
    cvMap.clear();
}

auto WaitMap::getCv(ConcretePathString const& path) -> std::condition_variable& {
    return cvMap[path];
}

} // namespace SP