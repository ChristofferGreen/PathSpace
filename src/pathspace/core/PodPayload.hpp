#pragma once

#include "core/Error.hpp"
#include "type/InputMetadata.hpp"
#include "type/InputMetadataT.hpp"

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <typeinfo>
#include <thread>

namespace SP {

namespace testing {
using PodPayloadPushHook = void (*)();
inline std::atomic<PodPayloadPushHook>& podPayloadPushHook() {
    static std::atomic<PodPayloadPushHook> hook{nullptr};
    return hook;
}
inline void SetPodPayloadPushHook(PodPayloadPushHook hook) {
    podPayloadPushHook().store(hook, std::memory_order_relaxed);
}
inline PodPayloadPushHook GetPodPayloadPushHook() {
    return podPayloadPushHook().load(std::memory_order_relaxed);
}
} // namespace testing

// Type-erased base so Node can hold any POD payload.
struct PodPayloadBase {
    virtual ~PodPayloadBase()                                    = default;
    virtual std::type_info const& type() const noexcept          = 0;
    virtual bool                  matches(std::type_info const& ti) const noexcept = 0;
    virtual std::size_t           size() const noexcept          = 0;
    virtual std::size_t           elementSize() const noexcept   = 0;
    virtual InputMetadata const&  podMetadata() const noexcept   = 0;
    virtual bool                  pushValue(void const* value)   = 0;
    virtual std::optional<Error>  readTo(void* out) const        = 0;
    virtual std::optional<Error>  takeTo(void* out)              = 0;
    virtual std::optional<Error>  withSpanRaw(std::function<void(void const*, std::size_t)> const& fn) const = 0;
    virtual std::optional<Error>  withSpanMutableRaw(std::function<void(void*, std::size_t)> const& fn)      = 0;
    virtual bool                  freezeForUpgrade()             = 0;
};

template <typename T>
class PodPayload final : public PodPayloadBase {
public:
    PodPayload()
        : buffer_(std::make_shared<Buffer>(kInitialCapacity)) {}

    std::type_info const& type() const noexcept override { return typeid(T); }
    bool matches(std::type_info const& ti) const noexcept override { return ti == typeid(T); }
    std::size_t elementSize() const noexcept override { return sizeof(T); }
    std::size_t size() const noexcept override {
        auto head = head_.load(std::memory_order_acquire);
        auto tail = publishedTail_.load(std::memory_order_acquire);
        return tail >= head ? tail - head : 0;
    }

    static std::shared_ptr<PodPayloadBase> CreateShared() {
        return std::make_shared<PodPayload<T>>();
    }

    static InputMetadata const& StaticMetadata() {
        static InputMetadata meta{InputMetadataT<T>{}};
        if (meta.createPodPayload == nullptr) {
            meta.createPodPayload = &PodPayload<T>::CreateShared;
        }
        return meta;
    }

    // Push value; returns true on success.
    bool push(T const& value) {
        if (frozen_.load(std::memory_order_acquire)) {
            return false;
        }
        for (;;) {
            auto buf  = std::atomic_load_explicit(&buffer_, std::memory_order_acquire);
            auto tail = tail_.load(std::memory_order_acquire);
            if (tail < buf->capacity) {
                if (tail_.compare_exchange_weak(tail, tail + 1, std::memory_order_acq_rel)) {
                    if (auto hook = testing::GetPodPayloadPushHook()) {
                        hook();
                    }
                    buf->data[tail] = value;
                    publishTail(tail + 1);
                    return true;
                }
                continue;
            }
            ensureCapacity(tail + 1);
        }
    }

    // Peek front without popping.
    std::optional<Error> read(T* out) const {
        for (;;) {
            auto head = head_.load(std::memory_order_acquire);
            auto tail = publishedTail_.load(std::memory_order_acquire);
            if (head >= tail) {
                return Error{Error::Code::NoObjectFound, "No data available"};
            }
            auto buf = std::atomic_load_explicit(&buffer_, std::memory_order_acquire);
            if (head < buf->capacity) {
                *out = buf->data[head];
                return std::nullopt;
            }
            // Should not happen; try to help forward progress.
            return Error{Error::Code::UnknownError, "PodPayload buffer bounds error"};
        }
    }

    // Pop front.
    std::optional<Error> take(T* out) {
        for (;;) {
            auto head = head_.load(std::memory_order_acquire);
            auto tail = publishedTail_.load(std::memory_order_acquire);
            if (head >= tail) {
                return Error{Error::Code::NoObjectFound, "No data available"};
            }
            if (!head_.compare_exchange_weak(head, head + 1, std::memory_order_acq_rel)) {
                continue;
            }
            auto buf = std::atomic_load_explicit(&buffer_, std::memory_order_acquire);
            if (head < buf->capacity) {
                *out = buf->data[head];
                return std::nullopt;
            }
            return Error{Error::Code::UnknownError, "PodPayload buffer bounds error"};
        }
    }

