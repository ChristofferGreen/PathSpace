#include "pathspace/core/BasePath.hpp"

namespace SP {
template struct BasePath<std::string>;
template struct BasePath<std::string_view>;

template<typename T>
BasePath<T>::Iterator::Iterator(T::const_iterator iter, T::const_iterator endIter)
    : current(iter), end(endIter) {}

template<typename T>
BasePath<T>::Iterator& BasePath<T>::Iterator::operator++() {
    while (this->current != this->end && *current != '/') {
        ++this->current;
    }
    this->skipSlashes();
    return *this;
}

template<typename T>
auto BasePath<T>::Iterator::skipSlashes() -> void {
    while (this->current != this->end && *current == '/')
        ++this->current;
}

template<typename T>
auto BasePath<T>::Iterator::operator==(const Iterator& other) const -> bool{
    return this->current == other.current;
}

template<typename T>
auto BasePath<T>::Iterator::isAtEnd() const -> bool {
    return this->current==this->end;
}

template<typename T>
auto BasePath<T>::begin() const -> BasePath<T>::Iterator {
    auto start = this->path.begin();
    ++start; // Skip initial '/'
    return Iterator(start, this->path.end());
}

template<typename T>
auto BasePath<T>::end() const -> BasePath<T>::Iterator {
    return Iterator(this->path.end(), this->path.end());
}

template<typename T>
auto BasePath<T>::Iterator::operator*() const -> T {
    auto currentCopy = current;
    auto const start = currentCopy;
    while (currentCopy != end && *currentCopy != '/')
        ++currentCopy;
    return T(start, currentCopy);
}

template<typename T>
auto BasePath<T>::Iterator::operator->() const -> T {
    return **this;
}

template<typename T>
BasePath<T>::BasePath(char const * const &str) : path(str) {}

template<typename T>
BasePath<T>::BasePath(T const &t) : path(t) {}

template<typename T>
auto BasePath<T>::operator==(BasePath<T> const &other) const -> bool {
    return this->path==other.path;
}

template<typename T>
auto BasePath<T>::operator<(BasePath<T> const &other) const -> bool {
    return this->path<other.path;
}

template<typename T>
auto BasePath<T>::operator==(char const * const other) const -> bool {
    return this->path==other;
}

template<typename T>
auto BasePath<T>::isValidPath() const -> bool {
    if(this->path.size()==0)
        return false;
    if(this->path[0] != '/')
        return false;
    return true;
}

}