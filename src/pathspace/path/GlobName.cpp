#include "GlobName.hpp"

namespace SP {

auto is_glob(std::string_view const& strv) -> bool {
    bool previousCharWasEscape = false;
    for (auto const& ch : strv) {
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

GlobName::GlobName(char const* const ptr)
    : name(ptr) {
}

GlobName::GlobName(std::string_view view)
    : name(view) {
}

GlobName::GlobName(std::string::const_iterator const& iter, std::string::const_iterator const& endIter)
    : name(iter, endIter) {
}

GlobName::GlobName(std::string_view::const_iterator const& iter, std::string_view::const_iterator const& endIter)
    : name(iter, endIter) {
}

auto GlobName::operator<=>(GlobName const& other) const -> std::strong_ordering {
    return this->name <=> other.name;
}

auto GlobName::operator==(GlobName const& other) const -> bool {
    return this->name == other.name;
}

auto GlobName::operator==(ConcreteNameStringView const& other) const -> bool {
    return this->name == other.name;
}

auto GlobName::operator==(char const* const other) const -> bool {
    return this->name == other;
}

auto GlobName::match(const std::string_view& str) const -> bool {
    size_t globIdx = 0;
    size_t strIdx  = 0;

    while (strIdx < str.size()) {
        if (globIdx >= this->name.size()) {
            return false;
        }

        if (this->name[globIdx] == '\\') {
            // Handle escape character
            globIdx++; // Skip backslash
            if (globIdx < this->name.size() && this->name[globIdx] == str[strIdx]) {
                // Match the escaped character literally
                ++strIdx;
                ++globIdx;
            } else {
                return false;
            }
        } else if (globIdx < this->name.size() && this->name[globIdx] == '?') {
            globIdx++;
            strIdx++;
        } else if (globIdx < this->name.size() && this->name[globIdx] == '*') {
            size_t nextGlobIdx = globIdx + 1;

            if (nextGlobIdx == this->name.size()) {
                return true; // Trailing '*' matches everything
            }

            size_t matchIdx = strIdx;
            while (matchIdx < str.size() && str[matchIdx] != this->name[nextGlobIdx]) {
                matchIdx++;
            }

            if (matchIdx == str.size()) {
                return false;
            }

            globIdx = nextGlobIdx;
            strIdx  = matchIdx;
        } else if (globIdx < this->name.size() && this->name[globIdx] == '[') {
            globIdx++;
            bool invert = false;
            if (globIdx < this->name.size() && this->name[globIdx] == '!') {
                invert = true;
                globIdx++;
            }

            bool matched  = false;
            char prevChar = '\0';

            while (globIdx < this->name.size() && this->name[globIdx] != ']') {
                if (this->name[globIdx] == '-' && prevChar != '\0' && globIdx + 1 < this->name.size()) {
                    char rangeEnd = this->name[globIdx + 1];
                    // Check if current string character is within range
                    if (strIdx < str.size() && str[strIdx] >= prevChar && str[strIdx] <= rangeEnd) {
                        matched = true;
                    }
                    globIdx += 2; // Skip both the hyphen and range end character
                } else {
                    if (strIdx < str.size() && str[strIdx] == this->name[globIdx]) {
                        matched = true;
                    }
                    prevChar = this->name[globIdx];
                    globIdx++;
                }
            }

            if (globIdx >= this->name.size() || this->name[globIdx] != ']') {
                return false; // Malformed pattern - missing closing bracket
            }

            if ((!invert && !matched) || (invert && matched)) {
                return false;
            }

            globIdx++; // Skip the closing bracket
            strIdx++;  // Move to next character in string
        } else {
            if (strIdx < str.size() && this->name[globIdx] == str[strIdx]) {
                globIdx++;
                strIdx++;
            } else {
                return false;
            }
        }
    }

    // Skip any remaining wildcards
    while (globIdx < this->name.size() && this->name[globIdx] == '*') {
        globIdx++;
    }

    return globIdx == this->name.size() && strIdx == str.size();
}

auto GlobName::match(const ConcreteNameStringView& str) const -> bool {
    return this->match(str.name);
}

auto GlobName::match(const ConcreteNameString& str) const -> bool {
    return this->match(str.getName());
}

auto GlobName::match(const GlobName& str) const -> bool {
    return this->match(str.getName());
}

auto GlobName::isConcrete() const -> bool {
    return !this->isGlob();
}

auto GlobName::isGlob() const -> bool {
    return is_glob(this->name);
}

auto GlobName::getName() const -> std::string_view const& {
    return this->name;
}

} // namespace SP