    // Invoke callback with a snapshot span of current queue (head..tail).
    template <typename Fn>
    std::optional<Error> withSpan(Fn&& fn) const {
        auto buf  = std::atomic_load_explicit(&buffer_, std::memory_order_acquire);
        auto head = head_.load(std::memory_order_acquire);
        auto tail = publishedTail_.load(std::memory_order_acquire);
        if (head > tail) {
            return Error{Error::Code::UnknownError, "PodPayload corrupted indices"};
        }
        std::span<const T> sp;
        if (tail > head) {
            sp = std::span<const T>(buf->data.get() + head, tail - head);
        }
        fn(sp);
        return std::nullopt;
    }

    // Invoke callback with a mutable view of the current queue (head..tail). Caller may edit in place.
    template <typename Fn>
    std::optional<Error> withSpanMutable(Fn&& fn) {
        auto buf  = std::atomic_load_explicit(&buffer_, std::memory_order_acquire);
        auto head = head_.load(std::memory_order_acquire);
        auto tail = publishedTail_.load(std::memory_order_acquire);
        if (head > tail) {
            return Error{Error::Code::UnknownError, "PodPayload corrupted indices"};
        }
        std::span<T> sp;
        if (tail > head) {
            sp = std::span<T>(buf->data.get() + head, tail - head);
        }
        fn(sp);
        return std::nullopt;
    }

    bool pushValue(void const* value) override {
        auto typed = static_cast<T const*>(value);
        if (!typed) return false;
        return push(*typed);
    }

    std::optional<Error> readTo(void* out) const override {
        return read(static_cast<T*>(out));
    }

    std::optional<Error> takeTo(void* out) override {
        return take(static_cast<T*>(out));
    }

    std::optional<Error> withSpanRaw(std::function<void(void const*, std::size_t)> const& fn) const override {
        return withSpan([&](std::span<const T> sp) { fn(static_cast<void const*>(sp.data()), sp.size()); });
    }

    std::optional<Error> withSpanMutableRaw(std::function<void(void*, std::size_t)> const& fn) override {
        return withSpanMutable([&](std::span<T> sp) { fn(static_cast<void*>(sp.data()), sp.size()); });
    }

    InputMetadata const& podMetadata() const noexcept override { return StaticMetadata(); }

    bool freezeForUpgrade() override { return freezeForUpgradeImpl(); }

    // Prevent further pushes when migrating to NodeData.
    bool freezeForUpgradeImpl() {
        bool expected = false;
        if (!frozen_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false;
        }
        waitForPublish();
        return true;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return head_.load(std::memory_order_acquire) >= publishedTail_.load(std::memory_order_acquire);
    }

private:
    struct Buffer {
        explicit Buffer(std::size_t cap)
            : capacity(cap), data(std::make_unique<T[]>(cap)) {}
        std::size_t               capacity;
        std::unique_ptr<T[]>      data;
    };

    static constexpr std::size_t kInitialCapacity = 1024;

    void publishTail(std::size_t next) {
        auto expected = next - 1;
        while (!publishedTail_.compare_exchange_weak(expected, next, std::memory_order_release, std::memory_order_relaxed)) {
            expected = next - 1;
        }
    }

    void waitForPublish() const {
        while (publishedTail_.load(std::memory_order_acquire) < tail_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void ensureCapacity(std::size_t needed) {
        std::lock_guard<std::mutex> lg(resizeMutex_);
        auto buf = std::atomic_load_explicit(&buffer_, std::memory_order_acquire);
        if (buf->capacity > needed) {
            return;
        }
        std::size_t newCap = buf->capacity * 2;
        if (newCap <= needed)
            newCap = needed + 1;
        auto newBuf = std::make_shared<Buffer>(newCap);
        auto tail   = tail_.load(std::memory_order_acquire);
        auto copyUpTo = std::min(tail, buf->capacity);
        for (std::size_t i = 0; i < copyUpTo; ++i) {
            newBuf->data[i] = buf->data[i];
        }
        std::atomic_store_explicit(&buffer_, newBuf, std::memory_order_release);
    }

    mutable std::shared_ptr<Buffer> buffer_;
    std::atomic<std::size_t>                     head_{0};
    std::atomic<std::size_t>                     tail_{0};
    std::atomic<std::size_t>                     publishedTail_{0};
    mutable std::mutex                           resizeMutex_;
    std::atomic<bool>                            frozen_{false};
};

} // namespace SP
