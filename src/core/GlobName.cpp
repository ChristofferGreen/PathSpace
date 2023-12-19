#include "pathspace/core/GlobName.hpp"

#include <string>

namespace SP2 {

GlobName::GlobName(std::string_view const &stringv) : stringv(stringv) {
}

auto GlobName::operator==(char const * const ptr) const -> bool {
    return this->stringv==ptr;
}

auto GlobName::operator==(GlobName const &other) const -> bool {
    return this->stringv == other.stringv;
}

auto GlobName::operator==(std::string_view const &other) const -> bool {
    return std::get<0>(this->match(other));
}

auto GlobName::operator->() const -> GlobName const * {
    return this;
}

auto GlobName::match(std::string_view const &str) const -> std::tuple<bool /*match*/, bool /*supermatch*/> {
    size_t globIdx = 0;
    size_t strIdx = 0;

    while (strIdx < str.size()) {
        if (this->stringv[globIdx] == '\\') {
            // Handle escape character
            globIdx++;  // Skip backslash
            if (globIdx < this->stringv.size() && this->stringv[globIdx] == str[strIdx]) {
                // Match the escaped character literally
                ++strIdx;
                ++globIdx;
            } else {
                // Mismatch, glob pattern does not match filename
                return {false, false};
            }
        } else if (globIdx < this->stringv.size() && this->stringv[globIdx] == '?') {
            globIdx++;
            strIdx++;
        } else if (globIdx < this->stringv.size() && this->stringv[globIdx] == '*') {
            size_t nextGlobIdx = globIdx + 1;
            if (nextGlobIdx < this->stringv.size() && this->stringv[nextGlobIdx] == '*') { // ** matches across name edge
                return {true, true};
            }

            if (nextGlobIdx == this->stringv.size()) {
                return {true, false}; // Trailing '*' matches everything
            }

            size_t matchIdx = strIdx;
            while (matchIdx < str.size() && str[matchIdx] != this->stringv[nextGlobIdx]) {
                matchIdx++;
            }

            if (matchIdx == str.size()) {
                return {false, false};
            }

            globIdx = nextGlobIdx;
            strIdx = matchIdx;
        } else if (globIdx < this->stringv.size() && this->stringv[globIdx] == '[') {
            globIdx++;
            bool invert = false;
            if (globIdx < this->stringv.size() && this->stringv[globIdx] == '!') {
                invert = true;
                globIdx++;
            }

            bool matched = false;
            char prevChar = '\0';
            bool inRange = false;

            while (globIdx < this->stringv.size() && this->stringv[globIdx] != ']') {
                if (this->stringv[globIdx] == '-' && prevChar != '\0' && globIdx + 1 < this->stringv.size()) {
                    inRange = true;
                    prevChar = this->stringv[globIdx + 1];
                    globIdx += 2;
                } else {
                    if (inRange) {
                        if (strIdx < str.size() && str[strIdx] >= prevChar && str[strIdx] <= this->stringv[globIdx]) {
                            matched = true;
                        }
                        inRange = false;
                    } else {
                        if (strIdx < str.size() && str[strIdx] == this->stringv[globIdx]) {
                            matched = true;
                        }
                    }
                    prevChar = this->stringv[globIdx];
                    globIdx++;
                }
            }

            if ((invert && !matched) || (!invert && matched)) {
                strIdx++;
            } else {
                return {false, false};
            }
        } else {
            if (strIdx < str.size() && this->stringv[globIdx] == str[strIdx]) {
                globIdx++;
                strIdx++;
            } else {
                return {false, false};
            }
        }
    }

    while (globIdx < this->stringv.size() && this->stringv[globIdx] == '*') {
        globIdx++;
    }

    return {globIdx == this->stringv.size() && strIdx == str.size(), false};
}

}