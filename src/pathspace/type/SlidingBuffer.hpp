#pragma once
#include <cstdint>
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

    // Constructor with pre-allocation
    SlidingBuffer() {
        data_.reserve(INITIAL_CAPACITY);
    }

    [[nodiscard]] auto data() const noexcept -> uint8_t const*;
    [[nodiscard]] auto size() const noexcept -> size_t;
    [[nodiscard]] auto rawSize() const noexcept -> size_t;
    [[nodiscard]] auto empty() const noexcept -> bool;
    [[nodiscard]] auto virtualFront() const noexcept -> size_t;
    [[nodiscard]] auto capacity() const noexcept -> size_t;

    [[nodiscard]] auto operator[](size_t index) & -> uint8_t&;
    [[nodiscard]] auto operator[](size_t index) const& -> uint8_t const&;
    [[nodiscard]] auto operator[](size_t index) && -> uint8_t&&;

    // Deducing this for iterators
    [[nodiscard]] auto begin(this auto&& self) {
        return self.data_.begin() + self.virtualFront_;
    }

    [[nodiscard]] auto end(this auto&& self) {
        return self.data_.end();
    }

    [[nodiscard]] auto rawBegin(this auto&& self) {
        return self.data_.begin();
    }

    [[nodiscard]] auto rawEnd(this auto&& self) {
        return self.data_.end();
    }

    [[nodiscard]] auto at(size_t index) & -> uint8_t&;
    [[nodiscard]] auto at(size_t index) const& -> uint8_t const&;

    auto resize(size_t newSize) -> void;
    auto append(std::span<uint8_t const> bytes) -> void;
    auto append(uint8_t const* bytes, size_t count) -> void;
    auto advance(size_t bytes) -> void;

private:
    std::vector<uint8_t> data_;
    size_t               virtualFront_ = 0uz;

    // Calculate appropriate growth for capacity
    [[nodiscard]] static auto calculateGrowth(size_t required) noexcept -> size_t;
    auto                      compact() -> void;
};

} // namespace SP