#pragma once
#include "core/Error.hpp"

#include "alpaca/alpaca.h"

#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <vector>

namespace SP {

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

struct SlidingBuffer {
    // Don't trigger compaction if data size is below 64 bytes (typical CPU cache line size).
    // This avoids the overhead of memmove for small buffers where:
    // 1. The memory savings would be minimal
    // 2. The entire operation likely fits in a single cache line anyway
    // 3. The cost of cache line invalidation + memmove would exceed the benefit
    static constexpr auto COMPACT_THRESHOLD = 64uz;

    [[nodiscard]] auto data() const -> uint8_t const* {
        return this->data_.data() + this->virtualFront_;
    }

    [[nodiscard]] auto size() const -> size_t {
        return this->data_.size() - this->virtualFront_;
    }

    [[nodiscard]] auto rawSize() const -> size_t {
        return this->data_.size();
    }

    [[nodiscard]] auto empty() const -> bool {
        return this->data_.empty();
    }

    [[nodiscard]] auto virtualFront() const -> size_t {
        return this->virtualFront_;
    }

    [[nodiscard]] auto operator[](size_t index) & -> uint8_t& {
        return this->data_[this->virtualFront_ + index];
    }

    [[nodiscard]] auto operator[](size_t index) const& -> uint8_t const& {
        return this->data_[this->virtualFront_ + index];
    }

    [[nodiscard]] auto operator[](size_t index) && -> uint8_t&& {
        return std::move(this->data_[this->virtualFront_ + index]);
    }

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

    auto resize(size_t newSize) -> void {
        this->compact(); // Always ensure data starts at 0
        this->data_.resize(newSize);
    }

    // Modern span-based append
    auto append(std::span<uint8_t const> bytes) -> void {
        this->data_.insert(this->data_.end(), bytes.begin(), bytes.end());
    }

    // Keep the C-style version for compatibility
    auto append(uint8_t const* bytes, size_t count) -> void {
        this->append(std::span<uint8_t const>(bytes, count));
    }

    auto advance(size_t bytes) -> void {
        this->virtualFront_ += bytes;
        if (this->virtualFront_ > this->data_.size() / 2 && this->data_.size() >= COMPACT_THRESHOLD) {
            this->compact(); // Only trigger compaction for larger buffers
        }
    }

private:
    std::vector<uint8_t> data_;
    size_t virtualFront_ = 0uz;

    auto compact() -> void {
        if (this->virtualFront_ == 0uz) {
            return;
        }

        if (this->virtualFront_ < this->data_.size()) {
            std::memmove(this->data_.data(), this->data_.data() + this->virtualFront_, this->data_.size() - this->virtualFront_);
            this->data_.resize(this->data_.size() - this->virtualFront_);
        } else {
            this->data_.clear();
        }
        this->virtualFront_ = 0uz;
    }
};

} // namespace SP
