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

    // Reject relative components ("." and ".."). Names that merely start with a dot ("/.hidden")
    // are valid, matching validate_path_impl.
    std::string_view const view{this->path};
    std::size_t            pos = 1;
    while (pos <= view.size()) {
        auto const next      = view.find('/', pos);
        auto const end       = next == std::string_view::npos ? view.size() : next;
        auto const component = view.substr(pos, end - pos);
        if (component == "." || component == "..")
            return false;
        if (next == std::string_view::npos)
            break;
        pos = next + 1;
    }
    return true;
}

template<typename T>
auto Path<T>::getPath() const -> T const& {
    return this->path;
}

template struct Path<std::string>;
template struct Path<std::string_view>;
} // namespace SP
