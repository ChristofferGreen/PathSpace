#pragma once
#include "Path.hpp"
#include "ConcretePathIterator.hpp"
#include "core/Error.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace SP {

template<typename T>
struct ConcretePath : public Path<T> {
    [[nodiscard]] auto begin() const -> ConcretePathIterator<T>;
    [[nodiscard]] auto end() const -> ConcretePathIterator<T>;

    ConcretePath() = default;
    ConcretePath(T const &t);
    ConcretePath(char const * const t);
    auto operator==(char const * const      other) const -> bool;
    auto operator==(std::string_view const &other) const -> bool;
    auto operator==(ConcretePath<T> const  &other) const -> bool;

    [[nodiscard]] auto canonicalized() const -> Expected<ConcretePath<std::string>>;
    [[nodiscard]] auto components() const -> Expected<std::vector<std::string>>;
    [[nodiscard]] auto isPrefixOf(ConcretePath<std::string_view> other) const -> Expected<bool>;
};
using ConcretePathString     = ConcretePath<std::string>;
using ConcretePathStringView = ConcretePath<std::string_view>;

} // namespace SP
