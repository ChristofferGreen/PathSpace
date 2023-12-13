#include "pathspace/core/GlobPath.hpp"
#include "pathspace/core/Path.hpp"

namespace SP {
    
GlobPath::Iterator::Iterator(std::string_view::const_iterator iter, std::string_view::const_iterator endIter)
    : current(iter), end(endIter) {}

GlobPath::Iterator& GlobPath::Iterator::operator++() {
    while (this->current != this->end && *current != '/')
        ++this->current;
    this->skipSlashes();
    return *this;
}

auto GlobPath::Iterator::skipSlashes() -> void {
    while (this->current != this->end && *current == '/')
        ++this->current;
}

auto GlobPath::Iterator::operator==(const Iterator& other) const -> bool{
    return this->current == other.current;
}

auto GlobPath::Iterator::isAtEnd() const -> bool {
    return this->current==this->end;
}

auto GlobPath::begin() const -> GlobPath::Iterator {
    auto start = this->stringv.begin();
    if(this->stringv.size()>0)
        ++start; // Skip initial '/'
    return Iterator(start, stringv.end());
}

auto GlobPath::end() const -> GlobPath::Iterator {
    return Iterator(stringv.end(), stringv.end());
}

auto GlobPath::Iterator::operator*() const -> GlobName {
    auto currentCopy = current;
    auto const start = currentCopy;
    while (currentCopy != this->end && *currentCopy != '/')
        ++currentCopy;
    return GlobName{std::string_view(start, currentCopy)};
}

auto GlobPath::Iterator::operator->() const -> GlobName {
    return **this;
}

GlobPath::GlobPath(char const * const ptr) : stringv(ptr) {}

GlobPath::GlobPath(std::string_view const &stringv) : stringv(stringv) {}

auto GlobPath::operator==(Path const &other) const -> bool {
    if(!this->isValidPath() || !other.isValidPath())
        return false;
    auto iterA = this->begin();
    auto iterB = other.begin();
    while(iterA != this->end() && iterB != other.end()) {
        auto const match = iterA->match(*iterB);
        if(std::get<1>(match)) // Supermatch (**)
            return true;
        if(!std::get<0>(match))
            return false;
        ++iterA;
        ++iterB;
    }
    if(iterA != this->end() || iterB != other.end())
        return false;
    return true;
}

auto GlobPath::operator==(GlobPath const &other) const -> bool {
    if(!this->isValidPath() || !other.isValidPath())
        return false;
    auto iterA = this->begin();
    auto iterB = other.begin();
    while(iterA != this->end() && iterB != other.end()) {
        if(*iterA != *iterB)
            return false;
        ++iterA;
        ++iterB;
    }
    if(iterA != this->end() || iterB != other.end())
        return false;
    return true;
}

auto GlobPath::operator==(char const * const other) const -> bool {
    return *this == Path{other};
}

auto GlobPath::operator<=>(GlobPath const &other) const -> std::strong_ordering {
    return this->stringv<=>other.stringv;
}

auto GlobPath::operator<=>(char const * const other) const -> std::strong_ordering {
    return this->stringv<=>other;
}

auto GlobPath::isValidPath() const -> bool {
    if(this->stringv.size()==0)
        return false;
    if(this->stringv[0] != '/')
        return false;
    return true;
}

}