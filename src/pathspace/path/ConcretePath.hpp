#pragma once
#include "ConcretePathIterator.hpp"
#include "Path.hpp"

#include <string>
#include <string_view>

namespace SP {

template <typename T>
struct ConcretePath : public Path<T> {
    using Path<T>::Path;

    auto begin() const -> ConcretePathIterator<T>;
    auto end() const -> ConcretePathIterator<T>;

    ConcretePath() = default;
    ConcretePath(std::string_view const& sv);
    ConcretePath(std::string const& s);
    ConcretePath(char const* const t);

    auto operator<=>(std::string_view other) const -> std::strong_ordering;
    auto operator<=>(const ConcretePath<std::string>& other) const -> std::strong_ordering;
    auto operator<=>(const ConcretePath<std::string_view>& other) const -> std::strong_ordering;
    auto operator==(char const* const other) const -> bool;
    auto operator==(std::string_view const& other) const -> bool;
    auto operator==(const ConcretePath<std::string_view>& other) const -> bool;
    auto operator==(const ConcretePath<std::string>& other) const -> bool;

    // Explicit conversion to string_view
    explicit operator std::string_view() const noexcept;
};

template <typename T>
auto operator<=>(std::string_view lhs, const ConcretePath<T>& rhs) -> std::strong_ordering {
    return lhs <=> std::string_view(rhs.getPath());
}

template <typename T>
auto operator==(std::string_view lhs, const ConcretePath<T>& rhs) -> bool {
    return lhs == std::string_view(rhs.getPath());
}

using ConcretePathString     = ConcretePath<std::string>;
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