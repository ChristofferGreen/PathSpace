#pragma once
#include <cassert>
#include <string_view>

namespace SP {

static consteval bool error(const char*) {
    return false;
}

template <size_t N>
struct fixed_string {
    char str[N]{};
    constexpr fixed_string(const char (&s)[N]) {
        for (size_t i = 0; i < N; ++i)
            str[i] = s[i];
    }
    constexpr operator std::string_view() const {
        return {str, N - 1};
    }
};

template <size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;

consteval bool validate_path(std::string_view str) {
    if (str.size() <= 1)
        return error("Empty path");
    if (str[0] != '/')
        return error("Path must start with '/'");
    if (str.size() > 1 && str[str.size() - 2] == '/')
        return error("Path ends with slash");

    bool inBracket  = false;
    bool prevSlash  = true;
    bool hasContent = false;

    for (size_t i = 1; i < str.size() - 1; ++i) {
        char c = str[i];

        if (c == '/') {
            if (inBracket)
                return error("Slash not allowed in brackets");
            if (prevSlash)
                return error("Empty path component");
            if (i + 1 < str.size() - 1 && str[i + 1] == '.') {
                if (i + 2 == str.size() - 1 || str[i + 2] == '/' || (i + 3 < str.size() - 1 && str[i + 2] == '.' && str[i + 3] == '/')) {
                    return error("Relative paths not allowed");
                }
            }
            prevSlash = true;
            continue;
        }

        if (c == '\\' && i + 1 < str.size() - 1) {
            char next = str[i + 1];
            if (next == '*' || next == '?' || next == '[' || next == ']' || next == '\\') {
                i++;
                prevSlash  = false;
                hasContent = true;
                continue;
            }
        }

        if (c == '[') {
            if (inBracket)
                return error("Nested brackets not allowed");
            inBracket = true;
            if (i + 1 >= str.size() - 1)
                return error("Unclosed bracket");
            if (str[i + 1] == '!') {
                if (i + 2 >= str.size() - 1)
                    return error("Empty negated bracket");
                i++;
            }
        } else if (c == ']') {
            if (!inBracket)
                return error("Unmatched closing bracket");
            if (str[i - 1] == '[' || str[i - 1] == '!')
                return error("Empty bracket");
            inBracket = false;
        } else if (c == '-' && inBracket) {
            if (i <= 1 || i + 1 >= str.size() - 1)
                return error("Invalid range specification");
            char prev = str[i - 1];
            char next = str[i + 1];
            if (prev >= next || prev == '[' || next == ']') {
                return error("Invalid character range");
            }
        }

        prevSlash  = false;
        hasContent = true;
    }

    if (inBracket)
        return error("Unclosed bracket at end of path");
    if (!hasContent)
        return error("Path has no content");
    return true;
}

} // namespace SP