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
    if(!this->isValid())
        return false;
    if(!other.isValid())
        return false;
    auto iterA = this->begin();
    auto iterB = other.begin();
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
auto ConcretePath<T>::operator==(char const * const other) const -> bool {
    if(!this->isValid())
        return false;
    std::string_view const b{other};
    auto iterA = this->path.begin();
    auto iterB = b.begin();
    while(iterA != this->path.end() && iterB != b.end()) {
        if(*iterA=='/') {
            while(iterA != this->path.end() && *iterA=='/')
                ++iterA;
        } else {
            ++iterA;
        }
        if(*iterB=='/') {
            while(iterB != b.end() && *iterB=='/')
                ++iterB;
        } else {
            ++iterB;
        }
        if(*iterA != *iterB)
            return false;
    }
    return true;
}

template<typename T>
auto ConcretePath<T>::isValid() const -> bool {
        if(this->path.size()==0)
            return false;

        if(this->path[0] != '/')
            return false;

        // Check for null character
        if(this->path.contains('\0'))
            return false;

        // Check for relative paths like '.', '..', etc.
        if (this->path.contains("/../") || this->path.contains("/./") || this->path.ends_with("/..") || this->path.ends_with("/."))
            return false;

        return true;
}

} // namespace SP