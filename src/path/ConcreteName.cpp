#include "pathspace/path/ConcreteName.hpp"

namespace SP {

ConcreteName::ConcreteName(char const * const ptr) : name(ptr) {}

ConcreteName::ConcreteName(std::string_view const &name) : name(name) {}

ConcreteName::ConcreteName(std::string::const_iterator const &iter, std::string::const_iterator const &endIter) : name(iter, endIter) {}

ConcreteName::ConcreteName(std::string_view::const_iterator const &iter, std::string_view::const_iterator const &endIter) : name(iter, endIter) {}

auto ConcreteName::operator<=>(ConcreteName const &other) const -> std::strong_ordering {
    return this->name<=>other.name;
}

auto ConcreteName::operator==(ConcreteName const &other) const -> bool {
    return this->name==other.name;
}

auto ConcreteName::operator==(char const * const other) const -> bool {
    return this->name==other;
}

auto ConcreteName::getName() const -> std::string_view const& {
    return this->name;
}

} // namespace SP