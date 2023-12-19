#include "pathspace/path/GlobPathIterator.hpp"

namespace SP {
template struct GlobPathIterator<std::string>;
template struct GlobPathIterator<std::string_view>;

template<typename T>
GlobPathIterator<T>::GlobPathIterator(T::const_iterator const &iter, T::const_iterator const &endIter) : current(iter), end(endIter) {
    this->skipSlashes();
}

template<typename T>
auto GlobPathIterator<T>::operator++() -> GlobPathIterator<T>&{
    while (this->current != this->end && *current != '/') {
        ++this->current;
    }
    this->skipSlashes();
    return *this;
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
auto GlobPathIterator<T>::operator->() const -> GlobName {
    return {this->current, this->end};
}

template<typename T>
auto GlobPathIterator<T>::skipSlashes() -> void {
    while (this->current != this->end && *current == '/')
        ++this->current;
}

} // namespace SP