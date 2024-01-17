#include <string>
#include <string_view>

#include "pathspace/path/Path.hpp"

namespace SP {
template<typename T>
Path<T>::Path(T const &path) : path(path) {}

template<typename T>
auto Path<T>::isValid() const -> bool {
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

template<typename T>
auto Path<T>::getPath() const -> T const& {
    return this->path;
}

template struct Path<std::string>;
template struct Path<std::string_view>;
} // namespace SP