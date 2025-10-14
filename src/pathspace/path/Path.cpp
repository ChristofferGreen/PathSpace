#include "Path.hpp"

#include <string>
#include <string_view>

namespace SP {
template<typename T>
Path<T>::Path(T const &path) : path(path) {}

template<typename T>
auto Path<T>::isValid() const -> bool {
    if(this->path.size()<1) // Must start with / and have at least one name.
        return false;

    if(this->path[0] != '/')
        return false;

    // Check for relative paths like '.', '..', etc. Names not allowed to start with .
    if (this->path.contains("/."))
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
