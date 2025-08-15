#pragma once

#include <string>

namespace SP {

/**
 * NotificationSink is a minimal interface that abstracts notification delivery.
 * Tasks and worker threads should hold only a weak_ptr<NotificationSink> and
 * attempt to lock it before notifying. If locking fails, the target has been
 * destroyed and the notification should be skipped without dereferencing
 * any raw pointers.
 *
 * Typical usage:
 * - A PathSpaceBase-derived object owns a single shared_ptr<NotificationSink>
 *   whose implementation forwards to the instance's notify(path).
 * - Tasks capture only a weak_ptr<NotificationSink> plus the notification path.
 * - On task completion, the worker tries notifier.lock()->notify(path) if the
 *   lock succeeds.
 */
struct NotificationSink {
    virtual ~NotificationSink() = default;

    // Deliver a path-based notification to the owning space/view.
    virtual void notify(const std::string& notificationPath) = 0;
};

} // namespace SP