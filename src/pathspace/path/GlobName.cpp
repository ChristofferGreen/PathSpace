#include "GlobName.hpp"

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

auto GlobName::match(const std::string_view& str) const -> std::tuple<bool /*match*/, bool /*supermatch*/> {
    auto const result = matchGlobComponent(this->name, str);
    return {result.match, result.supermatch};
}

auto GlobName::match(const ConcreteName& str) const -> std::tuple<bool /*match*/, bool /*supermatch*/> {
    return this->match(str.name);
}

auto GlobName::isConcrete() const -> bool {
    return !this->isGlob();
}

auto GlobName::isGlob() const -> bool {
    return is_glob(this->name);
}

auto GlobName::getName() const -> std::string_view const& {
    return this->name;
}

} // namespace SP
