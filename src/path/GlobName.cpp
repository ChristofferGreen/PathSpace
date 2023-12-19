#include "pathspace/path/GlobName.hpp"

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

} // namespace SP