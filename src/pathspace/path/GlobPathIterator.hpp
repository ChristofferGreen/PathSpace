#pragma once
#include "GlobName.hpp"

#include <string>
#include <string_view>

namespace SP {

template <typename T>
struct GlobPathIterator {
    using iterator_category = std::forward_iterator_tag;
    using value_type        = GlobName;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const GlobName*;
    using reference         = const GlobName&;
    using SIterator         = T::const_iterator;

    GlobPathIterator(SIterator const& iter, SIterator const& endIter);

    auto operator++() -> GlobPathIterator<T>&;
    auto operator++(int) -> GlobPathIterator<T>;
    auto operator==(GlobPathIterator const& other) const -> bool;
    auto operator*() const -> GlobName;

    auto isAtStart() const -> bool;
    auto fullPath() const -> std::string_view;

private:
    auto      skipSlashes(SIterator& iter) const -> void;
    auto      skipNonSlashes(SIterator& iter) const -> void;
    SIterator begin;
    SIterator current;
    SIterator end;
};
using GlobPathIteratorString     = GlobPathIterator<std::string>;
using GlobPathIteratorStringView = GlobPathIterator<std::string_view>;

} // namespace SP