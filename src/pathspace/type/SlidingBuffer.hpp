#pragma once
#include "core/Error.hpp"

#include "alpaca/alpaca.h"

#include <cstring>
#include <expected>
#include <vector>

namespace SP {

struct SlidingBuffer {
    [[nodiscard]] auto data() const -> uint8_t const* {
        return this->data_.data() + this->virtualFront_;
    }

    [[nodiscard]] auto size() const -> size_t {
        return this->data_.size() - this->virtualFront_;
    }

    [[nodiscard]] auto rawSize() const -> size_t {
        return this->data_.size();
    }

    [[nodiscard]] auto empty() const -> size_t {
        return this->data_.empty();
    }

    [[nodiscard]] auto virtualFront() const -> size_t {
        return this->virtualFront_;
    }

    [[nodiscard]] auto operator[](size_t index) const -> uint8_t {
        return this->data_[this->virtualFront_ + index];
    }

    [[nodiscard]] auto operator[](size_t index) -> uint8_t& {
        return this->data_[this->virtualFront_ + index];
    }

    [[nodiscard]] auto begin() -> std::vector<uint8_t>::iterator {
        return this->data_.begin() + this->virtualFront_;
    }

    [[nodiscard]] auto end() -> std::vector<uint8_t>::iterator {
        return this->data_.end();
    }

    [[nodiscard]] auto begin() const -> std::vector<uint8_t>::const_iterator {
        return this->data_.begin() + this->virtualFront_;
    }

    [[nodiscard]] auto end() const -> std::vector<uint8_t>::const_iterator {
        return this->data_.end();
    }

    [[nodiscard]] auto rawBegin() -> std::vector<uint8_t>::iterator {
        return this->data_.begin();
    }

    [[nodiscard]] auto rawEnd() -> std::vector<uint8_t>::iterator {
        return this->data_.end();
    }

    [[nodiscard]] auto rawBegin() const -> std::vector<uint8_t>::const_iterator {
        return this->data_.begin();
    }

    [[nodiscard]] auto rawEnd() const -> std::vector<uint8_t>::const_iterator {
        return this->data_.end();
    }

    auto resize(size_t newSize) -> void {
        compact();
        this->data_.resize(newSize);
    }

    auto append(const uint8_t* bytes, size_t count) -> void {
        this->data_.insert(this->data_.end(), bytes, bytes + count);
    }

    auto advance(size_t bytes) -> void {
        this->virtualFront_ += bytes;
        if (this->virtualFront_ > this->data_.size() / 2) {
            compact();
        }
    }

private:
    std::vector<uint8_t> data_;
    size_t virtualFront_ = 0;

    void compact() {
        if (this->virtualFront_ == 0)
            return;

        if (this->virtualFront_ < this->data_.size()) {
            std::memmove(this->data_.data(), this->data_.data() + this->virtualFront_, this->data_.size() - this->virtualFront_);
            this->data_.resize(this->data_.size() - this->virtualFront_);
        } else {
            this->data_.clear();
        }
        this->virtualFront_ = 0;
    }
};

} // namespace SP
