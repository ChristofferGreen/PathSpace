#include "GlobPathIterator.hpp"

namespace SP {
template<typename T>
GlobPathIterator<T>::GlobPathIterator(T::const_iterator const &iter, T::const_iterator const &endIter) : current(iter), end(endIter) {
    this->skipSlashes();
}

template<typename T>
auto GlobPathIterator<T>::operator++() -> GlobPathIterator<T>& {
    while (this->current != this->end && *current != '/') {
        ++this->current;
    }
    this->skipSlashes();
    return *this;
}

template<typename T>
auto GlobPathIterator<T>::operator++(int) -> GlobPathIterator<T> {
    GlobPathIterator<T> previous = *this;
    this->operator++();
    return previous;
}

template<typename T>
auto GlobPathIterator<T>::operator==(const GlobPathIterator<T>& other) const -> bool{
    return this->current == other.current;
}

template<typename T>
auto GlobPathIterator<T>::operator*() const -> GlobName {
    auto currentCopy = this->current;
    auto const start = this->current;
    while (currentCopy != this->end && *currentCopy != '/')
        ++currentCopy;
    return {start, currentCopy};
}

template<typename T>
auto GlobPathIterator<T>::skipSlashes() -> void {
    while (this->current != this->end && *current == '/')
        ++this->current;
}

template struct GlobPathIterator<std::string>;
template struct GlobPathIterator<std::string_view>;
} // namespace SP
