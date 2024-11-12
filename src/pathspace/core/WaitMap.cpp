#include "core/WaitMap.hpp"
#include "utils/TaggedLogger.hpp"

namespace SP {

WaitMap::Guard::Guard(WaitMap& waitMap, ConcretePathString const& path, std::unique_lock<std::mutex> lock) : waitMap(waitMap), path(path), lock(std::move(lock)) {}

auto WaitMap::wait(ConcretePathStringView const& path) -> Guard {
    sp_log("WaitMap::wait for path: " + std::string(path.getPath()), "DEBUG");
    return Guard(*this, ConcretePathString{path.getPath()}, std::unique_lock<std::mutex>(mutex));
}

auto WaitMap::notify(ConcretePathStringView const& path) -> void {
    this->notify(ConcretePathString{path.getPath()});
}

auto WaitMap::notify(ConcretePathString const& path) -> void {
    std::lock_guard<std::mutex> lock(mutex);
    auto                        it = cvMap.find(path);
    if (it != cvMap.end()) {
        it->second.notify_all();
    }
}

auto WaitMap::notify(GlobPathString const& path) -> void {
    this->notify(GlobPathStringView{path.getPath()});
}

auto WaitMap::notify(GlobPathStringView const& path) -> void {
    sp_log("notify(glob): " + std::string(path.getPath()), "DEBUG");

    if (path.isConcrete()) {
        sp_log("Path is concrete - using direct notification", "DEBUG");
        std::lock_guard<std::mutex> lock(mutex);
        auto                        it = cvMap.find(ConcretePathString{path.getPath()});
        if (it != cvMap.end()) {
            sp_log("Found matching concrete path", "DEBUG");
            it->second.notify_all();
        }
        return;
    }

    sp_log("Path is a glob pattern - checking registered paths", "DEBUG");
    std::lock_guard<std::mutex> lock(mutex);

    for (auto& [registeredPath, cv] : cvMap) {
        sp_log("Checking registered path: " + std::string(registeredPath.getPath()), "DEBUG");

        // Detailed comparison logging
        auto patternIter = path.begin();
        auto pathIter    = registeredPath.begin();
        bool matches     = true;

        while (patternIter != path.end() && pathIter != registeredPath.end()) {
            auto [isMatch, isSuper] = (*patternIter).match((*pathIter).getName());
            sp_log("  Comparing segment: '" + std::string((*pathIter).getName()) + "' against pattern: '" + std::string((*patternIter).getName()) + "' -> match=" + std::to_string(isMatch) + ", super=" + std::to_string(isSuper), "DEBUG");

            if (!isMatch) {
                matches = false;
                break;
            }
            ++patternIter;
            ++pathIter;
        }

        // Check complete match
        bool isCompleteMatch = matches && patternIter == path.end() && pathIter == registeredPath.end();

        sp_log("Match result: " + std::to_string(isCompleteMatch) + " (matches=" + std::to_string(matches) + ", patternAtEnd=" + std::to_string(patternIter == path.end()) + ", pathAtEnd=" + std::to_string(pathIter == registeredPath.end()) + ")",
               "DEBUG");

        if (isCompleteMatch) {
            sp_log("Notifying waiters for matching path: " + std::string(registeredPath.getPath()), "DEBUG");
            cv.notify_all();
        }
    }
}

auto WaitMap::notifyAll() -> void {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [_, cv] : cvMap) {
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