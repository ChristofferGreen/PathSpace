#pragma once

#include <cstddef>
#include <memory_resource>
#include <mutex>

namespace SP {

/**
 * Serializes every call into an upstream memory resource with a mutex.
 *
 * PathSpace allocates from concurrent operations, so the resource it is given has to be
 * thread-safe. Resources such as std::pmr::monotonic_buffer_resource and
 * std::pmr::unsynchronized_pool_resource are not; wrap them in this adapter (or use
 * std::pmr::synchronized_pool_resource) when the space is used from more than one thread:
 *
 *   std::pmr::monotonic_buffer_resource arena{1 << 20};
 *   SP::LockedMemoryResource            shared{&arena};
 *   SP::PathSpace                       space{shared};
 *
 * The adapter does not own the upstream, which must outlive it.
 */
class LockedMemoryResource final : public std::pmr::memory_resource {
public:
    explicit LockedMemoryResource(std::pmr::memory_resource* upstreamIn) noexcept : upstream(upstreamIn) {}

private:
    auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
        std::lock_guard<std::mutex> guard(this->mutex);
        return this->upstream->allocate(bytes, alignment);
    }
    auto do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) -> void override {
        std::lock_guard<std::mutex> guard(this->mutex);
        this->upstream->deallocate(ptr, bytes, alignment);
    }
    auto do_is_equal(std::pmr::memory_resource const& other) const noexcept -> bool override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream;
    std::mutex                 mutex;
};

} // namespace SP
