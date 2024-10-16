#include "ConcreteName.hpp"

namespace SP {

template <typename T>
ConcreteName<T>::ConcreteName(char const* const ptr) : name(ptr) {
}

template <typename T>
ConcreteName<T>::ConcreteName(std::string_view const& name) : name(name) {
}

template <typename T>
ConcreteName<T>::ConcreteName(std::string::const_iterator const& iter, std::string::const_iterator const& endIter) : name(iter, endIter) {
}

template <typename T>
ConcreteName<T>::ConcreteName(std::string const& str) : name(str) {
}

template <typename T>
ConcreteName<T>::ConcreteName(std::string_view::const_iterator const& iter, std::string_view::const_iterator const& endIter)
    : name(iter, endIter) {
}

template <typename T>
auto ConcreteName<T>::operator<=>(ConcreteName<T> const& other) const -> std::strong_ordering {
    return this->name <=> other.name;
}

template <typename T>
auto ConcreteName<T>::operator==(ConcreteName<T> const& other) const -> bool {
    return this->name == other.name;
}

template <typename T>
auto ConcreteName<T>::operator==(char const* const other) const -> bool {
    return this->name == other;
}

template <typename T>
auto ConcreteName<T>::getName() const -> std::string_view const {
    return this->name;
}

template struct ConcreteName<std::string>;
template struct ConcreteName<std::string_view>;

} // namespace SP