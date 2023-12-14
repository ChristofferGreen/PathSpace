#include "pathspace/core/Path.hpp"

namespace SP {
template struct Path<std::string>;
template struct Path<std::string_view>;

template<typename T>
Path<T>::Path(T const &str) : BasePath<T>(str) {}

template<typename T>
auto Path<T>::operator==(Path const &other) const -> bool {
    return this->path==other.path;
}

template<typename T>
auto Path<T>::operator<(Path const &other) const -> bool {
    return this->path<other.path;
}

template<typename T>
auto Path<T>::operator==(char const * const other) const -> bool {
    return this->path==other;
}

template<typename T>
auto Path<T>::isValidPath() const -> bool {
    if(this->path.size()==0)
        return false;
    if(this->path[0] != '/')
        return false;
    return true;
}

}