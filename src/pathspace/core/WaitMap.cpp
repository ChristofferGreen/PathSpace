#include "core/WaitMap.hpp"
#include "path/GlobPath.hpp"
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
    auto const globView = GlobPathStringView{path};
    sp_log("notify(glob view): " + std::string(path) + " concrete=" + std::to_string(globView.isConcrete()), "WaitMap");

    std::lock_guard<std::mutex> lock(mutex);

    sp_log("Currently registered waiters:", "WaitMap");
    for (auto& [rp, _] : cvMap) {
        sp_log("  - " + rp, "WaitMap");
    }

    // First, handle the concrete path case - both directions
    if (globView.isConcrete()) {
        // Look for exact match first (existing behavior)
        auto it = cvMap.find(path);
        if (it != cvMap.end()) {
            sp_log("Found matching concrete path", "WaitMap");
            it->second.notify_all();
        }

        // Check if any registered glob patterns match our concrete path
        for (auto& [registeredPath, cv] : cvMap) {
            auto registeredGlob = GlobPathStringView{registeredPath};
            if (registeredGlob.isGlob() && registeredGlob.match(globView)) {
                sp_log("Found matching glob pattern: " + registeredPath, "WaitMap");
                cv.notify_all();
            }
        }
        return;
    }

    // Handle the glob pattern case (existing behavior)
    for (auto& [registeredPath, cv] : cvMap) {
        sp_log("Checking if glob '" + std::string(path) + "' matches registered path '" + registeredPath + "'", "WaitMap");

        auto patternIter = globView.begin();
        auto pathGSV     = GlobPathStringView{registeredPath};
        auto pathIter    = pathGSV.begin();
        bool matches     = true;

        while (patternIter != globView.end() && pathIter != pathGSV.end()) {
            auto isMatch = (*patternIter).match((*pathIter).getName());
            sp_log("Comparing segment: " + std::string((*pathIter).getName()) + " against pattern: " + std::string((*patternIter).getName()) + " -> match=" + std::to_string(isMatch), "WaitMap");

            if (!isMatch) {
                matches = false;
                sp_log("No match on segment, breaking", "WaitMap");
                break;
            }
            ++patternIter;
            ++pathIter;
        }

        bool isCompleteMatch = matches && patternIter == globView.end() && pathIter == pathGSV.end();

        sp_log("Complete match result: " + std::to_string(isCompleteMatch), "WaitMap");

        if (isCompleteMatch) {
            sp_log("Notifying waiters for matching path: " + registeredPath, "WaitMap");
            cv.notify_all();
        }
    }
}

auto WaitMap::notifyAll() -> void {
    sp_log("notifyAll called", "WaitMap");
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [path, cv] : cvMap) {
        sp_log("Notifying path: " + path, "WaitMap");
        cv.notify_all();
    }
}

auto WaitMap::clear() -> void {
    std::lock_guard<std::mutex> lock(mutex);
    cvMap.clear();
}

auto WaitMap::getCv(std::string_view path) -> std::condition_variable& {
    return cvMap[std::string(path)]; // ToDo: We may not always need to create a string here
}

} // namespace SP