#pragma once
#include "Path.hpp"
#include "ConcretePathIterator.hpp"

#include <string>
#include <string_view>

namespace SP {

template<typename T>
struct ConcretePath : public Path<T> {
    auto begin() const -> ConcretePathIterator<T>;
    auto end() const -> ConcretePathIterator<T>;

    ConcretePath() = default;
    ConcretePath(T const &t);
    auto operator==(char const * const other) const -> bool;
    auto operator==(ConcretePath<T> const &other) const -> bool;

    auto isValid() const -> bool;
protected:
    T path;
};
using ConcretePathString = ConcretePath<std::string>;
using ConcretePathStringView = ConcretePath<std::string_view>;

} // namespace SP