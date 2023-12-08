#include "pathspace/core/GlobPath.hpp"

namespace SP {
    
GlobPath::Iterator::Iterator(std::string_view::const_iterator iter, std::string_view::const_iterator endIter)
    : current(iter), end(endIter) {}

GlobPath::Iterator& GlobPath::Iterator::operator++() {
    while (current != end && *current != '/') {
        ++current;
    }
    if (current != end) {
        ++current;
    }
    return *this;
}

auto GlobPath::Iterator::operator==(const Iterator& other) const -> bool{
    return current == other.current;
}

auto GlobPath::Iterator::isAtEnd() const -> bool {
    return this->current==this->end;
}

auto GlobPath::begin() const -> GlobPath::Iterator {
    auto start = stringv.begin();
    ++start; // Skip initial '/'
    return Iterator(start, stringv.end());
}

auto GlobPath::end() const -> GlobPath::Iterator {
    return Iterator(stringv.end(), stringv.end());
}

auto GlobPath::Iterator::operator*() const -> GlobName const {
    auto currentCopy = current;
    auto const start = currentCopy;
    while (currentCopy != end && *currentCopy != '/')
        ++currentCopy;
    return GlobName{std::string_view(start, currentCopy)};
}

GlobPath::GlobPath(std::string_view const &stringv) : stringv(stringv) {}

auto GlobPath::operator==(GlobPath const &other) const -> bool {
    return this->stringv==other.stringv;
}

auto GlobPath::operator==(char const * const other) const -> bool {
    return this->stringv==other;
}

auto GlobPath::validPath() const -> bool {
    if(this->stringv.size()==0)
        return false;
    if(this->stringv[0] != '/')
        return false;
    return true;
}

}