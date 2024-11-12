#include "core/WaitMap.hpp"

namespace SP {

WaitMap::Guard::Guard(WaitMap& waitMap, ConcretePathString const& path, std::unique_lock<std::mutex> lock) : waitMap(waitMap), path(path), lock(std::move(lock)) {}

auto WaitMap::wait(ConcretePathStringView const& path) -> Guard {
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

auto WaitMap::notify(GlobPathStringView const& path) -> void {
    this->notify(ConcretePathString{path.getPath()});
}

auto WaitMap::notify(GlobPathString const& path) -> void { // ToDo: Fix glob path situation
    this->notify(ConcretePathString{path.getPath()});
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