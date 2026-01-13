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
        std::scoped_lock<std::mutex> lg(this->stateMutex);
        return this->readyFlag;
    }

    void wait() const override {
        std::unique_lock<std::mutex> lock(this->stateMutex);
        this->stateCv.wait(lock, [&]{ return this->readyFlag; });
    }

    bool wait_until(std::chrono::time_point<std::chrono::steady_clock> deadline) const override {
        std::unique_lock<std::mutex> lock(this->stateMutex);
        return this->stateCv.wait_until(lock, deadline, [&]{ return this->readyFlag; });
    }

    [[nodiscard]] const std::type_info& type() const override {
        return typeid(T);
    }

    bool copy_to(void* dest) const override {
        if (dest == nullptr) return false;
        std::scoped_lock<std::mutex> lg(this->stateMutex);
        if (!this->readyFlag || !this->value.has_value()) return false;
        *static_cast<T*>(dest) = *this->value;
        return true;
    }

    // Producer-side API
    // Set the result if not already set; returns true on first successful set.
    bool set_value(T v) {
        {
            std::scoped_lock<std::mutex> lg(this->stateMutex);
            if (this->readyFlag) return false;
            this->value = std::move(v);
            this->readyFlag = true;
        }
        this->stateCv.notify_all();
        return true;
    }

    // Consumer-side typed access (copy). Returns false if not ready.
    bool get(T& out) const {
        std::scoped_lock<std::mutex> lg(this->stateMutex);
        if (!this->readyFlag || !this->value.has_value()) return false;
        out = *this->value;
        return true;
    }

private:
    // mutable to allow const wait/copy operations to lock
    mutable std::mutex              stateMutex;
    mutable std::condition_variable stateCv;
    std::optional<T>                value;
    bool                            readyFlag{false};
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
        : state(std::move(state)) {}

    template <typename T>
    explicit FutureAny(FutureT<T> const& fut);

    [[nodiscard]] bool valid() const { return static_cast<bool>(this->state); }

    [[nodiscard]] bool ready() const {
        return this->state ? this->state->ready() : false;
    }

    void wait() const {
        if (this->state) this->state->wait();
    }

    template <typename Rep, typename Period>
    bool wait_for(std::chrono::duration<Rep, Period> const& d) const {
        return wait_until(std::chrono::steady_clock::now()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration>(d));
    }

    bool wait_until(std::chrono::time_point<std::chrono::steady_clock> deadline) const {
        return this->state ? this->state->wait_until(deadline) : true;
    }

    [[nodiscard]] const std::type_info& type() const {
        if (this->state) return this->state->type();
        return typeid(void);
    }

    // Non-blocking result copy. Returns false if not ready or invalid.
    bool try_copy_to(void* dest) const {
        return this->state ? this->state->copy_to(dest) : false;
    }

    // Blocking result copy. Returns false only if invalid.
    bool copy_to(void* dest) const {
        if (!this->state) return false;
        this->state->wait();
        return this->state->copy_to(dest);
    }

    std::shared_ptr<ISharedState> shared_state() const { return this->state; }

private:
    std::shared_ptr<ISharedState> state;
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
        : state(std::move(state)) {}

    [[nodiscard]] bool valid() const { return static_cast<bool>(this->state); }
    [[nodiscard]] bool ready() const { return this->state ? this->state->ready() : false; }

    void wait() const { if (this->state) this->state->wait(); }

    template <typename Rep, typename Period>
    bool wait_for(std::chrono::duration<Rep, Period> const& d) const {
        return wait_until(std::chrono::steady_clock::now()
                          + std::chrono::duration_cast<std::chrono::steady_clock::duration>(d));
    }

    bool wait_until(std::chrono::time_point<std::chrono::steady_clock> deadline) const {
        return this->state ? this->state->wait_until(deadline) : true;
    }

    // Copy result to out if ready; returns false if not ready or invalid.
    bool try_get(T& out) const {
        return this->state ? this->state->get(out) : false;
    }

    // Blocking get; returns false if invalid (no state).
    bool get(T& out) const {
        if (!this->state) return false;
        this->state->wait();
        return this->state->get(out);
    }

    // Bridge to type-erased FutureAny
    [[nodiscard]] FutureAny to_any() const {
        return FutureAny(this->state);
    }

    std::shared_ptr<SharedState<T>> shared_state() const { return this->state; }

private:
    std::shared_ptr<SharedState<T>> state;
};

// FutureAny construction from typed future (defined after FutureT)
template <typename T>
FutureAny::FutureAny(FutureT<T> const& fut)
    : state(fut.shared_state()) {}

/**
 * PromiseT<T> — producer-side handle to fulfill a FutureT<T>.
 */
template <typename T>
class PromiseT {
public:
    PromiseT()
        : state(std::make_shared<SharedState<T>>()) {}

    explicit PromiseT(std::shared_ptr<SharedState<T>> state)
        : state(std::move(state)) {}

    // Obtain a typed future associated with this promise.
    [[nodiscard]] FutureT<T> get_future() const {
        return FutureT<T>(this->state);
    }

    // Set the value if not already set. Returns true on success.
    bool set_value(T v) {
        return this->state->set_value(std::move(v));
    }

    std::shared_ptr<SharedState<T>> shared_state() const { return this->state; }

private:
    std::shared_ptr<SharedState<T>> state;
};

} // namespace SP
