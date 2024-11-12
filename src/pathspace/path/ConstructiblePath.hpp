
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
    auto operator==(std::string_view other) const -> bool;
    auto operator==(const ConstructiblePath& other) const -> bool;
    template <typename T>
    auto operator==(const Path<T>& other) const -> bool {
        return path == other.getPath();
    }
    explicit operator std::string_view() const noexcept;

    auto append(std::string_view str) -> ConstructiblePath&;
    auto getPath() const noexcept -> std::string_view;
    auto isCompleted() const noexcept -> bool;
    auto markComplete() noexcept -> void;
    auto reset() -> void;

private:
    std::string path;
    bool        isComplete = false;
};

inline auto operator==(std::string_view lhs, const ConstructiblePath& rhs) -> bool {
    return lhs == rhs.getPath();
}

template <typename T>
inline auto operator==(const Path<T>& lhs, const ConstructiblePath& rhs) -> bool {
    return lhs.getPath() == rhs.getPath();
}

} // namespace SP