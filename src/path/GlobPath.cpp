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
    return this->operator==(ConcretePathStringView{otherView});
}

template<typename T> template<typename U>
auto GlobPath<T>::operator==(GlobPath<U> const &other) const -> bool {
    return ConcretePathStringView{this->path} == ConcretePathStringView{other.getPath()};
}

template<typename T> template<typename U>
auto GlobPath<T>::operator==(ConcretePath<U> const &other) const -> bool {
    if(!this->isValid() || !other.isValid())
        return false;
    auto iterA = this->begin();
    auto iterB = other.begin();
    while(iterA != this->end() && iterB != other.end()) {
        auto const match = (*iterA).match(*iterB);
        if(std::get<1>(match)) // Supermatch (**)
            return true;
        if(!std::get<0>(match))
            return false;
        ++iterA;
        ++iterB;
    }
    if(iterA != this->end() || iterB != other.end())
        return false;
    return true;
}

template<typename T>
auto GlobPath<T>::isConcrete() const -> bool {
    return !this->isGlob();
}

template<typename T>
auto GlobPath<T>::isGlob() const -> bool {
    return is_glob(this->path);
}


} // namespace SP