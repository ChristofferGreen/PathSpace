#include <string>
#include <string_view>

#include "path/Path.hpp"

namespace SP {
template <typename T>
Path<T>::Path(T const& path)
    : path(path) {}

template <typename T>
auto Path<T>::validate(ValidationLevel const& level) const noexcept -> std::optional<Error> {
    switch (level) {
        case ValidationLevel::Basic:
            return this->validateBasic();
        case ValidationLevel::Full:
            return this->validateFull();
        default:
            return std::nullopt;
    }
}

template <typename T>
auto Path<T>::validateBasic() const -> std::optional<Error> {
    // Handle empty path
    if (this->path.empty()) {
        return Error{Error::Code::InvalidPath, "Empty path"};
    }

    // Path must start with forward slash
    if (this->path[0] != '/') {
        return Error{Error::Code::InvalidPath, "Path must start with '/'"};
    }

    // Path cannot end with slash unless it's the root path
    if (this->path.size() > 1 && this->path.back() == '/')
        return Error{Error::Code::InvalidPath, "Path ends with slash"};

    return std::nullopt;
}

template <typename T>
auto Path<T>::validateFull() const -> std::optional<Error> {
    auto result = validate_path_impl(std::string_view(this->path));
    if (result.code != ValidationError::Code::None)
        return Error{Error::Code::InvalidPath, get_error_message(result.code)};
    return std::nullopt;
}

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