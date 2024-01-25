#pragma once
#define CEREAL_NO_EXCEPTIONS
#include <cereal/archives/binary.hpp>
#include <cereal/types/vector.hpp>
#include <vector>
#include <cstddef>
#include <fstream>
#include <cassert>

class ByteQueue {
public:
    ByteQueue() = default;
    ~ByteQueue() = default;

    ByteQueue(const ByteQueue&) = default;
    ByteQueue& operator=(const ByteQueue&) = default;

    ByteQueue(ByteQueue&&) noexcept = default;
    ByteQueue& operator=(ByteQueue&&) noexcept = default;

    void push_back(std::byte value) {
        this->data_.push_back(value);
    }

    auto front() const -> std::byte {
        return this->data_.front();
    }

    void pop_front() {
        if (!this->data_.empty()) {
            this->frontIndex_++;
            if (this->frontIndex_ > compactThreshold) {
                compact_data();
            }
        }
    }

    std::byte operator[](size_t index) const {
        assert(index + this->frontIndex_ < this->data_.size());
        return this->data_[this->frontIndex_ + index];
    }

    template <class Archive>
    void serialize(Archive& ar, const unsigned int version) {
        if constexpr (Archive::is_loading::value) {
            size_t size;
            ar(this->frontIndex_); // Serialize frontIndex_
            ar(size);
            ensure_capacity(size);
            this->data_.resize(this->frontIndex_ + size); // Resize to make room for incoming data
            ar(cereal::binary_data(this->data_.data() + this->frontIndex_, size * sizeof(std::byte)));
        } else {
            size_t size = this->data_.size() - this->frontIndex_;
            ar(this->frontIndex_); // Serialize frontIndex_
            ar(size);
            ar(cereal::binary_data(this->data_.data() + this->frontIndex_, size * sizeof(std::byte)));
        }
    }


    auto begin() const { return std::next(this->data_.cbegin(), this->frontIndex_); }
    auto end() const { return this->data_.cend(); }

private:
    std::vector<std::byte> data_;
    size_t frontIndex_ = 0;
    static constexpr size_t compactThreshold = 100;
    static constexpr double compactRatio = 0.5;

    void ensure_capacity(size_t additionalSize) {
        if (this->frontIndex_ + additionalSize > this->data_.capacity()) {
            this->data_.reserve(this->frontIndex_ + additionalSize);
        }
        if (this->frontIndex_ > 0 && this->frontIndex_ + additionalSize > this->data_.size()) {
            compact_data();
        }
    }

    void compact_data() {
        if (this->frontIndex_ > 0) {
            // Compact based on a relative threshold
            size_t dataSize = this->data_.size() - this->frontIndex_;
            if (this->frontIndex_ > dataSize * compactRatio) {
                std::move(this->data_.begin() + this->frontIndex_, this->data_.end(), this->data_.begin());
                this->data_.resize(dataSize);
                this->frontIndex_ = 0;
            }
        }
    }
};
