#include <string>
#include <string_view>

#include "path/Path.hpp"

namespace SP {
template <typename T>
Path<T>::Path(T const& path)
    : path(path) {}

template <typename T>
auto Path<T>::getPath() const -> T const& {
    return this->path;
}

template <typename T>
auto Path<T>::setPath(T const& path) -> void {
    this->path = path;
}

template <typename T>
auto Path<T>::size() const -> size_t {
    return this->path.size();
}

template <typename T>
auto Path<T>::empty() const -> bool {
    return this->path.empty();
}

template struct Path<std::string>;
template struct Path<std::string_view>;
} // namespace SP