#include "layer/io/PathIOMouse.hpp"

#include <chrono>
#include <string>

namespace SP {

// Accept Event writes at ".../events" (relative to mount) and enqueue into the provider queue.
auto PathIOMouse::in(Iterator const& path, InputData const& data) -> InsertReturn {
    InsertReturn ret;

    // Path tail must be "events" (relative to mount)
    const std::string tail = std::string(path.currentToEnd());
    if (auto handled = pushConfig_.handleInsert(tail, data)) {
        return *handled;
    }

    // Only support MouseEvent writes on the events queue
    if (data.metadata.typeInfo != &typeid(Event)) {
        ret.errors.emplace_back(Error::Code::InvalidType, "PathIOMouse only accepts Event at 'events'");
        return ret;
    }

    const bool okTail = (tail == "events") || (tail.size() > 7 && tail.rfind("/events") == tail.size() - 7);
    if (!okTail) {
        ret.errors.emplace_back(Error::Code::InvalidPath, "Unsupported path for mouse event; expected 'events'");
        return ret;
    }

    auto const* ev = reinterpret_cast<Event const*>(data.obj);
    if (!ev) {
        ret.errors.emplace_back(Error::Code::MalformedInput, "Null Event pointer");
        return ret;
    }

    try {
        std::lock_guard<std::mutex> lg(mutex_);
        queue_.push_back(*ev);
    } catch (...) {
        // As a fallback (e.g., if mutex is not yet initialized on some platforms), push without locking.
        queue_.push_back(*ev);
    }
    cv_.notify_all();
    ret.nbrValuesInserted = 1;
    return ret;
}

// Serve typed mouse events with peek/pop and optional blocking semantics.
// - If options.doPop is true: pop the front event into obj; otherwise peek without consuming.
// - If the queue is empty:
//    * If options.doBlock is false: return NoObjectFound.
//    * If options.doBlock is true: wait until timeout for an event to arrive; return Timeout on expiry.
auto PathIOMouse::out(Iterator const& path,
                      InputMetadata const& inputMetadata,
                      Out const& options,
                      void* obj) -> std::optional<Error> {
    const std::string tail = std::string(path.currentToEnd());
    if (auto handled = pushConfig_.handleRead(tail, inputMetadata, obj); handled.handled) {
        return handled.error;
    }

    // Type-check: only support MouseEvent payloads here
    if (inputMetadata.typeInfo != &typeid(Event)) {
        return Error{Error::Code::InvalidType, "Mouse provider only supports MouseEvent"};
    }
    if (obj == nullptr) {
        return Error{Error::Code::MalformedInput, "Null output pointer"};
    }

    // Fast path: try without blocking
    try {
        std::lock_guard<std::mutex> lg(mutex_);
        if (!queue_.empty()) {
            Event ev = queue_.front();
            if (options.doPop) {
                queue_.pop_front();
            }
            *reinterpret_cast<Event*>(obj) = ev;
            return std::nullopt;
        }
    } catch (...) {
        // Fallback path without locking
        if (!queue_.empty()) {
            Event ev = queue_.front();
            if (options.doPop) {
                queue_.pop_front();
            }
            *reinterpret_cast<Event*>(obj) = ev;
            return std::nullopt;
        }
    }

    // No event and non-blocking read requested
    if (!options.doBlock) {
        return Error{Error::Code::NoObjectFound, "No mouse event available"};
    }

    // Blocking path: wait until an event is available or timeout expires
    auto end_at = std::chrono::steady_clock::now() +
                  std::chrono::duration_cast<std::chrono::steady_clock::duration>(options.timeout);

    // Simple spin/sleep wait with best-effort locking to avoid throwing system_error
    while (std::chrono::steady_clock::now() < end_at) {
        try {
            std::lock_guard<std::mutex> lg(mutex_);
            if (!queue_.empty()) {
                Event ev = queue_.front();
                if (options.doPop) {
                    queue_.pop_front();
                }
                *reinterpret_cast<Event*>(obj) = ev;
                return std::nullopt;
            }
        } catch (...) {
            // If locking fails, try a lockless check
            if (!queue_.empty()) {
                Event ev = queue_.front();
                if (options.doPop) {
                    queue_.pop_front();
                }
                *reinterpret_cast<Event*>(obj) = ev;
                return std::nullopt;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return Error{Error::Code::Timeout, "Timed out waiting for mouse event"};
    return std::nullopt;
}

// Cooperative shutdown: stop worker and join; also stop OS backend if active
auto PathIOMouse::shutdown() -> void {
#if defined(PATHIO_BACKEND_MACOS)
    if (running_.load(std::memory_order_acquire) && mode_ == BackendMode::OS) {
        osShutdown_();
    }
#endif
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

// Provider does not rely on external notifications
auto PathIOMouse::notify(std::string const& /*notificationPath*/) -> void {
    // no-op
}

} // namespace SP
