#pragma once
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include <cereal/archives/binary.hpp>
#include <cereal/types/polymorphic.hpp>
#pragma GCC diagnostic pop
#include <streambuf>
#include <queue>
#include <cstdint>
#include <iostream>

namespace SP {

struct QueueStreamBuffer : public std::streambuf {
    QueueStreamBuffer(std::queue<std::byte>& q) : queue(q) {}

protected:
    virtual int_type overflow(int_type c) override {
        if (c != EOF)
            this->queue.push(std::byte(static_cast<char>(c)));
        return c;
    }

    virtual int_type underflow() override {
        if (this->queue.empty()) {
            return traits_type::eof();
        }

        char c = static_cast<char>(this->queue.front());
        queue.pop();

        // Set buffer pointers
        setg(&c, &c, &c + 1);
        return traits_type::to_int_type(c);
    }
private:
    std::queue<std::byte> &queue;
};

} // namespace SP