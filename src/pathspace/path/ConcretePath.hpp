#pragma once
#include "ConcretePathIterator.hpp"
#include "Path.hpp"

#include <compare>
#include <functional>
#include <string>
#include <string_view>

namespace SP {

template <typename T>
struct ConcretePath : public Path<T> {
    using Path<T>::Path; // Inherit constructors

    auto begin() const -> ConcretePathIterator<T>;
    auto end() const -> ConcretePathIterator<T>;

    ConcretePath() = default;
    ConcretePath(T const& t);
    ConcretePath(char const* const t);

    // Existing comparison operators
    auto operator==(char const* const other) const -> bool;
    auto operator==(std::string_view const& other) const -> bool;
    auto operator==(ConcretePath<T> const& other) const -> bool;

    // New comparison operators
    auto operator<=>(const ConcretePath& other) const -> std::strong_ordering {
        return this->path <=> other.path;
    }

    // Comparison with string_view
    auto operator<=>(std::string_view other) const -> std::strong_ordering {
        return this->path <=> other;
    }

    // New comparison operators for different ConcretePath types
    template <typename U>
    auto operator<=>(const ConcretePath<U>& other) const -> std::strong_ordering {
        return std::string_view(this->path) <=> std::string_view(other.getPath());
    }

    template <typename U>
    auto operator==(const ConcretePath<U>& other) const -> bool {
        return std::string_view(this->path) == std::string_view(other.getPath());
    }

    // Explicit conversion to string_view
    explicit operator std::string_view() const noexcept {
        return this->path;
    }
};

// Non-member comparison operators for symmetry
template <typename T>
auto operator<=>(std::string_view lhs, const ConcretePath<T>& rhs) -> std::strong_ordering {
    return lhs <=> std::string_view(rhs.getPath());
}

template <typename T>
auto operator==(std::string_view lhs, const ConcretePath<T>& rhs) -> bool {
    return lhs == std::string_view(rhs.getPath());
}

// New non-member comparison operators for different ConcretePath types
template <typename T, typename U>
auto operator<=>(const ConcretePath<T>& lhs, const ConcretePath<U>& rhs) -> std::strong_ordering {
    return std::string_view(lhs.getPath()) <=> std::string_view(rhs.getPath());
}

template <typename T, typename U>
auto operator==(const ConcretePath<T>& lhs, const ConcretePath<U>& rhs) -> bool {
    return std::string_view(lhs.getPath()) == std::string_view(rhs.getPath());
}

using ConcretePathString = ConcretePath<std::string>;
using ConcretePathStringView = ConcretePath<std::string_view>;

} // namespace SP

namespace std {
template <typename T>
struct hash<SP::ConcretePath<T>> {
    size_t operator()(const SP::ConcretePath<T>& path) const noexcept {
        return hash<string_view>{}(static_cast<string_view>(path));
    }
};
} // namespace std