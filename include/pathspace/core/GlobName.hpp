#pragma once
#include <string_view>

namespace SP {

struct GlobName {
    GlobName(std::string_view const &stringv);

    auto operator==(char const * const ptr) const -> bool;
    auto operator==(GlobName const &gn) const -> bool;

    auto isGlob() const -> bool;
private:
    std::string_view stringv;
};

}