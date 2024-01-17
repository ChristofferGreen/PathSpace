#include "pathspace/path/ConcretePathIterator.hpp"

namespace SP {
template<typename T>
ConcretePathIterator<T>::ConcretePathIterator(T::const_iterator const &iter, T::const_iterator const &endIter) : current(iter), end(endIter) {
    this->skipSlashes();
}

template<typename T>
auto ConcretePathIterator<T>::operator++() -> ConcretePathIterator<T>&{
    while (this->current != this->end && *current != '/') {
        ++this->current;
    }
    this->skipSlashes();
    return *this;
}

template<typename T>
auto ConcretePathIterator<T>::operator==(const ConcretePathIterator<T>& other) const -> bool{
    return this->current == other.current;
}

template<typename T>
auto ConcretePathIterator<T>::operator*() const -> ConcreteName {
    auto currentCopy = this->current;
    auto const start = this->current;
    while (currentCopy != this->end && *currentCopy != '/')
        ++currentCopy;
    return {start, currentCopy};
}

template<typename T>
auto ConcretePathIterator<T>::skipSlashes() -> void {
    while (this->current != this->end && *current == '/')
        ++this->current;
}

template struct ConcretePathIterator<std::string>;
template struct ConcretePathIterator<std::string_view>;
} // namespace SP