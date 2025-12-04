#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/core/Error.hpp>

#include <optional>
#include <string>

namespace SP::ServeHtml {

template <typename T>
auto read_optional_value(SP::PathSpace const& space, std::string const& path)
    -> SP::Expected<std::optional<T>> {
    auto value = space.read<T, std::string>(path);
    if (value) {
        return std::optional<T>{*value};
    }
    auto const& error = value.error();
    if (error.code == SP::Error::Code::NoObjectFound || error.code == SP::Error::Code::NoSuchPath) {
        return std::optional<T>{};
    }
    return std::unexpected(error);
}

template <typename T>
auto clear_queue(SP::PathSpace& space, std::string const& path) -> SP::Expected<void> {
    while (true) {
        auto taken = space.take<T>(path);
        if (taken) {
            continue;
        }
        auto const& error = taken.error();
        if (error.code == SP::Error::Code::NoObjectFound
            || error.code == SP::Error::Code::NoSuchPath) {
            break;
        }
        return std::unexpected(error);
    }
    return {};
}

template <typename T>
auto replace_single_value(SP::PathSpace& space, std::string const& path, T const& value)
    -> SP::Expected<void> {
    if (auto cleared = clear_queue<T>(space, path); !cleared) {
        return cleared;
    }
    auto result = space.insert(path, value);
    if (!result.errors.empty()) {
        return std::unexpected(result.errors.front());
    }
    return {};
}

} // namespace SP::ServeHtml
