#pragma once
#include "ConcreteName.hpp"

#include <string>
#include <string_view>

namespace SP {

template<typename T>
struct ConcretePathIterator {
    using iterator_category = std::forward_iterator_tag;
    using value_type = ConcreteName;
    using difference_type = std::ptrdiff_t;
    using pointer = const ConcreteName*;
    using reference = const ConcreteName&;
    using SIterator = T::const_iterator;

    ConcretePathIterator(SIterator const &iter, SIterator const &endIter);

    auto operator++() -> ConcretePathIterator&;
    auto operator==(ConcretePathIterator const &other) const -> bool;
    auto operator*() const -> ConcreteName;
private:
    auto skipSlashes() -> void;
    SIterator current;
    SIterator end;
};
using ConcretePathIteratorString     = ConcretePathIterator<std::string>;
using ConcretePathIteratorStringView = ConcretePathIterator<std::string_view>;

}