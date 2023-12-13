#include "pathspace/core/Path.hpp"

namespace SP {
    
Path::Iterator::Iterator(std::string::const_iterator iter, std::string::const_iterator endIter)
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
    auto start = this->string.begin();
    ++start; // Skip initial '/'
    return Iterator(start, string.end());
}

auto Path::end() const -> Path::Iterator {
    return Iterator(string.end(), string.end());
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

Path::Path(std::string const &str) : string(str) {}

auto Path::operator==(Path const &other) const -> bool {
    return this->string==other.string;
}

auto Path::operator<(Path const &other) const -> bool {
    return this->string<other.string;
}

auto Path::operator==(char const * const other) const -> bool {
    return this->string==other;
}

auto Path::isValidPath() const -> bool {
    if(this->string.size()==0)
        return false;
    if(this->string[0] != '/')
        return false;
    return true;
}

}