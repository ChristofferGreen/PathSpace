#include "pathspace/core/Path.hpp"

namespace SP {
    
Path::Iterator::Iterator(std::string_view::const_iterator iter, std::string_view::const_iterator endIter)
    : current(iter), end(endIter) {}

Path::Iterator& Path::Iterator::operator++() {
    while (this->current != this->end && *current != '/') {
        ++this->current;
    }
    this->skipSlashes();
    return *this;
}

auto Path::Iterator::skipSlashes() -> void {
    while (this->current != this->end && *current == '/')
        ++this->current;
}

auto Path::Iterator::operator==(const Iterator& other) const -> bool{
    return this->current == other.current;
}

auto Path::Iterator::isAtEnd() const -> bool {
    return this->current==this->end;
}

auto Path::begin() const -> Path::Iterator {
    auto start = this->stringv.begin();
    ++start; // Skip initial '/'
    return Iterator(start, this->stringv.end());
}

auto Path::end() const -> Path::Iterator {
    return Iterator(this->stringv.end(), this->stringv.end());
}

auto Path::Iterator::operator*() const -> std::string_view {
    auto currentCopy = current;
    auto const start = currentCopy;
    while (currentCopy != end && *currentCopy != '/')
        ++currentCopy;
    return std::string_view(start, currentCopy);
}

auto Path::Iterator::operator->() const -> std::string_view {
    return **this;
}

Path::Path(std::string_view const &str) : stringv(str) {}

auto Path::operator==(Path const &other) const -> bool {
    return this->stringv==other.stringv;
}

auto Path::operator<(Path const &other) const -> bool {
    return this->stringv<other.stringv;
}

auto Path::operator==(char const * const other) const -> bool {
    return this->stringv==other;
}

auto Path::isValidPath() const -> bool {
    if(this->stringv.size()==0)
        return false;
    if(this->stringv[0] != '/')
        return false;
    return true;
}

}