#include "SlidingBuffer.hpp"
#include <cstring>
#include <stdexcept>

namespace SP {

auto SlidingBuffer::data() const -> uint8_t const* {
    return this->data_.data() + this->virtualFront_;
}

auto SlidingBuffer::size() const -> size_t {
    return this->data_.size() - this->virtualFront_;
}

auto SlidingBuffer::rawSize() const -> size_t {
    return this->data_.size();
}

auto SlidingBuffer::empty() const -> bool {
    return this->data_.empty();
}

auto SlidingBuffer::virtualFront() const -> size_t {
    return this->virtualFront_;
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

auto SlidingBuffer::resize(size_t newSize) -> void {
    this->compact(); // Ensure data starts at 0
    this->data_.resize(newSize);
}

auto SlidingBuffer::append(std::span<uint8_t const> bytes) -> void {
    if (!bytes.empty()) {
        size_t oldSize = this->data_.size();
        this->data_.resize(oldSize + bytes.size());
        std::memcpy(this->data_.data() + oldSize, bytes.data(), bytes.size());
    }
}

auto SlidingBuffer::append(uint8_t const* bytes, size_t count) -> void {
    this->append(std::span<uint8_t const>(bytes, count));
}

auto SlidingBuffer::advance(size_t bytes) -> void {
    this->virtualFront_ += bytes;
    if (this->virtualFront_ > this->data_.size() / 2 && this->data_.size() >= COMPACT_THRESHOLD) {
        this->compact();
    }
}

auto SlidingBuffer::compact() -> void {
    if (this->virtualFront_ == 0uz) {
        return;
    }

    if (this->virtualFront_ < this->data_.size()) {
        size_t remaining = this->data_.size() - this->virtualFront_;
        std::memmove(this->data_.data(), this->data_.data() + this->virtualFront_, remaining);
        this->data_.resize(remaining);
    } else {
        this->data_.clear();
    }
    this->virtualFront_ = 0uz;
}

} // namespace SP