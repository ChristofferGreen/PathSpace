#include "pathspace/core/GlobName.hpp"

#include <string>

namespace SP {

static auto match_glob(const std::string_view& globPattern, const std::string_view& str) -> std::tuple<bool /*match*/, bool /*supermatch*/>{
    size_t globIdx = 0;
    size_t strIdx = 0;

    while (strIdx < str.size()) {
        if (globPattern[globIdx] == '\\') {
            // Handle escape character
            globIdx++;  // Skip backslash
            if (globIdx < globPattern.size() && globPattern[globIdx] == str[strIdx]) {
                // Match the escaped character literally
                ++strIdx;
                ++globIdx;
            } else {
                // Mismatch, glob pattern does not match filename
                return {false, false};
            }
        } else if (globIdx < globPattern.size() && globPattern[globIdx] == '?') {
            globIdx++;
            strIdx++;
        } else if (globIdx < globPattern.size() && globPattern[globIdx] == '*') {
            size_t nextGlobIdx = globIdx + 1;
            if (nextGlobIdx < globPattern.size() && globPattern[nextGlobIdx] == '*') { // ** matches across name edge
                return {true, true};
            }

            if (nextGlobIdx == globPattern.size()) {
                return {true, false}; // Trailing '*' matches everything
            }

            size_t matchIdx = strIdx;
            while (matchIdx < str.size() && str[matchIdx] != globPattern[nextGlobIdx]) {
                matchIdx++;
            }

            if (matchIdx == str.size()) {
                return {false, false};
            }

            globIdx = nextGlobIdx;
            strIdx = matchIdx;
        } else if (globIdx < globPattern.size() && globPattern[globIdx] == '[') {
            globIdx++;
            bool invert = false;
            if (globIdx < globPattern.size() && globPattern[globIdx] == '!') {
                invert = true;
                globIdx++;
            }

            bool matched = false;
            char prevChar = '\0';
            bool inRange = false;

            while (globIdx < globPattern.size() && globPattern[globIdx] != ']') {
                if (globPattern[globIdx] == '-' && prevChar != '\0' && globIdx + 1 < globPattern.size()) {
                    inRange = true;
                    prevChar = globPattern[globIdx + 1];
                    globIdx += 2;
                } else {
                    if (inRange) {
                        if (strIdx < str.size() && str[strIdx] >= prevChar && str[strIdx] <= globPattern[globIdx]) {
                            matched = true;
                        }
                        inRange = false;
                    } else {
                        if (strIdx < str.size() && str[strIdx] == globPattern[globIdx]) {
                            matched = true;
                        }
                    }
                    prevChar = globPattern[globIdx];
                    globIdx++;
                }
            }

            if ((invert && !matched) || (!invert && matched)) {
                strIdx++;
            } else {
                return {false, false};
            }
        } else {
            if (strIdx < str.size() && globPattern[globIdx] == str[strIdx]) {
                globIdx++;
                strIdx++;
            } else {
                return {false, false};
            }
        }
    }

    while (globIdx < globPattern.size() && globPattern[globIdx] == '*') {
        globIdx++;
    }

    return {globIdx == globPattern.size() && strIdx == str.size(), false};
}

GlobName::GlobName(std::string_view const &stringv) : stringv(stringv) {
}

auto GlobName::operator==(char const * const ptr) const -> bool {
    return this->stringv==ptr;
}

auto GlobName::operator==(GlobName const &other) const -> bool {
    return this->stringv == other.stringv;
}

auto GlobName::operator==(std::string_view const &other) const -> bool {
    return std::get<0>(match_glob(this->stringv, other));
}

auto GlobName::operator->() const -> GlobName const * {
    return this;
}

auto GlobName::isMatch(std::string_view const &other) const -> std::tuple<bool /*match*/, bool /*supermatch*/> {
    return match_glob(this->stringv, other);
}

auto GlobName::isMatch(GlobName const &other) const -> std::tuple<bool /*match*/, bool /*supermatch*/> {
    return {this->stringv==other.stringv, false};
}

}