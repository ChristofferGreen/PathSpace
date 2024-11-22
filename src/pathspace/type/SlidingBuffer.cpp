#include "SlidingBuffer.hpp"
#include "utils/TaggedLogger.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace SP {

auto SlidingBuffer::data() const noexcept -> uint8_t const* {
    sp_log("Buffer access - front: " + std::to_string(virtualFront_) + ", size: " + std::to_string(data_.size()), "SlidingBuffer");
    return this->data_.data() + this->virtualFront_;
}

auto SlidingBuffer::size() const noexcept -> size_t {
    return this->data_.size() - this->virtualFront_;
}

auto SlidingBuffer::rawSize() const noexcept -> size_t {
    return this->data_.size();
}

auto SlidingBuffer::empty() const noexcept -> bool {
    return this->data_.empty();
}

auto SlidingBuffer::virtualFront() const noexcept -> size_t {
    return this->virtualFront_;
}

auto SlidingBuffer::capacity() const noexcept -> size_t {
    return this->data_.capacity();
}

auto SlidingBuffer::operator[](size_t index) & -> uint8_t& {
    return this->data_[this->virtualFront_ + index];
}

auto SlidingBuffer::operator[](size_t index) const& -> uint8_t const& {
    return this->data_[this->virtualFront_ + index];
}

auto SlidingBuffer::operator[](size_t index) && -> uint8_t&& {
    return std::move(this->data_[this->virtualFront_ + index]);
}

auto SlidingBuffer::at(size_t index) & -> uint8_t& {
    if (index >= this->size()) {
        throw std::out_of_range("Index out of bounds");
    }
    return (*this)[index];
}

auto SlidingBuffer::at(size_t index) const& -> uint8_t const& {
    if (index >= this->size()) {
        throw std::out_of_range("Index out of bounds");
    }
    return (*this)[index];
}

auto SlidingBuffer::calculateGrowth(size_t required) noexcept -> size_t {
    const size_t maximum = (std::numeric_limits<size_t>::max)();
    if (required > maximum / 2) {
        return maximum; // Avoid overflow
    }
    return std::max(required * 2, INITIAL_CAPACITY);
}

auto SlidingBuffer::resize(size_t newSize) -> void {
    this->compact(); // Ensure data starts at 0
    size_t newCapacity = calculateGrowth(newSize);
    data_.reserve(newCapacity);
    this->data_.resize(newSize);
}

auto SlidingBuffer::append(std::span<uint8_t const> bytes) -> void {
    if (!bytes.empty()) {
        size_t oldSize = this->data_.size();
        size_t newSize = oldSize + bytes.size();

        // Calculate new capacity if needed
        if (newSize > this->data_.capacity()) {
            size_t newCapacity = calculateGrowth(newSize);
            this->data_.reserve(newCapacity);
        }

        this->data_.resize(newSize);
        std::memcpy(this->data_.data() + oldSize, bytes.data(), bytes.size());
    }
}

auto SlidingBuffer::append(uint8_t const* bytes, size_t count) -> void {
    this->append(std::span<uint8_t const>(bytes, count));
}

auto SlidingBuffer::advance(size_t bytes) -> void {
    sp_log("Buffer advance - current front: " + std::to_string(virtualFront_) + ", advancing: " + std::to_string(bytes), "SlidingBuffer");
    if (bytes > this->size()) {
        sp_log("WARNING: Attempting to advance beyond buffer size", "SlidingBuffer");
        return;
    }
    this->virtualFront_ += bytes;
    if (this->virtualFront_ > this->data_.size() / 2 && this->data_.size() >= COMPACT_THRESHOLD) {
        sp_log("Compacting buffer", "SlidingBuffer");
        this->compact();
    }
}

auto SlidingBuffer::compact() -> void {
    if (this->virtualFront_ == 0uz) {
        return;
    }

    sp_log("Compacting buffer - before size: " + std::to_string(data_.size()) + ", front: " + std::to_string(virtualFront_), "SlidingBuffer");

    if (this->virtualFront_ < this->data_.size()) {
        size_t remaining = this->data_.size() - this->virtualFront_;
        std::memmove(this->data_.data(), this->data_.data() + this->virtualFront_, remaining);
        this->data_.resize(remaining);
    } else {
        this->data_.clear();
    }
    this->virtualFront_ = 0uz;
    sp_log("After compact - size: " + std::to_string(data_.size()) + ", front: " + std::to_string(virtualFront_), "SlidingBuffer");
}

} // namespace SP