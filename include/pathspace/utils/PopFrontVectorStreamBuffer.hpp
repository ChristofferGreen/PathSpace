#pragma once
#include <streambuf>
#include <vector>
#include <cassert>
#include "PopFrontVector.hpp"

namespace SP {

class PopFrontVectorStreamBuffer : public std::streambuf {
public:
    explicit PopFrontVectorStreamBuffer(PopFrontVector<std::byte>& buffer)
        : buffer(buffer) {
        setg(this->inputBuffer, this->inputBuffer, this->inputBuffer); // Initialize get area
        setp(this->outputBuffer, this->outputBuffer + BufferSize - 1); // Initialize put area
    }

protected:
    // Overflow is called when the output buffer is full and needs to be flushed.
    int_type overflow(int_type ch) override {
        if (ch != traits_type::eof()) {
            *pptr() = static_cast<char>(ch);  // Add character to buffer
            pbump(1);  // Increment the put pointer
        }

        // Flush the buffer
        for (char* it = pbase(); it != pptr(); ++it) {
            this->buffer.push_back(static_cast<std::byte>(*it));
        }

        setp(this->outputBuffer, this->outputBuffer + BufferSize - 1); // Reset put area
        return ch != traits_type::eof() ? ch : traits_type::not_eof(ch);
    }

    // Sync is called to flush the buffer.
    int sync() override {
        overflow(traits_type::eof());
        return 0; // Return 0 on success
    }

    // Underflow is called when more data is requested than is available in the get area.
    int_type underflow() override {
        if (gptr() < egptr()) { // buffer not exhausted
            return traits_type::to_int_type(*gptr());
        }

        if (this->buffer.isEmpty()) {
            return traits_type::eof();
        }

        size_t n = std::min(BufferSize, this->buffer.size());
        for (size_t i = 0; i < n; ++i) {
            this->inputBuffer[i] = static_cast<char>(this->buffer[0]);
            this->buffer.pop_front();
        }

        setg(this->inputBuffer, this->inputBuffer, this->inputBuffer + n);
        return traits_type::to_int_type(*gptr());
    }

protected:
    pos_type seekoff(off_type off, std::ios_base::seekdir dir, 
                     std::ios_base::openmode which = std::ios_base::in) override {
        if (which & std::ios_base::in) {
            if (dir == std::ios::cur) {
                // Forward seek within the read buffer
                while (off-- > 0 && underflow() != traits_type::eof()) { }
            } else if (dir == std::ios::beg && off >= 0) {
                // Reset and seek from the beginning
                resetBuffer();
                while (off-- > 0 && underflow() != traits_type::eof()) { }
            } else {
                // Unsupported seek direction or position for this buffer
                return pos_type(off_type(-1));
            }
            return pos_type(gptr() - eback());
        }
        return pos_type(off_type(-1)); // Seeking not supported for output
    }

    pos_type seekpos(pos_type pos, 
                     std::ios_base::openmode which = std::ios_base::in) override {
        return seekoff(off_type(pos), std::ios_base::beg, which);
    }

private:
    void resetBuffer() {
        // Logic to reset the buffer to its initial state
        // Depending on how PopFrontVector is implemented, this might be complex
        // For now, let's assume it's a simple clear operation
        setg(this->inputBuffer, this->inputBuffer, this->inputBuffer); // Reset get area
    }

    PopFrontVector<std::byte>& buffer;
    static constexpr size_t BufferSize = 1024;
    char inputBuffer[BufferSize];
    char outputBuffer[BufferSize];
};

} // namespace SP