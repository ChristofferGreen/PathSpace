#pragma once
#include <chrono>

namespace SP {

struct TimeToLive {
    std::chrono::steady_clock::time_point expiryTime;

    TimeToLive() = default;
    TimeToLive(std::chrono::steady_clock::duration duration)
        : expiryTime(std::chrono::steady_clock::now() + duration) {}
    
    static TimeToLive Infinite() {
        return TimeToLive(std::chrono::seconds::max());
    }

    bool isExpired() const {
        return std::chrono::steady_clock::now() >= expiryTime;
    }
};

}