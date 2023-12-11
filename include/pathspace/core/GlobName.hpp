#pragma once
#include <tuple>
#include <string_view>

namespace SP {

struct GlobName {
    GlobName(std::string_view const &stringv);

    auto operator==(char const * const ptr) const -> bool;
    auto operator==(GlobName const &gn) const -> bool;
    auto operator->() const -> GlobName const *;

    auto isGlob() const -> bool;
    auto isMatch(GlobName const &other) const -> std::tuple<bool /*match*/, bool /*supermatch*/>;
    auto isMatch(std::string_view const &other) const -> std::tuple<bool /*match*/, bool /*supermatch*/>;
private:
    std::string_view stringv;
};

}