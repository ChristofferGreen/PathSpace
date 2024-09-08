#include "ConcretePath.hpp"

namespace SP {

template <typename T>
auto ConcretePath<T>::begin() const -> ConcretePathIterator<T> {
    return {this->path.begin(), this->path.end()};
}

template <typename T>
auto ConcretePath<T>::end() const -> ConcretePathIterator<T> {
    return {this->path.end(), this->path.end()};
}

template <typename T>
ConcretePath<T>::ConcretePath(T const& t)
    : Path<T>(t) {
}

template <typename T>
ConcretePath<T>::ConcretePath(char const* const t)
    : Path<T>(t) {
}

template <typename T>
auto ConcretePath<T>::operator==(std::string_view const& otherView) const -> bool {
    ConcretePathStringView const other{otherView};
    if (!this->isValid() || !other.isValid())
        return false;
    auto iterA = this->begin();
    auto iterB = other.begin();
    while (iterA != this->end() && iterB != other.end()) {
        if (*iterA != *iterB)
            return false;
        ++iterA;
        ++iterB;
    }
    return iterA == this->end() && iterB == other.end();
}

template <typename T>
auto ConcretePath<T>::operator==(ConcretePath<T> const& other) const -> bool {
    return this->operator==(std::string_view{other.getPath()});
}

template <typename T>
auto ConcretePath<T>::operator==(char const* const other) const -> bool {
    return this->operator==(std::string_view{other});
}

// Explicit instantiations
template struct ConcretePath<std::string>;
template struct ConcretePath<std::string_view>;

// Explicit instantiations for non-member functions
template auto operator<=>(std::string_view, ConcretePath<std::string> const&) -> std::strong_ordering;
template auto operator==(std::string_view, ConcretePath<std::string> const&) -> bool;
template auto operator<=>(std::string_view, ConcretePath<std::string_view> const&) -> std::strong_ordering;
template auto operator==(std::string_view, ConcretePath<std::string_view> const&) -> bool;

// New explicit instantiations for mixed ConcretePath comparisons
template auto ConcretePath<std::string>::operator<=>(const ConcretePath<std::string_view>&) const -> std::strong_ordering;
template auto ConcretePath<std::string>::operator==(const ConcretePath<std::string_view>&) const -> bool;
template auto ConcretePath<std::string_view>::operator<=>(const ConcretePath<std::string>&) const -> std::strong_ordering;
template auto ConcretePath<std::string_view>::operator==(const ConcretePath<std::string>&) const -> bool;

template auto operator<=>(const ConcretePath<std::string>&, const ConcretePath<std::string_view>&) -> std::strong_ordering;
template auto operator==(const ConcretePath<std::string>&, const ConcretePath<std::string_view>&) -> bool;
template auto operator<=>(const ConcretePath<std::string_view>&, const ConcretePath<std::string>&) -> std::strong_ordering;
template auto operator==(const ConcretePath<std::string_view>&, const ConcretePath<std::string>&) -> bool;

} // namespace SP