#include "path/utils.hpp"
#include "path/Iterator.hpp"

namespace SP {

auto match_names(std::string_view const nameA, std::string_view const nameB) -> bool {
    size_t nameAIdx = 0;
    size_t nameBIdx = 0;

    while (nameBIdx < nameB.size()) {
        if (nameAIdx >= nameA.size()) {
            return false;
        }

        if (nameA[nameAIdx] == '\\') {
            // Handle escape character
            nameAIdx++; // Skip backslash
            if (nameAIdx < nameA.size() && nameA[nameAIdx] == nameB[nameBIdx]) {
                // Match the escaped character literally
                ++nameBIdx;
                ++nameAIdx;
            } else {
                return false;
            }
        } else if (nameAIdx < nameA.size() && nameA[nameAIdx] == '?') {
            nameAIdx++;
            nameBIdx++;
        } else if (nameAIdx < nameA.size() && nameA[nameAIdx] == '*') {
            size_t nextnameAIdx = nameAIdx + 1;

            if (nextnameAIdx == nameA.size()) {
                return true; // Trailing '*' matches everything
            }

            size_t matchIdx = nameBIdx;
            while (matchIdx < nameB.size() && nameB[matchIdx] != nameA[nextnameAIdx]) {
                matchIdx++;
            }

            if (matchIdx == nameB.size()) {
                return false;
            }

            nameAIdx = nextnameAIdx;
            nameBIdx = matchIdx;
        } else if (nameAIdx < nameA.size() && nameA[nameAIdx] == '[') {
            nameAIdx++;
            bool invert = false;
            if (nameAIdx < nameA.size() && nameA[nameAIdx] == '!') {
                invert = true;
                nameAIdx++;
            }

            bool matched  = false;
            char prevChar = '\0';

            while (nameAIdx < nameA.size() && nameA[nameAIdx] != ']') {
                if (nameA[nameAIdx] == '-' && prevChar != '\0' && nameAIdx + 1 < nameA.size()) {
                    char rangeEnd = nameA[nameAIdx + 1];
                    // Check if current nameBing character is within range
                    if (nameBIdx < nameB.size() && nameB[nameBIdx] >= prevChar && nameB[nameBIdx] <= rangeEnd) {
                        matched = true;
                    }
                    nameAIdx += 2; // Skip both the hyphen and range end character
                } else {
                    if (nameBIdx < nameB.size() && nameB[nameBIdx] == nameA[nameAIdx]) {
                        matched = true;
                    }
                    prevChar = nameA[nameAIdx];
                    nameAIdx++;
                }
            }

            if (nameAIdx >= nameA.size() || nameA[nameAIdx] != ']') {
                return false; // Malformed pattern - missing closing bracket
            }

            if ((!invert && !matched) || (invert && matched)) {
                return false;
            }

            nameAIdx++; // Skip the closing bracket
            nameBIdx++; // Move to next character in nameBing
        } else {
            if (nameBIdx < nameB.size() && nameA[nameAIdx] == nameB[nameBIdx]) {
                nameAIdx++;
                nameBIdx++;
            } else {
                return false;
            }
        }
    }

    // Skip any remaining wildcards
    while (nameAIdx < nameA.size() && nameA[nameAIdx] == '*') {
        nameAIdx++;
    }

    return nameAIdx == nameA.size() && nameBIdx == nameB.size();
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

auto append_index_suffix(std::string const& base, std::size_t index) -> std::string {
    if (index == 0) {
        return base;
    }
    std::string result = base;
    result.push_back('[');
    result.append(std::to_string(index));
    result.push_back(']');
    return result;
}

} // namespace SP
