#pragma once

#include "PathSpace.hpp"
#include "core/Error.hpp"

#include <string>

namespace SP::Inspector::Detail {

template <typename T>
inline auto DrainMetricQueue(PathSpace& space, std::string const& path) -> Expected<void> {
    while (true) {
        auto taken = space.take<T, std::string>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == Error::Code::NoObjectFound
            || error.code == Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }
    return {};
}

template <typename T>
inline auto ReplaceMetricValue(PathSpace& space,
                               std::string const& path,
                               T const& value) -> Expected<void> {
    if (auto cleared = DrainMetricQueue<T>(space, path); !cleared) {
        return cleared;
    }
    auto inserted = space.insert(path, value);
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

} // namespace SP::Inspector::Detail

