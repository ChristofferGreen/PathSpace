#include "pathspace/core/GlobPath.hpp"

namespace SP {

GlobPath::GlobPath(std::string_view const &stringv) : stringv(stringv) {
    if(stringv.size()>=1) {
        currentNameIterator = stringv.begin();
        ++currentNameIterator;
    }
}

auto GlobPath::validPath() const -> bool {
    if(this->stringv.size()==0)
        return false;
    if(this->stringv[0] != '/')
        return false;
    return true;
}

auto GlobPath::toString() const -> std::string {
    return std::string(this->stringv);
}

auto GlobPath::currentName() const -> std::optional<GlobName> {
    if(this->currentNameIterator == this->stringv.end())
        return std::nullopt;
    auto iter = this->currentNameIterator;
    while(iter != this->stringv.end() && *iter != '/')
        ++iter;
    return std::string_view(this->currentNameIterator, iter);
}

auto GlobPath::moveToNextName() -> void {
    ++this->currentNameIterator;
    while(this->currentNameIterator != this->stringv.end() && *this->currentNameIterator != '/')
        ++this->currentNameIterator;
    if(this->currentNameIterator != this->stringv.end())
        ++this->currentNameIterator;
}

} // namespace SP