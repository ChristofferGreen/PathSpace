#pragma once
#include "Capabilities.hpp"
#include "TimeToLive.hpp"

#include <optional>

namespace SP {

struct InsertOptions {
    std::optional<Capabilities> capabilities;
    std::optional<TimeToLive> ttl;
    std::optional<int> maxInsertions;
    bool waitForLocks = false;
};

}