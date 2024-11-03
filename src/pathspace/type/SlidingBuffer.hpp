#pragma once
#include "core/Error.hpp"

#include "alpaca/alpaca.h"

#include <cstring>
#include <expected>
#include <vector>

namespace SP {

class SlidingBuffer {
public:
    std::vector<uint8_t> data;
    size_t virtualFront = 0;

    [[nodiscard]] const uint8_t* current_data() const {
        return data.data() + virtualFront;
    }

    [[nodiscard]] size_t remaining_size() const {
        return data.size() - virtualFront;
    }

    void append(const uint8_t* bytes, size_t count) {
        data.insert(data.end(), bytes, bytes + count);
    }

    void advance(size_t bytes) {
        virtualFront += bytes;
        if (virtualFront > data.size() / 2) {
            compact();
        }
    }

private:
    void compact() {
        if (virtualFront == 0)
            return;

        if (virtualFront < data.size()) {
            std::memmove(data.data(), data.data() + virtualFront, data.size() - virtualFront);
            data.resize(data.size() - virtualFront);
        } else {
            data.clear();
        }
        virtualFront = 0;
    }
};

} // namespace SP
