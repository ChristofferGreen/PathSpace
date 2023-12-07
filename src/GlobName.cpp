#include "pathspace/core/GlobName.hpp"

#include <string>

namespace SP {

GlobName::GlobName(std::string_view const &stringv) : stringv(stringv) {
}

auto GlobName::operator==(char const * const ptr) const -> bool {
    return this->stringv==ptr;
}

auto GlobName::isGlob() -> bool {
    bool prevWasEscape = false;
    for(auto const ch : this->stringv) {
        if(!prevWasEscape && (ch == '*' || ch == '?'))
            return true;
        prevWasEscape = ch == '\\';
    }
    return false;
}

}