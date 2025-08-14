#include "PathSpaceRegistry.hpp"

// Include the full PathSpace definition so we can call its methods (e.g. notify).
// PathSpaceRegistry.hpp forward-declares `struct PathSpace;` which is sufficient
// for declarations, but the implementation below needs the complete type.
#include "PathSpace.hpp"

#include <sstream>
#include <iomanip>
#include <cstdint>

namespace SP {

PathSpaceRegistry& PathSpaceRegistry::Instance() {
    static PathSpaceRegistry instance;
    return instance;
}

PathSpaceRegistry::PathSpaceRegistry() = default;
PathSpaceRegistry::~PathSpaceRegistry() = default;

void PathSpaceRegistry::registerSpace(PathSpace* space) {
    if (!space)
        return;
    std::lock_guard<std::mutex> lk(mutex_);
    auto const inserted = set_.insert(space).second;
    if (inserted) {
        sp_log("PathSpaceRegistry: registered PathSpace " + pointerToString(space), "PathSpaceRegistry");
    }
}

void PathSpaceRegistry::unregisterSpace(PathSpace* space) {
    if (!space)
        return;
    std::lock_guard<std::mutex> lk(mutex_);
    size_t erased = set_.erase(space);
    if (erased > 0) {
        sp_log("PathSpaceRegistry: unregistered PathSpace " + pointerToString(space), "PathSpaceRegistry");
    }
}

bool PathSpaceRegistry::isRegistered(PathSpace* space) {
    if (!space)
        return false;
    std::lock_guard<std::mutex> lk(mutex_);
    return set_.find(space) != set_.end();
}

void PathSpaceRegistry::safeNotify(PathSpace* space, std::string const& notificationPath) {
    if (!space)
        return;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (set_.find(space) == set_.end()) {
            sp_log("PathSpaceRegistry::safeNotify skipped notify for unregistered PathSpace "
                   + pointerToString(space) + " path=" + notificationPath,
                   "PathSpaceRegistry");
            return;
        }
    }
    // At this point the space was registered. Call notify outside the registry lock.
    // NOTE: A race still exists if the PathSpace unregisters immediately after we release
    // the lock — the PathSpace destructor should unregister early to minimize this window.
    try {
        sp_log("PathSpaceRegistry::safeNotify invoking notify for PathSpace " + pointerToString(space) + " path=" + notificationPath,
               "PathSpaceRegistry");
        space->notify(notificationPath);
    } catch (...) {
        sp_log("PathSpaceRegistry::safeNotify caught exception when notifying PathSpace " + pointerToString(space),
               "PathSpaceRegistry");
    }
}

std::string PathSpaceRegistry::pointerToString(PathSpace* p) {
    std::ostringstream oss;
    // Format pointer as hex with zero padding corresponding to pointer width.
    // setw expects an int; sizeof(void*)*2 fits comfortably in int on supported platforms.
    oss << "0x" << std::hex << std::setw(static_cast<int>(sizeof(void*) * 2))
        << std::setfill('0') << reinterpret_cast<uintptr_t>(p);
    return oss.str();
}

} // namespace SP