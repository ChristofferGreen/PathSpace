#pragma once
#include "ConcreteName.hpp"

#include <string>
#include <string_view>

namespace SP {

template<typename T>
struct ConcretePathIterator {
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