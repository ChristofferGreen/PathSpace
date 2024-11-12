#pragma once
#include "ConcretePath.hpp"
#include "GlobPathIterator.hpp"
#include "Path.hpp"

#include <string>
#include <string_view>

namespace SP {

template <typename T>
struct GlobPath : public Path<T> {
    auto begin() const -> GlobPathIterator<T>;
    auto end() const -> GlobPathIterator<T>;

    GlobPath() = default;
    GlobPath(std::string_view const& sv);
    GlobPath(std::string const& s);
    GlobPath(char const* path);

    auto operator<=>(GlobPath<T> const& other) const -> std::strong_ordering;
    auto operator==(std::string_view const& other) const -> bool;
    template <typename U>
    auto operator==(ConcretePath<U> const& other) const -> bool;
    template <typename U>
    auto operator==(GlobPath<U> const& other) const -> bool;

    auto isConcrete() const -> bool;
    auto isGlob() const -> bool;
};
using GlobPathString     = GlobPath<std::string>;
using GlobPathStringView = GlobPath<std::string_view>;

} // namespace SP
