#pragma once

#include <algorithm>
#include <cctype>
#include <string_view>

namespace SP::ServeHtml {

inline bool is_identifier(std::string_view value) {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

} // namespace SP::ServeHtml

