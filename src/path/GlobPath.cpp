#include "pathspace/path/GlobPath.hpp"

namespace SP {
template struct GlobPath<std::string>;
template auto   GlobPathString::operator==(ConcretePathString     const &) const -> bool;
template auto   GlobPathString::operator==(ConcretePathStringView const &) const -> bool;
template auto   GlobPathString::operator==(GlobPathString     const &) const -> bool;
template auto   GlobPathString::operator==(GlobPathStringView const &) const -> bool;

template struct GlobPath<std::string_view>;
template auto   GlobPathStringView::operator==(ConcretePathString     const &) const -> bool;
template auto   GlobPathStringView::operator==(ConcretePathStringView const &) const -> bool;
template auto   GlobPathStringView::operator==(GlobPathString     const &) const -> bool;
template auto   GlobPathStringView::operator==(GlobPathStringView const &) const -> bool;


template<typename T>
auto GlobPath<T>::begin() const -> GlobPathIterator<T> {
    return {this->path.begin(), this->path.end()};
}

template<typename T>
auto GlobPath<T>::end() const -> GlobPathIterator<T> {
    return {this->path.end(), this->path.end()};
}

template<typename T>
GlobPath<T>::GlobPath(T const &t) : path(t) {}

template<typename T>
GlobPath<T>::GlobPath(char const *path) : path(path) {}

template<typename T>
auto GlobPath<T>::operator<=>(GlobPath<T> const &other) const -> std::strong_ordering {
    return this->path<=>other.path;
}

template<typename T> template<typename U>
auto GlobPath<T>::operator==(GlobPath<U> const &other) const -> bool {
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

template<typename T> template<typename U>
auto GlobPath<T>::operator==(ConcretePath<U> const &other) const -> bool {
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
auto GlobPath<T>::operator==(char const * const other) const -> bool {
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
auto GlobPath<T>::isValid() const -> bool {
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