#include "path/UnvalidatedPath.hpp"

#include "core/Error.hpp"

#include <string>
#include <string_view>

namespace {

using SP::Error;
using SP::Expected;
using SP::UnvalidatedPathView;

constexpr auto make_path_error(std::string message) -> Error {
    return Error{Error::Code::InvalidPath, std::move(message)};
}

constexpr auto component_error(std::string message) -> Error {
    return Error{Error::Code::InvalidPathSubcomponent, std::move(message)};
}

auto split_absolute_impl(std::string_view absolutePath) -> Expected<std::vector<std::string_view>> {
    if (absolutePath.empty() || absolutePath.front() != '/') {
        return std::unexpected(make_path_error("path must be absolute"));
    }

    std::vector<std::string_view> components;
    std::size_t pos  = 1;
    auto const size  = absolutePath.size();
    bool seenComponent = false;

    while (pos <= size) {
        if (pos == size) {
            break; // trailing slash is ignored
        }

        auto next = absolutePath.find('/', pos);
        auto end  = (next == std::string_view::npos) ? size : next;

        if (end == pos) {
            if (next == std::string_view::npos) {
                break; // trailing slash
            }
            return std::unexpected(component_error("empty path component"));
        }

        auto token = absolutePath.substr(pos, end - pos);
        if (token == "." || token == "..") {
            return std::unexpected(component_error("relative path components are not allowed"));
        }

        components.push_back(token);
        seenComponent = true;

        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }

    if (!seenComponent) {
        return std::unexpected(make_path_error("path must contain at least one component"));
    }

    return components;
}

auto contains_relative_tokens_impl(std::string_view candidate) -> bool {
    std::size_t pos = 0;
    auto const size = candidate.size();

    while (pos <= size) {
        if (pos == size) {
            return false;
        }

        auto next = candidate.find('/', pos);
        auto end  = (next == std::string_view::npos) ? size : next;

        if (end == pos) {
            return true; // empty component or trailing slash
        }

        auto token = candidate.substr(pos, end - pos);
        if (token == "." || token == "..") {
            return true;
        }

        if (next == std::string_view::npos) {
            return false;
        }
        pos = next + 1;
    }

    return false;
}

} // namespace

namespace SP {

UnvalidatedPathView::UnvalidatedPathView(std::string_view raw) noexcept
    : raw_(raw) {}

auto UnvalidatedPathView::contains_relative_tokens() const noexcept -> bool {
    return contains_relative_tokens_impl(raw_);
}

auto UnvalidatedPathView::split_absolute_components() const -> Expected<std::vector<std::string_view>> {
    return split_absolute_impl(raw_);
}

auto UnvalidatedPathView::canonicalize_absolute() const -> Expected<std::string> {
    auto components = split_absolute_impl(raw_);
    if (!components) {
        return std::unexpected(components.error());
    }

    std::string result;
    std::size_t required = 1; // leading slash
    for (auto const& comp : *components) {
        required += comp.size() + 1; // component + slash
    }
    if (!components->empty()) {
        required -= 1; // last trailing slash not needed
    }

    result.reserve(required);
    result.push_back('/');
    for (std::size_t i = 0; i < components->size(); ++i) {
        if (i > 0) {
            result.push_back('/');
        }
        result.append((*components)[i]);
    }

    return result;
}

} // namespace SP

