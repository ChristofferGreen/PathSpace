#pragma once
#include <string>
#include <string_view>

namespace SP {

template <typename... Bases>
struct Overload : Bases... {
    using is_transparent = void;
    using Bases::operator()...;
};

struct CharPointerHash {
    auto operator()(const char* ptr) const noexcept {
        return std::hash<std::string_view>{}(ptr);
    }
};

using TransparentStringHash = Overload<
        std::hash<std::string>,
        std::hash<std::string_view>,
        CharPointerHash>;

} // namespace SP