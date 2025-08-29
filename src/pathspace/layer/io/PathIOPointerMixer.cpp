#include "layer/io/PathIOPointerMixer.hpp"

#include <chrono>

namespace SP {

auto PathIOPointerMixer::in(Iterator const& path, InputData const& data) -> InsertReturn {
    InsertReturn ret;

    // Only support Event payloads here
    if (data.metadata.typeInfo != &typeid(Event)) {
        ret.errors.emplace_back(Error::Code::InvalidType, "PathIOPointerMixer only accepts Event at 'events'");
        return ret;
    }

    // Path tail must be "events" (relative to mount)
    const std::string tail = std::string(path.currentToEnd());
    const bool okTail = (tail == "events") || (tail.size() > 7 && tail.rfind("/events") == tail.size() - 7);
    if (!okTail) {
        ret.errors.emplace_back(Error::Code::InvalidPath, "Unsupported path for pointer event; expected 'events'");
        return ret;
    }

    auto const* ev = reinterpret_cast<Event const*>(data.obj);
    if (!ev) {
        ret.errors.emplace_back(Error::Code::MalformedInput, "Null Event pointer");
        return ret;
    }

    {
        std::lock_guard<std::mutex> lg(mutex_);
        queue_.push_back(*ev);
    }
    cv_.notify_all();
    ret.nbrValuesInserted = 1;
    return ret;
}

auto PathIOPointerMixer::out(Iterator const& /*path*/,
                             InputMetadata const& inputMetadata,
                             Out const& options,
                             void* obj) -> std::optional<Error> {
    // Type-check: only support Event payloads here
    if (inputMetadata.typeInfo != &typeid(Event)) {
        return Error{Error::Code::InvalidType, "PathIOPointerMixer only supports PointerEvent"};
    }
    if (obj == nullptr) {
        return Error{Error::Code::MalformedInput, "Null output pointer for PathIOPointerMixer::out"};
    }

    // Fast path: try without blocking
    {
        std::lock_guard<std::mutex> lg(mutex_);
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
        return Error{Error::Code::NoObjectFound, "No pointer event available"};
    }

    // Blocking path: wait until an event is available or timeout expires
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(options.timeout);

    {
        std::unique_lock<std::mutex> lk(mutex_);
        // Wait predicate: queue is non-empty or deadline reached
        if (!cv_.wait_until(lk, deadline, [this] { return !queue_.empty(); })) {
            return Error{Error::Code::Timeout, "Timed out waiting for pointer event"};
        }
        // Event is available now
        if (queue_.empty()) {
            // Rare race: event consumed by another thread before we reacquired the lock
            return Error{Error::Code::NoObjectFound, "No pointer event available after wake"};
        }
        Event ev = queue_.front();
        if (options.doPop) {
            queue_.pop_front();
        }
        *reinterpret_cast<Event*>(obj) = ev;
    }
    return std::nullopt;
}

auto PathIOPointerMixer::shutdown() -> void {
    // No special shutdown for the mixer
}

auto PathIOPointerMixer::notify(std::string const& /*notificationPath*/) -> void {
    // Provider does not rely on external notifications
}

uint64_t PathIOPointerMixer::nowNs_() {
    auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
}

} // namespace SP