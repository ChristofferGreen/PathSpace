#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

#include "log/TaggedLogger.hpp"

namespace SP {

struct PathSpace;

/**
 * @brief Global registry of live PathSpace instances.
 *
 * Purpose:
 * - Allow code that would otherwise hold raw `PathSpace*` pointers (e.g., Tasks)
 *   to check whether a `PathSpace` is still registered (alive) before invoking
 *   methods that would dereference the pointer (such as `notify(...)`).
 *
 * Usage:
 * - `PathSpace` should register itself on construction and unregister on destruction:
 *
 *     PathSpaceRegistry::Instance().registerSpace(this);
 *     ...
 *     PathSpaceRegistry::Instance().unregisterSpace(this);
 *
 * - Callers that may hold raw `PathSpace*` should call:
 *
 *     PathSpaceRegistry::Instance().safeNotify(spacePtr, notificationPath);
 *
 *   instead of calling `spacePtr->notify(...)` directly. `safeNotify` will only
 *   call into the `PathSpace` if it is currently registered.
 *
 * Notes / limitations:
 * - This registry only tracks registrations/unregistrations and cannot fully
 *   eliminate races where a `PathSpace` is destroyed concurrently with a caller
 *   trying to notify it. To minimize races, `PathSpace` should unregister
 *   (call `unregisterSpace`) as early as possible in its destructor before
 *   performing additional teardown actions.
 */
class PathSpaceRegistry {
public:
    /**
     * Get the global singleton instance.
     *
     * Note: Implementation is in the corresponding .cpp to avoid inlining
     * heavy implementation details into every translation unit that includes
     * this header.
     */
    static PathSpaceRegistry& Instance();

    PathSpaceRegistry(const PathSpaceRegistry&) = delete;
    PathSpaceRegistry& operator=(const PathSpaceRegistry&) = delete;

    /**
     * Register a PathSpace pointer as alive.
     * Safe to call multiple times for the same pointer (idempotent).
     */
    void registerSpace(PathSpace* space);

    /**
     * Unregister a PathSpace pointer.
     * Safe to call multiple times or for a nullptr.
     */
    void unregisterSpace(PathSpace* space);

    /**
     * Check whether a PathSpace pointer is registered (alive).
     * This check is performed under lock; it avoids calling into potentially
     * destroyed objects when used as a guard before notification.
     */
    bool isRegistered(PathSpace* space);

    /**
     * If the given PathSpace pointer is currently registered, call its notify
     * method with the provided notification path. Otherwise, do nothing.
     *
     * This helper must be used in places where callers might otherwise call
     * `space->notify(...)` directly using a raw pointer that could be stale.
     */
    void safeNotify(PathSpace* space, std::string const& notificationPath);

private:
    PathSpaceRegistry();
    ~PathSpaceRegistry();

    // Helper to format a pointer as a string for logging. Implementation in .cpp.
    static std::string pointerToString(PathSpace* p);

    std::mutex mutex_;
    std::unordered_set<PathSpace*> set_;
};

} // namespace SP