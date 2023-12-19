#pragma once
#include "Path.hpp"
#include "ConcretePath.hpp"
#include "GlobPathIterator.hpp"

#include <string>
#include <string_view>

namespace SP {

template<typename T>
struct GlobPath : public Path<std::string_view> {
    auto begin() const -> GlobPathIterator<T>;
    auto end() const -> GlobPathIterator<T>;

    GlobPath() = default;
    GlobPath(T const &t);
    GlobPath(char const *path);
    auto operator==(char const * const other) const -> bool;
    auto operator<=>(GlobPath<T> const &other) const -> std::strong_ordering;
    template<typename U>
    auto operator==(ConcretePath<U> const &other) const -> bool;
    template<typename U>
    auto operator==(GlobPath<U> const &other) const -> bool;

    auto isValid() const -> bool;
private:
    T path;
};
using GlobPathString = GlobPath<std::string>;
using GlobPathStringView = GlobPath<std::string_view>;

} // namespace SP
