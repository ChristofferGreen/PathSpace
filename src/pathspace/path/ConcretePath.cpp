#include "ConcretePath.hpp"

namespace SP {
template<typename T>
auto ConcretePath<T>::begin() const -> ConcretePathIterator<T> {
    return {this->path.begin(), this->path.end()};
}

template<typename T>
auto ConcretePath<T>::end() const -> ConcretePathIterator<T> {
    return {this->path.end(), this->path.end()};
}

template<typename T>
ConcretePath<T>::ConcretePath(T const &t) : Path<T>(t) {}

template<typename T>
ConcretePath<T>::ConcretePath(char const * const t) : Path<T>(t) {}

template<typename T>
auto ConcretePath<T>::operator==(std::string_view const &otherView) const -> bool {
    ConcretePathStringView const other{otherView};
    if(!this->isValid() || !other.isValid())
        return false;
    auto iterA = this->begin();
    auto iterB = other.begin();
    bool a = iterA != this->end();
    bool b = iterB != other.end();
    while(iterA != this->end() && iterB != other.end()) {
        if(*iterA != *iterB)
            return false;
        ++iterA;
        ++iterB;
    }
    if(iterA != this->end() || iterB != other.end())
        return false;
    return true;
}

template<typename T>
auto ConcretePath<T>::operator==(ConcretePath<T> const &other) const -> bool {
    return this->operator==(std::string_view{other.path});
}

template<typename T>
auto ConcretePath<T>::operator==(char const * const other) const -> bool {
    return this->operator==(std::string_view{other});
}

template struct ConcretePath<std::string>;
template struct ConcretePath<std::string_view>;
} // namespace SP
