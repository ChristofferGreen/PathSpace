#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <typeinfo>
#include <utility>

namespace SP {

// Forward-declare typed future/promise templates.
template <typename T>
class FutureT;

template <typename T>
class PromiseT;

/**
 * ISharedState — type-erased shared state interface for asynchronous results.
 *
 * Implementations provide:
 * - readiness queries
 * - blocking/timed waits
 * - type information
 * - a way to copy the stored result into a caller-provided buffer
 *
 * This interface is intended to back both type-erased and typed futures.
 */
class ISharedState {
public:
    virtual ~ISharedState() = default;

    // True if a value was set and the state will not change further.
    [[nodiscard]] virtual bool ready() const = 0;

    // Block until ready() becomes true.
    virtual void wait() const = 0;

    // Block until deadline; return true if ready() is true at return.
    virtual bool wait_until(std::chrono::time_point<std::chrono::steady_clock> deadline) const = 0;

    // The concrete C++ type stored in this shared state.
    [[nodiscard]] virtual const std::type_info& type() const = 0;

    // Copy the result to the provided, non-null destination.
    // Returns false if not ready or destination is null.
    virtual bool copy_to(void* dest) const = 0;
};

/**
 * SharedState<T> — typed shared state for asynchronous results.
 *
 * Stores a single value of type T, supports readiness checks and blocking waits.
 * Thread-safe: multiple waiters permitted; first set_value "wins".
 */
template <typename T>
class SharedState final : public ISharedState, public std::enable_shared_from_this<SharedState<T>> {
public:
    SharedState() = default;
    ~SharedState() override = default;

    SharedState(SharedState const&)            = delete;
    SharedState& operator=(SharedState const&) = delete;

    // ISharedState interface
    [[nodiscard]] bool ready() const override {
        std::scoped_lock<std::mutex> lg(mutex_);
        return ready_;
    }

    void wait() const override {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]{ return ready_; });
    }

    bool wait_until(std::chrono::time_point<std::chrono::steady_clock> deadline) const override {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_until(lock, deadline, [&]{ return ready_; });
    }

    [[nodiscard]] const std::type_info& type() const override {
        return typeid(T);
    }

    bool copy_to(void* dest) const override {
        if (dest == nullptr) return false;
        std::scoped_lock<std::mutex> lg(mutex_);
        if (!ready_ || !value_.has_value()) return false;
        *static_cast<T*>(dest) = *value_;
        return true;
    }

    // Producer-side API
    // Set the result if not already set; returns true on first successful set.
    bool set_value(T v) {
        {
            std::scoped_lock<std::mutex> lg(mutex_);
            if (ready_) return false;
            value_ = std::move(v);
            ready_ = true;
        }
        cv_.notify_all();
        return true;
    }

    // Consumer-side typed access (copy). Returns false if not ready.
    bool get(T& out) const {
        std::scoped_lock<std::mutex> lg(mutex_);
        if (!ready_ || !value_.has_value()) return false;
        out = *value_;
        return true;
    }

private:
    // mutable to allow const wait/copy operations to lock
    mutable std::mutex              mutex_;
    mutable std::condition_variable cv_;
    std::optional<T>                value_;
    bool                            ready_{false};
};

/**
 * FutureAny — type-erased future based on ISharedState.
 *
 * Provides readiness checks, waits, and a copy_to(void*) interface to obtain
 * the stored value given a destination pointer of the correct type.
 */
class FutureAny {
public:
    FutureAny() = default;

    explicit FutureAny(std::shared_ptr<ISharedState> state)
        : state_(std::move(state)) {}

    template <typename T>
    explicit FutureAny(FutureT<T> const& fut);

    [[nodiscard]] bool valid() const { return static_cast<bool>(state_); }

    [[nodiscard]] bool ready() const {
        return state_ ? state_->ready() : false;
    }

    void wait() const {
        if (state_) state_->wait();
    }

    template <typename Rep, typename Period>
    bool wait_for(std::chrono::duration<Rep, Period> const& d) const {
        return wait_until(std::chrono::steady_clock::now()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration>(d));
    }

    bool wait_until(std::chrono::time_point<std::chrono::steady_clock> deadline) const {
        return state_ ? state_->wait_until(deadline) : true;
    }

    [[nodiscard]] const std::type_info& type() const {
        if (state_) return state_->type();
        return typeid(void);
    }

    // Non-blocking result copy. Returns false if not ready or invalid.
    bool try_copy_to(void* dest) const {
        return state_ ? state_->copy_to(dest) : false;
    }

    // Blocking result copy. Returns false only if invalid.
    bool copy_to(void* dest) const {
        if (!state_) return false;
        state_->wait();
        return state_->copy_to(dest);
    }

    std::shared_ptr<ISharedState> shared_state() const { return state_; }

private:
    std::shared_ptr<ISharedState> state_;
};

/**
 * FutureT<T> — typed future backed by SharedState<T>.
 *
 * Provides typed accessors as well as a bridge to FutureAny for type erasure.
 */
template <typename T>
class FutureT {
public:
    FutureT() = default;
    explicit FutureT(std::shared_ptr<SharedState<T>> state)
        : state_(std::move(state)) {}

    [[nodiscard]] bool valid() const { return static_cast<bool>(state_); }
    [[nodiscard]] bool ready() const { return state_ ? state_->ready() : false; }

    void wait() const { if (state_) state_->wait(); }

    template <typename Rep, typename Period>
    bool wait_for(std::chrono::duration<Rep, Period> const& d) const {
        return wait_until(std::chrono::steady_clock::now()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration>(d));
    }

    bool wait_until(std::chrono::time_point<std::chrono::steady_clock> deadline) const {
        return state_ ? state_->wait_until(deadline) : true;
    }

    // Copy result to out if ready; returns false if not ready or invalid.
    bool try_get(T& out) const {
        return state_ ? state_->get(out) : false;
    }

    // Blocking get; returns false if invalid (no state).
    bool get(T& out) const {
        if (!state_) return false;
        state_->wait();
        return state_->get(out);
    }

    // Bridge to type-erased FutureAny
    [[nodiscard]] FutureAny to_any() const {
        return FutureAny(state_);
    }

    std::shared_ptr<SharedState<T>> shared_state() const { return state_; }

private:
    std::shared_ptr<SharedState<T>> state_;
};

// FutureAny construction from typed future (defined after FutureT)
template <typename T>
FutureAny::FutureAny(FutureT<T> const& fut)
    : state_(fut.shared_state()) {}

/**
 * PromiseT<T> — producer-side handle to fulfill a FutureT<T>.
 */
template <typename T>
class PromiseT {
public:
    PromiseT()
        : state_(std::make_shared<SharedState<T>>()) {}

    explicit PromiseT(std::shared_ptr<SharedState<T>> state)
        : state_(std::move(state)) {}

    // Obtain a typed future associated with this promise.
    [[nodiscard]] FutureT<T> get_future() const {
        return FutureT<T>(state_);
    }

    // Set the value if not already set. Returns true on success.
    bool set_value(T v) {
        return state_->set_value(std::move(v));
    }

    std::shared_ptr<SharedState<T>> shared_state() const { return state_; }

private:
    std::shared_ptr<SharedState<T>> state_;
};

} // namespace SP