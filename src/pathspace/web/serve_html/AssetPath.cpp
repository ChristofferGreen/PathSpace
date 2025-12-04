#include <pathspace/web/serve_html/AssetPath.hpp>

#include <algorithm>
#include <cctype>

namespace SP::ServeHtml {

namespace {

bool IsAssetComponent(std::string_view value) {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.';
    });
}

} // namespace

bool IsAssetPath(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    while (!value.empty() && value.front() == '/') {
        value.remove_prefix(1);
    }
    if (value.empty()) {
        return false;
    }
    std::size_t offset = 0;
    while (offset < value.size()) {
        auto next = value.find('/', offset);
        std::string_view segment;
        if (next == std::string_view::npos) {
            segment = value.substr(offset);
        } else {
            segment = value.substr(offset, next - offset);
        }
        if (!IsAssetComponent(segment)) {
            return false;
        }
        if (next == std::string_view::npos) {
            break;
        }
        offset = next + 1;
    }
    return true;
}

} // namespace SP::ServeHtml

