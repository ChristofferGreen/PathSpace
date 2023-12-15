#include "pathspace/path/ConcretePath.hpp"

namespace SP {
template struct ConcretePath<std::string>;
template struct ConcretePath<std::string_view>;

template<typename T>
auto ConcretePath<T>::begin() const -> ConcretePathIterator<T> {
    return {this->path.begin(), this->path.end()};
}

template<typename T>
auto ConcretePath<T>::end() const -> ConcretePathIterator<T> {
    return {this->path.end(), this->path.end()};
}

template<typename T>
ConcretePath<T>::ConcretePath(T const &t) : path(t) {}

template<typename T>
auto ConcretePath<T>::operator==(ConcretePath<T> const &other) const -> bool {
    return this->path==other.path;
}

template<typename T>
auto ConcretePath<T>::operator==(char const * const other) const -> bool {
    return this->path==other;
}

template<typename T>
auto ConcretePath<T>::isValid() const -> bool {
    if(this->path.size()==0)
        return false;
    if(this->path[0] != '/')
        return false;
    return true;
}

} // namespace SP