#pragma once

#include <atomic>
#include <cstddef>
#include <memory_resource>

namespace SP::testing {

// Memory resource for allocator tests: forwards to an upstream resource (thread-safe by default)
// and counts allocations, deallocations and live bytes with atomics, so assertions are exact and
// independent of timing.
class CountingMemoryResource final : public std::pmr::memory_resource {
public:
    explicit CountingMemoryResource(std::pmr::memory_resource* upstreamIn = std::pmr::new_delete_resource())
        : upstream(upstreamIn) {}

    [[nodiscard]] auto allocations() const noexcept -> std::size_t {
        return this->allocationCount.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto deallocations() const noexcept -> std::size_t {
        return this->deallocationCount.load(std::memory_order_relaxed);
    }
    [[nodiscard]] auto liveAllocations() const noexcept -> std::size_t {
        return this->allocations() - this->deallocations();
    }
    [[nodiscard]] auto liveBytes() const noexcept -> std::size_t {
        return this->liveByteCount.load(std::memory_order_relaxed);
    }

private:
    auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
        void* ptr = this->upstream->allocate(bytes, alignment);
        this->allocationCount.fetch_add(1, std::memory_order_relaxed);
        this->liveByteCount.fetch_add(bytes, std::memory_order_relaxed);
        return ptr;
    }
    auto do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) -> void override {
        this->upstream->deallocate(ptr, bytes, alignment);
        this->deallocationCount.fetch_add(1, std::memory_order_relaxed);
        this->liveByteCount.fetch_sub(bytes, std::memory_order_relaxed);
    }
    auto do_is_equal(std::pmr::memory_resource const& other) const noexcept -> bool override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream;
    std::atomic<std::size_t>   allocationCount{0};
    std::atomic<std::size_t>   deallocationCount{0};
    std::atomic<std::size_t>   liveByteCount{0};
};

} // namespace SP::testing
