#include "pathspace/path/GlobPath.hpp"

namespace SP {
template struct GlobPath<std::string>;
template auto   GlobPathString::operator==(ConcretePathString     const &) const -> bool;
template auto   GlobPathString::operator==(ConcretePathStringView const &) const -> bool;
template auto   GlobPathString::operator==(GlobPathString         const &) const -> bool;
template auto   GlobPathString::operator==(GlobPathStringView     const &) const -> bool;

template struct GlobPath<std::string_view>;
template auto   GlobPathStringView::operator==(ConcretePathString     const &) const -> bool;
template auto   GlobPathStringView::operator==(ConcretePathStringView const &) const -> bool;
template auto   GlobPathStringView::operator==(GlobPathString         const &) const -> bool;
template auto   GlobPathStringView::operator==(GlobPathStringView     const &) const -> bool;


template<typename T>
auto GlobPath<T>::begin() const -> GlobPathIterator<T> {
    return {this->path.begin(), this->path.end()};
}

template<typename T>
auto GlobPath<T>::end() const -> GlobPathIterator<T> {
    return {this->path.end(), this->path.end()};
}

template<typename T>
GlobPath<T>::GlobPath(T const &t) : Path<T>(t) {}

template<typename T>
GlobPath<T>::GlobPath(char const *path) : Path<T>(path) {}

template<typename T>
auto GlobPath<T>::operator<=>(GlobPath<T> const &other) const -> std::strong_ordering {
    return this->path<=>other.path;
}

template<typename T>
auto GlobPath<T>::operator==(std::string_view const &otherView) const -> bool {
    ConcretePathStringView const other{otherView};
    if(!this->isValid() || !other.isValid())
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

template<typename T> template<typename U>
auto GlobPath<T>::operator==(GlobPath<U> const &other) const -> bool {
    if(!this->isValid() || !other.isValid())
        return false;
    return this->path==other.getPath();
}

template<typename T> template<typename U>
auto GlobPath<T>::operator==(ConcretePath<U> const &other) const -> bool {
    return this->operator==(other.getPath());
}

template<typename T>
auto GlobPath<T>::operator==(char const * const other) const -> bool {
    return this->operator==(std::string_view{other});
}

} // namespace SP