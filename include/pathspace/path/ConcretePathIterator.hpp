#pragma once
#include "ConcreteName.hpp"

#include <string>
#include <string_view>

namespace SP {

template<typename T>
struct ConcretePathIterator {
    ConcretePathIterator(T::const_iterator const &iter, T::const_iterator const &endIter);

    ConcretePathIterator& operator++();
    auto operator==(ConcretePathIterator const &other) const -> bool;
    auto operator*() const -> ConcreteName;
    auto operator->() const -> ConcreteName;
private:
    auto skipSlashes() -> void;
    T::const_iterator current;
    T::const_iterator end;
};

}