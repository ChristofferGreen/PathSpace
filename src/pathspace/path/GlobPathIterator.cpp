#include "GlobPathIterator.hpp"

namespace SP {
template <typename T>
GlobPathIterator<T>::GlobPathIterator(T::const_iterator const& iter, T::const_iterator const& endIter)
    : begin(iter), current(iter), end(endIter) {
}

template <typename T>
auto GlobPathIterator<T>::operator++() -> GlobPathIterator<T>& {
    this->skipSlashes(this->current); // Will only happen at beginning
    this->skipNonSlashes(this->current);
    this->skipSlashes(this->current);
    return *this;
}

template <typename T>
auto GlobPathIterator<T>::operator++(int) -> GlobPathIterator<T> {
    GlobPathIterator<T> current = *this;
    this->operator++();
    return current;
}

template <typename T>
auto GlobPathIterator<T>::operator==(const GlobPathIterator<T>& other) const -> bool {
    return this->current == other.current;
}

template <typename T>
auto GlobPathIterator<T>::operator*() const -> GlobName {
    auto startSub = this->current;
    auto currentSub = this->current;

    this->skipSlashes(startSub);   // Will only happen at beginning
    this->skipSlashes(currentSub); // Will only happen at beginning
    this->skipNonSlashes(currentSub);
    return {startSub, currentSub};
}

template <typename T>
auto GlobPathIterator<T>::isAtStart() const -> bool {
    return this->begin == this->current;
}

template <typename T>
auto GlobPathIterator<T>::fullPath() const -> std::string_view {
    return std::string_view(this->begin, this->end);
}

template <typename T>
auto GlobPathIterator<T>::skipSlashes(SIterator& iter) const -> void {
    while (iter != this->end && *iter == '/')
        ++iter;
}

template <typename T>
auto GlobPathIterator<T>::skipNonSlashes(SIterator& iter) const -> void {
    while (iter != this->end && *iter != '/')
        ++iter;
}

template struct GlobPathIterator<std::string>;
template struct GlobPathIterator<std::string_view>;
} // namespace SP