#include "path/ConstructiblePath.hpp"

namespace SP {

ConstructiblePath::ConstructiblePath() noexcept : path("/") {}

bool ConstructiblePath::operator==(std::string_view other) const {
    return path == other;
}

bool ConstructiblePath::operator==(const ConstructiblePath& other) const {
    return path == other.path;
}

ConstructiblePath& ConstructiblePath::append(std::string_view str) {
    if (!isComplete) {
        if (!str.empty()) {
            if (path.back() == '/' && str.front() == '/') {
                // If both path and str have a slash, skip one
                path.append(str.substr(1));
            } else if (path.back() != '/' && str.front() != '/') {
                // If neither has a slash, add one
                path += '/';
                path.append(str);
            } else {
                // In all other cases, just append
                path.append(str);
            }
        }
    }
    return *this;
}

std::string_view ConstructiblePath::getPath() const noexcept {
    return path;
}
bool ConstructiblePath::isCompleted() const noexcept {
    return isComplete;
}

void ConstructiblePath::markComplete() noexcept {
    isComplete = true;
}

void ConstructiblePath::reset() {
    path       = "/";
    isComplete = false;
}

ConstructiblePath::operator std::string_view() const noexcept {
    return path;
}

} // namespace SP