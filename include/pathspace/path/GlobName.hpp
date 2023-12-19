#pragma once
#include "ConcreteName.hpp"

#include <string>
#include <string_view>

namespace SP {

struct GlobName {
    GlobName(char const * const ptr);
    GlobName(std::string::const_iterator const &iter, std::string::const_iterator const &endIter);
    GlobName(std::string_view::const_iterator const &iter, std::string_view::const_iterator const &endIter);

    auto operator<=>(GlobName const &other) const -> std::strong_ordering;
    auto operator==(GlobName const &other) const -> bool;
    auto operator==(ConcreteName const &other) const -> bool;
    auto operator==(char const * const other) const -> bool;

    auto match(const std::string_view& str) const -> std::tuple<bool /*match*/, bool /*supermatch*/>;
    auto match(const ConcreteName& str) const -> std::tuple<bool /*match*/, bool /*supermatch*/>;
private:
    std::string_view name;
};

}