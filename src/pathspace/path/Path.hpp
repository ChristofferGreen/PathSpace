#pragma once

#include "core/Error.hpp"
#include <cstddef>
#include <optional>
namespace SP {

template <typename T>
struct Path {
    Path() = default;
    Path(T const& path);

    auto getPath() const -> T const&;
    auto setPath(T const& path) -> void;
    auto size() const -> size_t;
    auto empty() const -> bool;

    [[nodiscard]] constexpr auto validate() const noexcept -> std::optional<Error> {
        if (this->path.empty()) {
            return Error{Error::Code::InvalidPath, "Empty path"};
        }

        if (this->path[0] != '/') {
            return Error{Error::Code::InvalidPath, "Path must start with '/'"};
        }

        size_t pos        = 0;
        bool   in_bracket = false;
        bool   prev_slash = true; // Consider start as after slash

        auto makeError = [&pos](std::string_view msg) {
            return Error{
                    Error::Code::InvalidPath,
                    std::string(msg) + " at position " + std::to_string(pos)};
        };

        for (pos = 1; pos < this->path.length(); ++pos) {
            char c = this->path[pos];

            if (c == '/') {
                if (in_bracket)
                    return makeError("Slash not allowed in brackets");

                if (prev_slash)
                    return makeError("Empty path component");

                if (pos + 1 < this->path.length() && this->path[pos + 1] == '.')
                    return makeError("Relative paths not allowed");

                prev_slash = true;
                continue;
            }

            // Handle backslashes - only treat as escape if followed by a glob character
            if (c == '\\' && pos + 1 < this->path.length()) {
                char next = this->path[pos + 1];
                if (next == '*' || next == '?' || next == '[' || next == ']' || next == '\\') {
                    pos++; // Skip next character as it's escaped
                    prev_slash = false;
                    continue;
                }
                // Otherwise, treat backslash as a normal character
            }

            if (c == '[') {
                if (in_bracket)
                    return makeError("Nested brackets not allowed");

                in_bracket = true;

                if (pos + 1 >= this->path.length())
                    return makeError("Unclosed bracket");

                // Handle negation
                if (this->path[pos + 1] == '!') {
                    if (pos + 2 >= this->path.length())
                        return makeError("Empty negated bracket");
                    pos++;
                }
            } else if (c == ']') {
                if (!in_bracket)
                    return makeError("Unmatched closing bracket");

                if (this->path[pos - 1] == '[' || this->path[pos - 1] == '!')
                    return makeError("Empty bracket");

                in_bracket = false;
            } else if (c == '-' && in_bracket) {
                if (pos <= 1 || pos + 1 >= this->path.length())
                    return makeError("Invalid range specification");

                char prev = this->path[pos - 1];
                char next = this->path[pos + 1];

                if (prev >= next || prev == '[' || next == ']')
                    return makeError("Invalid character range");
            }

            prev_slash = false;
        }

        if (in_bracket)
            return makeError("Unclosed bracket at end of path");

        if (this->path.length() > 1 && prev_slash)
            return makeError("Path ends with slash");

        return std::nullopt;
    }

protected:
    T path;
};

} // namespace SP