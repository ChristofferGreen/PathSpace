#pragma once
#include "Path.hpp"
#include "ConcretePathIterator.hpp"

#include <string>
#include <string_view>

namespace SP {

template<typename T>
struct ConcretePath : public Path<T> {
    auto begin() const -> ConcretePathIterator<T>;
    auto end()   const -> ConcretePathIterator<T>;

    ConcretePath() = default;
    ConcretePath(T const &t);
    ConcretePath(char const * const t);
    auto operator==(char const * const      other) const -> bool;
    auto operator==(std::string_view const &other) const -> bool;
    auto operator==(ConcretePath<T> const  &other) const -> bool;
};
using ConcretePathString     = ConcretePath<std::string>;
using ConcretePathStringView = ConcretePath<std::string_view>;

} // namespace SP