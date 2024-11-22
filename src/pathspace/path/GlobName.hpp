#pragma once
#include "ConcreteName.hpp"

#include <string>
#include <string_view>

namespace SP {

auto is_glob(std::string_view const& strv) -> bool;

struct GlobName {
    GlobName(char const* const ptr);
    GlobName(std::string::const_iterator const& iter, std::string::const_iterator const& endIter);
    GlobName(std::string_view::const_iterator const& iter, std::string_view::const_iterator const& endIter);

    auto operator<=>(GlobName const& other) const -> std::strong_ordering;
    auto operator==(GlobName const& other) const -> bool;
    auto operator==(ConcreteNameStringView const& other) const -> bool;
    auto operator==(char const* const other) const -> bool;

    auto match(const std::string_view& str) const -> bool;
    auto match(const ConcreteNameStringView& str) const -> bool;
    auto match(const ConcreteNameString& str) const -> bool;

    auto isConcrete() const -> bool;
    auto isGlob() const -> bool;
    auto getName() const -> std::string_view const&;

private:
    std::string_view name;
};

} // namespace SP