#include <pathspace/ui/DebugFlags.hpp>

#include <array>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

constexpr auto kEnvFlags = std::to_array({
    "PATHSPACE_UI_DEBUG_TREE",
    "PATHSPACE_UI_DEBUG_DIAGNOSTICS",
    "PATHSPACE_UI_DEBUG_PATHSPACE",
});

auto parse_truthy(char const* value) -> bool {
    if (value == nullptr) {
        return false;
    }
    std::string_view text{value};
    if (text.empty()) {
        return true;
    }
    auto is_false = [](char ch) {
        return ch == ' ' || ch == '\t' || ch == '\n';
    };
    while (!text.empty() && is_false(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_false(text.back())) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return true;
    }
    auto lower = text;
    std::string normalized;
    normalized.reserve(lower.size());
    for (char ch : lower) {
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (normalized == "0" || normalized == "false" || normalized == "off" || normalized == "no") {
        return false;
    }
    return true;
}

auto env_debug_enabled() -> bool {
    for (auto const* name : kEnvFlags) {
        if (parse_truthy(std::getenv(name))) {
            return true;
        }
    }
    return false;
}

} // namespace

namespace SP::UI {

auto DebugTreeWritesEnabled() -> bool {
    static bool enabled = env_debug_enabled();
    return enabled;
}

} // namespace SP::UI
