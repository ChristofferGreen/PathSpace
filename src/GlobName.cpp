#include "pathspace/core/GlobName.hpp"

#include <string>

namespace SP {

bool match_glob(const std::string_view& globPattern, const std::string_view& str) {
    size_t globIdx = 0;
    size_t strIdx = 0;

    while (strIdx < str.size()) {
        if (globIdx < globPattern.size() && globPattern[globIdx] == '?') {
            globIdx++;
            strIdx++;
        } else if (globIdx < globPattern.size() && globPattern[globIdx] == '*') {
            size_t nextGlobIdx = globIdx + 1;
            while (nextGlobIdx < globPattern.size() && globPattern[nextGlobIdx] == '*') {
                nextGlobIdx++;
            }

            if (nextGlobIdx == globPattern.size()) {
                return true; // Trailing '*' matches everything
            }

            size_t matchIdx = strIdx;
            while (matchIdx < str.size() && str[matchIdx] != globPattern[nextGlobIdx]) {
                matchIdx++;
            }

            if (matchIdx == str.size()) {
                return false;
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
                return false;
            }
        } else {
            if (strIdx < str.size() && globPattern[globIdx] == str[strIdx]) {
                globIdx++;
                strIdx++;
            } else {
                return false;
            }
        }
    }

    while (globIdx < globPattern.size() && globPattern[globIdx] == '*') {
        globIdx++;
    }

    return globIdx == globPattern.size() && strIdx == str.size();
}

GlobName::GlobName(std::string_view const &stringv) : stringv(stringv) {
}

auto GlobName::operator==(char const * const ptr) const -> bool {
    return this->stringv==ptr;
}

auto GlobName::operator==(GlobName const &other) const -> bool {
    bool const selfIsGlob = this->isGlob();
    bool const otherIsGlob = other.isGlob();
    if(!selfIsGlob && !otherIsGlob)
        return this->stringv==other.stringv;
    else if(selfIsGlob && otherIsGlob)
        return this->stringv==other.stringv;
    if(selfIsGlob) {
        return match_glob(this->stringv, other.stringv);
    }else {
        return match_glob(other.stringv, this->stringv);
    }
    return false;
}

auto GlobName::isGlob() const -> bool {
    bool prevWasEscape = false;
    for(auto const ch : this->stringv) {
        if(!prevWasEscape && (ch == '*' || ch == '?'))
            return true;
        prevWasEscape = ch == '\\';
    }
    return false;
}

}