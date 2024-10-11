#include "ConcretePathIterator.hpp"

namespace SP {
template <typename T>
ConcretePathIterator<T>::ConcretePathIterator(T::const_iterator const& iter, T::const_iterator const& endIter)
    : begin(iter), current(iter), end(endIter) {
}

template <typename T>
auto ConcretePathIterator<T>::operator++() -> ConcretePathIterator<T>& {
    this->skipSlashes(this->current); // Will only happen at beginning
    this->skipNonSlashes(this->current);
    this->skipSlashes(this->current);
    return *this;
}

template <typename T>
auto ConcretePathIterator<T>::operator==(const ConcretePathIterator<T>& other) const -> bool {
    return this->current == other.current;
}

template <typename T>
auto ConcretePathIterator<T>::operator*() const -> ConcreteNameStringView {
    auto startSub = this->current;
    auto currentSub = this->current;

    this->skipSlashes(startSub);   // Will only happen at beginning
    this->skipSlashes(currentSub); // Will only happen at beginning
    this->skipNonSlashes(currentSub);
    return {startSub, currentSub};
}

template <typename T>
auto ConcretePathIterator<T>::isAtStart() const -> bool {
    return this->begin == this->current;
}

template <typename T>
auto ConcretePathIterator<T>::fullPath() const -> std::string_view {
    return std::string_view(this->begin, this->end);
}

template <typename T>
auto ConcretePathIterator<T>::skipSlashes(SIterator& iter) const -> void {
    while (iter != this->end && *iter == '/')
        ++iter;
}

template <typename T>
auto ConcretePathIterator<T>::skipNonSlashes(SIterator& iter) const -> void {
    while (iter != this->end && *iter != '/')
        ++iter;
}

template struct ConcretePathIterator<std::string>;
template struct ConcretePathIterator<std::string_view>;
} // namespace SP