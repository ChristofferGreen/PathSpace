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
ConcretePath<T>::ConcretePath(std::string_view const& sv) : Path<T>(std::is_same_v<T, std::string> ? T(std::string(sv)) : T(sv)) {}

template <typename T>
ConcretePath<T>::ConcretePath(std::string const& s) : Path<T>(s) {}

template <typename T>
ConcretePath<T>::ConcretePath(char const* const t) : Path<T>(t) {}

template <typename T>
auto ConcretePath<T>::operator<=>(std::string_view other) const -> std::strong_ordering {
    return this->path <=> other;
}

template <typename T>
auto ConcretePath<T>::operator<=>(const ConcretePath<std::string>& other) const -> std::strong_ordering {
    return std::string_view(this->path) <=> std::string_view(other.getPath());
}

template <typename T>
auto ConcretePath<T>::operator<=>(const ConcretePath<std::string_view>& other) const -> std::strong_ordering {
    return std::string_view(this->path) <=> std::string_view(other.getPath());
}

template <typename T>
auto ConcretePath<T>::operator==(char const* const other) const -> bool {
    return this->operator==(std::string_view{other});
}

template <typename T>
auto ConcretePath<T>::operator==(const ConcretePath<std::string_view>& other) const -> bool {
    return this->operator==(std::string_view{other.getPath()});
}

template <typename T>
auto ConcretePath<T>::operator==(const ConcretePath<std::string>& other) const -> bool {
    return this->operator==(std::string_view{other.getPath()});
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
ConcretePath<T>::operator std::string_view() const noexcept {
    return this->path;
}

// Explicit instantiations
template struct ConcretePath<std::string>;
template struct ConcretePath<std::string_view>;

// Explicit instantiations for non-member functions
template auto operator<=>(std::string_view, ConcretePath<std::string> const&) -> std::strong_ordering;
template auto operator==(std::string_view, ConcretePath<std::string> const&) -> bool;
template auto operator<=>(std::string_view, ConcretePath<std::string_view> const&) -> std::strong_ordering;
template auto operator==(std::string_view, ConcretePath<std::string_view> const&) -> bool;

} // namespace SP