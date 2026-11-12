#include "path/utils.hpp"
#include "path/Iterator.hpp"

namespace SP {

namespace {

// One step of a glob pattern against a single character. `length` is how many pattern bytes the
// step consumed; std::nullopt means the pattern is malformed (dangling escape, unterminated class).
struct GlobStep {
    std::size_t length  = 0;
    bool        matched = false;
};

auto matchGlobStep(std::string_view pattern, std::size_t pos, char ch) -> std::optional<GlobStep> {
    switch (pattern[pos]) {
        case '\\':
            if (pos + 1 >= pattern.size()) {
                return std::nullopt;
            }
            return GlobStep{2, pattern[pos + 1] == ch};
        case '?':
            return GlobStep{1, true};
        case '[': {
            std::size_t cursor = pos + 1;
            bool const  invert = cursor < pattern.size() && pattern[cursor] == '!';
            if (invert) {
                ++cursor;
            }
            bool matched = false;
            while (cursor < pattern.size() && pattern[cursor] != ']') {
                char low = pattern[cursor];
                if (low == '\\' && cursor + 1 < pattern.size()) {
                    low = pattern[++cursor];
                }
                ++cursor;
                // "x-y" is a range unless the '-' is the last member ("[a-]" holds a literal '-').
                if (cursor + 1 < pattern.size() && pattern[cursor] == '-' && pattern[cursor + 1] != ']') {
                    char const high = pattern[cursor + 1];
                    matched         = matched || (ch >= low && ch <= high);
                    cursor += 2;
                } else {
                    matched = matched || ch == low;
                }
            }
            if (cursor >= pattern.size()) {
                return std::nullopt;
            }
            return GlobStep{cursor + 1 - pos, matched != invert};
        }
        default:
            return GlobStep{1, pattern[pos] == ch};
    }
}

} // namespace

auto matchGlobComponent(std::string_view const pattern, std::string_view const name) -> GlobComponentMatch {
    std::size_t patternIdx = 0;
    std::size_t nameIdx    = 0;
    // Where to resume after the most recent '*' when a later step fails (classic backtracking).
    std::optional<std::size_t> starPatternIdx;
    std::size_t                starNameIdx = 0;

    while (nameIdx < name.size()) {
        if (patternIdx < pattern.size()) {
            if (pattern[patternIdx] == '*') {
                if (patternIdx + 1 < pattern.size() && pattern[patternIdx + 1] == '*') {
                    return {.match = true, .supermatch = true}; // '**' matches across the name edge
                }
                starPatternIdx = ++patternIdx;
                starNameIdx    = nameIdx;
                continue;
            }
            auto step = matchGlobStep(pattern, patternIdx, name[nameIdx]);
            if (!step) {
                return {};
            }
            if (step->matched) {
                patternIdx += step->length;
                ++nameIdx;
                continue;
            }
        }
        if (!starPatternIdx) {
            return {};
        }
        patternIdx = *starPatternIdx;
        nameIdx    = ++starNameIdx;
    }

    while (patternIdx < pattern.size() && pattern[patternIdx] == '*') {
        ++patternIdx;
    }
    return {.match = patternIdx == pattern.size(), .supermatch = false};
}

auto match_names(std::string_view const nameA, std::string_view const nameB) -> bool {
    return matchGlobComponent(nameA, nameB).match;
}

auto match_paths(std::string_view const pathA, std::string_view const pathB) -> bool {
    auto iterA = Iterator{pathA};
    auto iterB = Iterator{pathB};
    while (!iterA.isAtEnd() && !iterB.isAtEnd()) {
        if (!match_names(*iterA, *iterB))
            return false;
        ++iterA;
        ++iterB;
    }
    if (!iterA.isAtEnd() || !iterB.isAtEnd())
        return false;
    return true;
}

namespace {
auto trimPrefixSlash(std::string_view prefix) -> std::string_view {
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.remove_suffix(1);
    }
    return prefix;
}
} // namespace

auto isPathPrefix(std::string_view prefix, std::string_view path) -> bool {
    prefix = trimPrefixSlash(prefix);
    if (prefix.empty() || prefix == "/") {
        return true;
    }
    if (!path.starts_with(prefix)) {
        return false;
    }
    return path.size() == prefix.size() || path[prefix.size()] == '/';
}

auto stripPathPrefix(std::string_view path, std::string_view prefix) -> std::optional<std::string> {
    prefix = trimPrefixSlash(prefix);
    if (prefix.empty() || prefix == "/") {
        return std::string{path};
    }
    if (!isPathPrefix(prefix, path)) {
        return std::nullopt;
    }
    if (path.size() == prefix.size()) {
        return std::string{"/"};
    }
    return std::string{path.substr(prefix.size())};
}

auto is_concrete(std::string_view path) -> bool {
    return !is_glob(path);
}

auto is_glob(std::string_view path) -> bool {
    bool previousCharWasEscape = false;
    for (std::size_t idx = 0; idx < path.size(); ++idx) {
        auto const ch = path[idx];
        if (ch == '\\' && !previousCharWasEscape) {
            previousCharWasEscape = true;
            continue;
        }
        if (previousCharWasEscape) {
            previousCharWasEscape = false;
            continue;
        }
        if (ch == '[') {
            auto rb = path.find(']', idx + 1);
            if (rb != std::string_view::npos) {
                bool digitsOnly = true;
                for (std::size_t i = idx + 1; i < rb; ++i) {
                    char c = path[i];
                    if (c < '0' || c > '9') {
                        digitsOnly = false;
                        break;
                    }
                }
                bool validIndex = digitsOnly && rb > idx + 1 && (rb + 1 == path.size() || path[rb + 1] == '/');
                if (validIndex) {
                    idx = rb;
                    continue;
                }
            }
            return true;
        }
        if (ch == '*' || ch == '?' || ch == ']') {
            return true;
        }
    }
    return false;
}

auto parse_indexed_component(std::string_view component) -> IndexedComponent {
    bool previousEscape = false;
    std::optional<std::size_t> lb;
    for (std::size_t i = 0; i < component.size(); ++i) {
        char c = component[i];
        if (previousEscape) {
            previousEscape = false;
            continue;
        }
        if (c == '\\') {
            previousEscape = true;
            continue;
        }
        if (c == '[') {
            lb = i;
            break;
        }
    }

    if (!lb) {
        return {component, std::nullopt, false};
    }

    bool basePresent = *lb > 0;

    bool escape = false;
    std::optional<std::size_t> rb;
    for (std::size_t i = *lb + 1; i < component.size(); ++i) {
        char c = component[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (c == '\\') {
            escape = true;
            continue;
        }
        if (c == ']') {
            rb = i;
            break;
        }
    }

    // If the bracket starts the component or isn't the final character, treat as a glob/character class.
    if (!basePresent || !rb || *rb != component.size() - 1) {
        return {component, std::nullopt, false};
    }

    auto indexView = component.substr(*lb + 1, *rb - *lb - 1);
    if (indexView.empty()) {
        return {component, std::nullopt, true};
    }
    std::size_t idx = 0;
    for (char c : indexView) {
        if (c < '0' || c > '9') {
            return {component, std::nullopt, true};
        }
        idx = idx * 10 + static_cast<std::size_t>(c - '0');
    }

    return {component.substr(0, *lb), idx, false};
}

auto append_index_suffix(std::string_view base, std::size_t index) -> std::string {
    if (index == 0) {
        return std::string{base};
    }
    std::string result{base};
    result.push_back('[');
    result.append(std::to_string(index));
    result.push_back(']');
    return result;
}

} // namespace SP
