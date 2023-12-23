#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace SP {

struct ConcreteName {
    ConcreteName(char const * const ptr);
    ConcreteName(std::string_view const &name);
    ConcreteName(std::string::const_iterator const &iter, std::string::const_iterator const &endIter);
    ConcreteName(std::string_view::const_iterator const &iter, std::string_view::const_iterator const &endIter);

    auto operator<=>(ConcreteName const &other) const -> std::strong_ordering;
    auto operator==(ConcreteName const &other) const -> bool;
    auto operator==(char const * const other) const -> bool;

    auto getName() const -> std::string_view const&;

    friend struct GlobName;
private:
    std::string_view name;
};

}

namespace std {

template<>
struct hash<SP::ConcreteName> {
    std::size_t operator()(const SP::ConcreteName& name) const noexcept {
        return std::hash<std::string_view>{}(name.getName());
    }
};

} // namespace std