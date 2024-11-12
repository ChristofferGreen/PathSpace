#pragma once
#include "ConcreteName.hpp"

#include <string>
#include <string_view>

namespace SP {

template <typename T>
struct ConcretePathIterator {
    using iterator_category = std::forward_iterator_tag;
    using value_type        = ConcreteNameStringView;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const ConcreteNameStringView*;
    using reference         = const ConcreteNameStringView&;
    using SIterator         = T::const_iterator;

    ConcretePathIterator(SIterator const& iter, SIterator const& endIter);

    auto operator++() -> ConcretePathIterator&;
    auto operator==(ConcretePathIterator const& other) const -> bool;
    auto operator*() const -> ConcreteNameStringView;

    auto isAtStart() const -> bool;
    auto fullPath() const -> std::string_view;

private:
    auto skipSlashes(SIterator& iter) const -> void;
    auto skipNonSlashes(SIterator& iter) const -> void;

    SIterator begin;
    SIterator current;
    SIterator end;
};
using ConcretePathIteratorString     = ConcretePathIterator<std::string>;
using ConcretePathIteratorStringView = ConcretePathIterator<std::string_view>;

} // namespace SP