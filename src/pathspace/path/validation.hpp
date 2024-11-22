#pragma once
#include <cassert>
#include <string_view>

namespace SP {

struct ValidationError {
    enum class Code {
        None,
        EmptyPath,
        MustStartWithSlash,
        EndsWithSlash,
        EmptyPathComponent,
        SlashInBrackets,
        RelativePath,
        NestedBrackets,
        UnclosedBracket,
        EmptyNegatedBracket,
        UnmatchedClosingBracket,
        EmptyBracket,
        InvalidRangeSpec,
        InvalidCharRange,
        NoContent
    };
    Code code;
};

// In validation.hpp
constexpr ValidationError validate_path_impl(std::string_view str) {
    if (str.size() <= 1)
        return {ValidationError::Code::EmptyPath};
    if (str[0] != '/')
        return {ValidationError::Code::MustStartWithSlash};
    if (str.size() > 1 && str[str.size() - 1] == '/')
        return {ValidationError::Code::EndsWithSlash};

    bool inBracket  = false;
    bool prevSlash  = true;
    bool hasContent = false;

    for (size_t i = 1; i < str.size(); ++i) {
        char c = str[i];

        if (c == '/') {
            if (inBracket)
                return {ValidationError::Code::SlashInBrackets};
            if (prevSlash)
                return {ValidationError::Code::EmptyPathComponent};
            if (i > 1 && str[i - 1] == '.') {
                if (i == 2 || str[i - 2] == '/' || (i > 2 && str[i - 2] == '.')) {
                    return {ValidationError::Code::RelativePath};
                }
            }
            prevSlash = true;
            continue;
        }

        if (c == '.') {
            if (i > 0 && str[i - 1] == '/') {
                if (i + 1 == str.size() ||                       // "/." at end
                    (i + 1 < str.size() && str[i + 1] == '.')) { // ".." anywhere
                    return {ValidationError::Code::RelativePath};
                }
            }
        }

        if (c == '\\' && i + 1 < str.size()) {
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
                return {ValidationError::Code::NestedBrackets};
            inBracket = true;
            if (i + 1 >= str.size())
                return {ValidationError::Code::UnclosedBracket};
            if (str[i + 1] == '!') {
                if (i + 2 >= str.size())
                    return {ValidationError::Code::EmptyNegatedBracket};
                i++;
            }
        } else if (c == ']') {
            if (!inBracket)
                return {ValidationError::Code::UnmatchedClosingBracket};
            if (str[i - 1] == '[' || str[i - 1] == '!')
                return {ValidationError::Code::EmptyBracket};
            inBracket = false;
        } else if (c == '-' && inBracket) {
            if (i <= 1 || i + 1 >= str.size())
                return {ValidationError::Code::InvalidRangeSpec};
            char prev = str[i - 1];
            char next = str[i + 1];
            if (prev >= next || prev == '[' || next == ']')
                return {ValidationError::Code::InvalidCharRange};
        }

        prevSlash  = false;
        hasContent = true;
    }

    if (inBracket)
        return {ValidationError::Code::UnclosedBracket};
    if (!hasContent)
        return {ValidationError::Code::NoContent};

    return {ValidationError::Code::None};
}

enum struct ValidationLevel {
    None = 0,
    Basic,
    Full
};

static consteval bool error(const char*) {
    return false;
}

constexpr const char* get_error_message(ValidationError::Code code) {
    switch (code) {
        case ValidationError::Code::EmptyPath:
            return "Empty path";
        case ValidationError::Code::MustStartWithSlash:
            return "Path must start with '/'";
        case ValidationError::Code::EndsWithSlash:
            return "Path ends with slash";
        case ValidationError::Code::EmptyPathComponent:
            return "Empty path component";
        case ValidationError::Code::SlashInBrackets:
            return "Slash not allowed in brackets";
        case ValidationError::Code::RelativePath:
            return "Relative paths not allowed";
        case ValidationError::Code::NestedBrackets:
            return "Nested brackets not allowed";
        case ValidationError::Code::UnclosedBracket:
            return "Unclosed bracket";
        case ValidationError::Code::EmptyNegatedBracket:
            return "Empty negated bracket";
        case ValidationError::Code::UnmatchedClosingBracket:
            return "Unmatched closing bracket";
        case ValidationError::Code::EmptyBracket:
            return "Empty bracket";
        case ValidationError::Code::InvalidRangeSpec:
            return "Invalid range specification";
        case ValidationError::Code::InvalidCharRange:
            return "Invalid character range";
        case ValidationError::Code::NoContent:
            return "Path has no content";
        case ValidationError::Code::None:
            return nullptr;
    }
    return "Unknown error";
}

template <size_t N>
struct FixedString {
    char str[N]{};
    constexpr FixedString(const char (&s)[N]) {
        std::copy(s, s + N, str);
    }
    constexpr operator std::string_view() const {
        return {str, N - 1};
    }
};

consteval bool validate_path(std::string_view str) {
    auto result = validate_path_impl(str);
    if (result.code != ValidationError::Code::None) {
        error(get_error_message(result.code));
        return false;
    }
    return true;
}

} // namespace SP