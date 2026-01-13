#include "SlidingBuffer.hpp"
#include "log/TaggedLogger.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace SP {

auto SlidingBuffer::data() const noexcept -> uint8_t const* {
    sp_log("Buffer access - front: " + std::to_string(this->frontOffset) + ", size: " + std::to_string(this->storage.size()), "SlidingBuffer");
    return this->storage.data() + this->frontOffset;
}

auto SlidingBuffer::size() const noexcept -> size_t {
    return this->storage.size() - this->frontOffset;
}

auto SlidingBuffer::rawSize() const noexcept -> size_t {
    return this->storage.size();
}

auto SlidingBuffer::empty() const noexcept -> bool {
    return this->storage.empty();
}

auto SlidingBuffer::virtualFront() const noexcept -> size_t {
    return this->frontOffset;
}

auto SlidingBuffer::rawData() const noexcept -> std::span<uint8_t const> {
    return std::span<uint8_t const>{this->storage.data(), this->storage.size()};
}

auto SlidingBuffer::rawDataMutable() noexcept -> std::span<uint8_t> {
    return std::span<uint8_t>{this->storage.data(), this->storage.size()};
}

auto SlidingBuffer::capacity() const noexcept -> size_t {
    return this->storage.capacity();
}

auto SlidingBuffer::operator[](size_t index) & -> uint8_t& {
    return this->storage[this->frontOffset + index];
}

auto SlidingBuffer::operator[](size_t index) const& -> uint8_t const& {
    return this->storage[this->frontOffset + index];
}

auto SlidingBuffer::operator[](size_t index) && -> uint8_t {
    return this->storage[this->frontOffset + index];
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
    this->storage.reserve(newCapacity);
    this->storage.resize(newSize);
}

auto SlidingBuffer::append(std::span<uint8_t const> bytes) -> void {
    if (!bytes.empty()) {
        size_t oldSize = this->storage.size();
        size_t newSize = oldSize + bytes.size();

        // Calculate new capacity if needed
        if (newSize > this->storage.capacity()) {
            size_t newCapacity = calculateGrowth(newSize);
            this->storage.reserve(newCapacity);
        }

        this->storage.resize(newSize);
        std::memcpy(this->storage.data() + oldSize, bytes.data(), bytes.size());
    }
}

auto SlidingBuffer::append(uint8_t const* bytes, size_t count) -> void {
    this->append(std::span<uint8_t const>(bytes, count));
}

auto SlidingBuffer::advance(size_t bytes) -> void {
    sp_log("Buffer advance - current front: " + std::to_string(frontOffset) + ", advancing: " + std::to_string(bytes), "SlidingBuffer");
    if (bytes > this->size()) {
        sp_log("WARNING: Attempting to advance beyond buffer size", "SlidingBuffer");
        return;
    }
    this->frontOffset += bytes;
    if (this->frontOffset > this->storage.size() / 2 && this->storage.size() >= COMPACT_THRESHOLD) {
        sp_log("Compacting buffer", "SlidingBuffer");
        this->compact();
    }
}

auto SlidingBuffer::compact() -> void {
    if (this->frontOffset == 0uz) {
        return;
    }

    sp_log("Compacting buffer - before size: " + std::to_string(storage.size()) + ", front: " + std::to_string(frontOffset), "SlidingBuffer");

    if (this->frontOffset < this->storage.size()) {
        size_t remaining = this->storage.size() - this->frontOffset;
        std::memmove(this->storage.data(), this->storage.data() + this->frontOffset, remaining);
        this->storage.resize(remaining);
    } else {
        this->storage.clear();
    }
    this->frontOffset = 0uz;
    sp_log("After compact - size: " + std::to_string(storage.size()) + ", front: " + std::to_string(frontOffset), "SlidingBuffer");
}

auto SlidingBuffer::assignRaw(std::vector<uint8_t> data, size_t frontOffsetIn) -> void {
    this->storage     = std::move(data);
    this->frontOffset = std::min(frontOffsetIn, this->storage.size());
}

} // namespace SP
