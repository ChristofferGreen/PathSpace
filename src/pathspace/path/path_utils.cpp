#include "path_utils.hpp"
#include "PathIterator.hpp"

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
    auto iterA = PathIterator{pathA};
    auto iterB = PathIterator{pathB};
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
    for (auto const& ch : path) {
        if (ch == '\\' && !previousCharWasEscape) {
            previousCharWasEscape = true;
            continue;
        }
        if (previousCharWasEscape) {
            previousCharWasEscape = false;
            continue;
        }
        if (ch == '*' || ch == '?' || ch == '[' || ch == ']') {
            return true;
        }
    }
    return false;
}

} // namespace SP