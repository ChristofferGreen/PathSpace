#include "layer/io/PathIOGamepad.hpp"

#include <algorithm>
#include <chrono>
#if defined(PATHIO_BACKEND_MACOS)
extern "C" bool PSGameControllerApplyRumble(int deviceId, float low, float high, uint32_t durationMs) __attribute__((weak_import));
#endif

namespace SP {

PathIOGamepad::PathIOGamepad(BackendMode mode, int deviceId)
    : mode_(mode), deviceId_(deviceId) {
#if defined(PATHIO_BACKEND_MACOS)
    if (mode_ == BackendMode::Auto) {
        // Prefer OS backend if the haptics helper is linked; otherwise fall back to Simulation.
        if (&PSGameControllerApplyRumble != nullptr) {
            mode_ = BackendMode::OS;
        } else {
            mode_ = BackendMode::Simulation;
        }
    }
#else
    if (mode_ == BackendMode::Auto) {
        // On non-macOS platforms, default Auto to Simulation.
        mode_ = BackendMode::Simulation;
    }
#endif
    // No background worker needed for v1; events arrive via simulate* or platform hooks when integrated.
}

PathIOGamepad::~PathIOGamepad() {
    // No worker currently; if we add one, ensure proper teardown here.
}

// Event writes are accepted via in() at '.../events'

// ---- Haptics (command) API ----

bool PathIOGamepad::hapticsSupported() const noexcept {
    if (mode_ == BackendMode::Simulation) return true;
#if defined(PATHIO_BACKEND_MACOS)
    return &PSGameControllerApplyRumble != nullptr;
#else
    return false;
#endif
}

std::optional<Error> PathIOGamepad::applyHaptics(Command const& cmd) {
    // Clamp inputs
    Command clamped = cmd;
    clamped.lowFrequency  = clamp01_(clamped.lowFrequency);
    clamped.highFrequency = clamp01_(clamped.highFrequency);

    if (mode_ == BackendMode::Simulation) {
        {
            std::lock_guard<std::mutex> lg(mutex_);
            lastHaptics_ = clamped;
        }
        // Simulation has no external effect; accept the command.
        return std::nullopt;
    }
#if defined(PATHIO_BACKEND_MACOS)
    if (&PSGameControllerApplyRumble != nullptr) {
        bool ok = PSGameControllerApplyRumble(deviceId_, clamped.lowFrequency, clamped.highFrequency, clamped.durationMs);
        if (ok) return std::nullopt;
        return Error{Error::Code::InvalidPermissions, "Gamepad haptics command rejected by GameController"};
    }
    return Error{Error::Code::InvalidPermissions, "Gamepad haptics OS helper not linked (macOS)"};
#else
    return Error{Error::Code::InvalidPermissions, "Gamepad haptics unsupported on this platform"};
#endif
}

// ---- PathSpaceBase overrides ----

auto PathIOGamepad::in(Iterator const& path, InputData const& data) -> InsertReturn {
    InsertReturn ret;

    // Tail under the current iterator (relative to mount)
    const std::string tail = std::string(path.currentToEnd());
    if (auto handled = pushConfig_.handleInsert(tail, data)) {
        return *handled;
    }

    // 1) Accept and enqueue Event writes at ".../events"
    if (data.metadata.typeInfo == &typeid(Event)) {
        bool ok = (tail == "events") || endsWith_(tail, "/events");
        if (!ok) {
            ret.errors.emplace_back(Error::Code::InvalidPath, "Unsupported path for gamepad event; expected 'events'");
            return ret;
        }
        auto const* ev = reinterpret_cast<Event const*>(data.obj);
        if (!ev) {
            ret.errors.emplace_back(Error::Code::MalformedInput, "Null Event pointer");
            return ret;
        }
        enqueue_(*ev);
        ret.nbrValuesInserted = 1;
        return ret;
    }

    // 2) Accept HapticsCommand at ".../rumble" or ".../haptics"
    if (data.metadata.typeInfo == &typeid(Command)) {
        auto const* cmd = reinterpret_cast<Command const*>(data.obj);
        if (!cmd) {
            ret.errors.emplace_back(Error::Code::MalformedInput, "Null HapticsCommand pointer");
            return ret;
        }

        if (!tail.empty()) {
            bool ok =
                (tail == "rumble" || tail == "haptics") ||
                endsWith_(tail, "/rumble") || endsWith_(tail, "/haptics");
            if (!ok) {
                ret.errors.emplace_back(Error::Code::InvalidPath, "Unsupported control path for gamepad haptics");
                return ret;
            }
        }

        if (auto err = applyHaptics(*cmd)) {
            ret.errors.push_back(*err);
            return ret;
        }

        ret.nbrValuesInserted = 1;
        return ret;
    }

    // 3) Otherwise, reject by type
    ret.errors.emplace_back(Error::Code::InvalidType, "PathIOGamepad only accepts Event at 'events' or HapticsCommand at 'rumble'/'haptics'");
    return ret;
}

auto PathIOGamepad::out(Iterator const& path,
                        InputMetadata const& inputMetadata,
                        Out const& options,
                        void* obj) -> std::optional<Error> {
    const std::string tail = std::string(path.currentToEnd());
    if (auto handled = pushConfig_.handleRead(tail, inputMetadata, obj); handled.handled) {
        return handled.error;
    }

    if (inputMetadata.typeInfo != &typeid(Event)) {
        return Error{Error::Code::InvalidType, "PathIOGamepad only supports GamepadEvent reads"};
    }
    if (obj == nullptr) {
        return Error{Error::Code::MalformedInput, "Null output pointer for PathIOGamepad::out"};
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

    // Non-blocking requested
    if (!options.doBlock) {
        return Error{Error::Code::NoObjectFound, "No gamepad event available"};
    }

    // Blocking wait
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::duration_cast<std::chrono::steady_clock::duration>(options.timeout);
    {
        std::unique_lock<std::mutex> lk(mutex_);
        if (!cv_.wait_until(lk, deadline, [this] { return !queue_.empty(); })) {
            return Error{Error::Code::Timeout, "Timed out waiting for gamepad event"};
        }
        if (queue_.empty()) {
            return Error{Error::Code::NoObjectFound, "No gamepad event available after wake"};
        }
        Event ev = queue_.front();
        if (options.doPop) {
            queue_.pop_front();
        }
        *reinterpret_cast<Event*>(obj) = ev;
    }
    return std::nullopt;
}

auto PathIOGamepad::shutdown() -> void {
    // No special shutdown at the moment.
}

auto PathIOGamepad::notify(std::string const& /*notificationPath*/) -> void {
    // Provider does not rely on external notifications.
}

void PathIOGamepad::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    PathSpaceBase::adoptContextAndPrefix(std::move(context), prefix);
    std::lock_guard<std::mutex> lg(mutex_);
    mountPrefix_ = std::move(prefix);
}

// ---- Internals ----

void PathIOGamepad::enqueue_(Event const& ev) {
    {
        std::lock_guard<std::mutex> lg(mutex_);
        queue_.push_back(ev);
    }
    cv_.notify_all();
    notifyTargets_();
}

void PathIOGamepad::notifyTargets_() {
    if (auto ctx = this->getContext()) {
        std::string mount;
        {
            std::lock_guard<std::mutex> lg(mutex_);
            mount = mountPrefix_;
        }
        if (!mount.empty()) {
            ctx->notify(mount);
            ctx->notify(mount + "/events");
        } else {
            ctx->notifyAll();
        }
    }
}

uint64_t PathIOGamepad::nowNs_() {
    auto nowDur = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(nowDur).count());
}

float PathIOGamepad::clamp01_(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

bool PathIOGamepad::endsWith_(std::string const& s, std::string const& suffix) {
    if (s.size() < suffix.size()) return false;
    return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

} // namespace SP
