#pragma once

#include "path/Path.hpp"
#include <compare>
#include <concepts>
#include <string>
#include <string_view>

namespace SP {

class ConstructiblePath {
public:
    ConstructiblePath() noexcept : path("/") {
    }

    template <std::convertible_to<std::string_view> T>
    explicit ConstructiblePath(T&& str) : path(std::forward<T>(str)), isComplete(true) {
    }

    template <typename T>
    explicit ConstructiblePath(const Path<T>& p) : path(p.getPath()), isComplete(true) {
    }

    // Rule of zero
    ConstructiblePath(const ConstructiblePath&) = default;
    ConstructiblePath(ConstructiblePath&&) noexcept = default;
    ConstructiblePath& operator=(const ConstructiblePath&) = default;
    ConstructiblePath& operator=(ConstructiblePath&&) noexcept = default;
    ~ConstructiblePath() = default;

    // Three-way comparison
    auto operator<=>(const ConstructiblePath&) const = default;

    // Equality comparisons
    bool operator==(std::string_view other) const {
        return path == other;
    }

    bool operator==(const ConstructiblePath& other) const {
        return path == other.path;
    }

    template <typename T>
    bool operator==(const Path<T>& other) const {
        return path == other.getPath();
    }

    ConstructiblePath& append(std::string_view str) {
        if (!isComplete) {
            if (!str.empty()) {
                if (path.back() == '/' && str.front() == '/') {
                    // If both path and str have a slash, skip one
                    path.append(str.substr(1));
                } else if (path.back() != '/' && str.front() != '/') {
                    // If neither has a slash, add one
                    path += '/';
                    path.append(str);
                } else {
                    // In all other cases, just append
                    path.append(str);
                }
            }
        }
        return *this;
    }

    // Getters
    std::string_view getPath() const noexcept {
        return path;
    }
    bool isCompleted() const noexcept {
        return isComplete;
    }

    // Utility methods
    void markComplete() noexcept {
        isComplete = true;
    }
    void reset() {
        path = "/";
        isComplete = false;
    }

    // Conversion operator
    explicit operator std::string_view() const noexcept {
        return path;
    }

private:
    std::string path;
    bool isComplete = false;
};

// Non-member comparison functions
inline bool operator==(std::string_view lhs, const ConstructiblePath& rhs) {
    return lhs == rhs.getPath();
}

template <typename T>
inline bool operator==(const Path<T>& lhs, const ConstructiblePath& rhs) {
    return lhs.getPath() == rhs.getPath();
}

} // namespace SP