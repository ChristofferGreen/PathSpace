#pragma once
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace SP {

struct SlidingBuffer {
    // Don't trigger compaction if data size is below 64 bytes (typical CPU cache line size).
    // This avoids the overhead of memmove for small buffers where:
    // 1. The memory savings would be minimal
    // 2. The entire operation likely fits in a single cache line anyway
    // 3. The cost of cache line invalidation + memmove would exceed the benefit
    static constexpr auto COMPACT_THRESHOLD = 64uz;

    // Initial capacity to reduce early reallocations
    static constexpr auto INITIAL_CAPACITY = 128uz;

    // Constructor with pre-allocation. The storage is allocated from `resource`, which must not be
    // null and must outlive the buffer. Copies and moves follow std::pmr::vector semantics: a
    // copy-constructed buffer uses the default resource, a move-constructed one keeps the source's.
    explicit SlidingBuffer(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : storage(resource) {
        storage.reserve(INITIAL_CAPACITY);
    }

    // Allocator-extended copy/move: the new buffer allocates from `resource`.
    SlidingBuffer(SlidingBuffer const& other, std::pmr::memory_resource* resource)
        : storage(other.storage, resource), frontOffset(other.frontOffset) {}
    SlidingBuffer(SlidingBuffer&& other, std::pmr::memory_resource* resource)
        : storage(std::move(other.storage), resource), frontOffset(other.frontOffset) {}

    SlidingBuffer(SlidingBuffer const&)            = default;
    SlidingBuffer(SlidingBuffer&&) noexcept        = default;
    auto operator=(SlidingBuffer const&) -> SlidingBuffer& = default;
    auto operator=(SlidingBuffer&&) noexcept -> SlidingBuffer& = default;
    ~SlidingBuffer()                               = default;

    // Resource the storage allocates from.
    [[nodiscard]] auto memoryResource() const noexcept -> std::pmr::memory_resource* {
        return storage.get_allocator().resource();
    }

    [[nodiscard]] auto data() const noexcept -> uint8_t const*;
    [[nodiscard]] auto size() const noexcept -> size_t;
    [[nodiscard]] auto rawSize() const noexcept -> size_t;
    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto virtualFront() const noexcept -> size_t;
    [[nodiscard]] auto capacity() const noexcept -> size_t;
    [[nodiscard]] auto rawData() const noexcept -> std::span<uint8_t const>;
    [[nodiscard]] auto rawDataMutable() noexcept -> std::span<uint8_t>;

    [[nodiscard]] auto operator[](size_t index) & -> uint8_t&;
    [[nodiscard]] auto operator[](size_t index) const& -> uint8_t const&;
    [[nodiscard]] auto operator[](size_t index) && -> uint8_t;

    // Deducing this for iterators
    [[nodiscard]] auto begin(this auto&& self) {
        return self.storage.begin() + self.frontOffset;
    }

    [[nodiscard]] auto end(this auto&& self) {
        return self.storage.end();
    }

    [[nodiscard]] auto rawBegin(this auto&& self) {
        return self.storage.begin();
    }

    [[nodiscard]] auto rawEnd(this auto&& self) {
        return self.storage.end();
    }

    [[nodiscard]] auto at(size_t index) & -> uint8_t&;
    [[nodiscard]] auto at(size_t index) const& -> uint8_t const&;

    auto resize(size_t newSize) -> void;
    auto append(std::span<uint8_t const> bytes) -> void;
    auto append(uint8_t const* bytes, size_t count) -> void;
    auto advance(size_t bytes) -> void;
    // Replace the raw storage with a copy of `data` (kept in this buffer's resource).
    auto assignRaw(std::span<uint8_t const> data, size_t frontOffsetIn) -> void;

private:
    std::pmr::vector<uint8_t> storage;
    size_t               frontOffset = 0uz;

    // Calculate appropriate growth for capacity
    [[nodiscard]] static auto calculateGrowth(size_t required) noexcept -> size_t;
    auto                      compact() -> void;
};

} // namespace SP
