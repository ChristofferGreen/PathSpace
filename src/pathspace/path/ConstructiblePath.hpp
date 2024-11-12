
#pragma once
#include "path/Path.hpp"

#include <concepts>
#include <string>
#include <string_view>

namespace SP {

class ConstructiblePath {
public:
    ConstructiblePath() noexcept;
    template <std::convertible_to<std::string_view> T>
    explicit ConstructiblePath(T&& str) : path(std::forward<T>(str)), isComplete(true) {}
    template <typename T>
    explicit ConstructiblePath(const Path<T>& p) : path(p.getPath()), isComplete(true) {}

    // Rule of zero
    ConstructiblePath(const ConstructiblePath&)                = default;
    ConstructiblePath(ConstructiblePath&&) noexcept            = default;
    ConstructiblePath& operator=(const ConstructiblePath&)     = default;
    ConstructiblePath& operator=(ConstructiblePath&&) noexcept = default;
    ~ConstructiblePath()                                       = default;

    auto operator<=>(const ConstructiblePath&) const = default;
    bool operator==(std::string_view other) const;
    bool operator==(const ConstructiblePath& other) const;
    template <typename T>
    bool operator==(const Path<T>& other) const {
        return path == other.getPath();
    }
    explicit operator std::string_view() const noexcept;

    ConstructiblePath& append(std::string_view str);

    std::string_view getPath() const noexcept;
    bool             isCompleted() const noexcept;
    void             markComplete() noexcept;
    void             reset();

private:
    std::string path;
    bool        isComplete = false;
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