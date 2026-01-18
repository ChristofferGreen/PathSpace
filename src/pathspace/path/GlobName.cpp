#include "GlobName.hpp"

namespace SP {

GlobName::GlobName(char const * const ptr) : name(ptr) {}

GlobName::GlobName(std::string::const_iterator const &iter, std::string::const_iterator const &endIter) : name(iter, endIter) {}

GlobName::GlobName(std::string_view::const_iterator const &iter, std::string_view::const_iterator const &endIter) : name(iter, endIter) {}

auto GlobName::operator<=>(GlobName const &other) const -> std::strong_ordering {
    return this->name<=>other.name;
}

auto GlobName::operator==(GlobName const &other) const -> bool {
    return this->name==other.name;
}

auto GlobName::operator==(ConcreteName const &other) const -> bool {
    return this->name==other.name;
}

auto GlobName::operator==(char const * const other) const -> bool {
    return this->name==other;
}

auto GlobName::match(const std::string_view& str) const -> std::tuple<bool /*match*/, bool /*supermatch*/> {
    size_t globIdx = 0;
    size_t strIdx = 0;

    while (strIdx < str.size()) {
        if (this->name[globIdx] == '\\') {
            // Handle escape character
            globIdx++;  // Skip backslash
            if (globIdx < this->name.size() && this->name[globIdx] == str[strIdx]) {
                // Match the escaped character literally
                ++strIdx;
                ++globIdx;
            } else {
                // Mismatch, glob pattern does not match filename
                return {false, false};
            }
        } else if (globIdx < this->name.size() && this->name[globIdx] == '?') {
            globIdx++;
            strIdx++;
        } else if (globIdx < this->name.size() && this->name[globIdx] == '*') {
            size_t nextGlobIdx = globIdx + 1;
            if (nextGlobIdx < this->name.size() && this->name[nextGlobIdx] == '*') { // ** matches across name edge
                return {true, true};
            }

            if (nextGlobIdx == this->name.size()) {
                return {true, false}; // Trailing '*' matches everything
            }

            size_t matchIdx = strIdx;
            while (matchIdx < str.size() && str[matchIdx] != this->name[nextGlobIdx]) {
                matchIdx++;
            }

            if (matchIdx == str.size()) {
                return {false, false};
            }

            globIdx = nextGlobIdx;
            strIdx = matchIdx;
        } else if (globIdx < this->name.size() && this->name[globIdx] == '[') {
            ++globIdx;

            bool invert = false;
            if (globIdx < this->name.size() && this->name[globIdx] == '!') {
                invert = true;
                ++globIdx;
            }

            if (strIdx >= str.size()) {
                return {false, false};
            }

            bool matched = false;
            while (globIdx < this->name.size() && this->name[globIdx] != ']') {
                if (globIdx + 2 < this->name.size() && this->name[globIdx + 1] == '-' && this->name[globIdx + 2] != ']') {
                    const char low = this->name[globIdx];
                    const char high = this->name[globIdx + 2];
                    if (str[strIdx] >= low && str[strIdx] <= high) {
                        matched = true;
                    }
                    globIdx += 3;
                    continue;
                }

                if (str[strIdx] == this->name[globIdx]) {
                    matched = true;
                }
                ++globIdx;
            }

            if (globIdx == this->name.size()) { // Unterminated character class
                return {false, false};
            }
            ++globIdx; // Skip closing ']'

            if ((matched && !invert) || (!matched && invert)) {
                ++strIdx;
            } else {
                return {false, false};
            }
        } else {
            if (strIdx < str.size() && this->name[globIdx] == str[strIdx]) {
                globIdx++;
                strIdx++;
            } else {
                return {false, false};
            }
        }
    }

    while (globIdx < this->name.size() && this->name[globIdx] == '*') {
        globIdx++;
    }

    return {globIdx == this->name.size() && strIdx == str.size(), false};
}

auto GlobName::match(const ConcreteName& str) const -> std::tuple<bool /*match*/, bool /*supermatch*/> {
    return this->match(str.name);
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
