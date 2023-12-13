#pragma once
#include <tuple>
#include <string_view>

namespace SP {

struct GlobName {
    GlobName(std::string_view const &stringv);

    auto operator==(char const * const other) const -> bool;
    auto operator==(std::string_view const &other) const -> bool;
    auto operator==(GlobName const &other) const -> bool;
    auto operator->() const -> GlobName const *;

    auto match(const std::string_view& str) const -> std::tuple<bool /*match*/, bool /*supermatch*/>;
private:
    std::string_view stringv;
};

}