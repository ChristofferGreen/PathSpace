#pragma once
#include <string>
#include <string_view>

namespace SP {

template <typename T>
struct ConcreteName {
    ConcreteName() = default;
    ConcreteName(char const* const ptr);
    ConcreteName(std::string const& str);
    ConcreteName(std::string_view const& name);
    ConcreteName(std::string::const_iterator const& iter, std::string::const_iterator const& endIter);
    ConcreteName(std::string_view::const_iterator const& iter, std::string_view::const_iterator const& endIter);

    auto operator<=>(ConcreteName<T> const& other) const -> std::strong_ordering;
    auto operator==(ConcreteName<T> const& other) const -> bool;
    auto operator==(char const* const other) const -> bool;

    auto getName() const -> std::string_view const;

    friend struct GlobName;

private:
    T name;
};

using ConcreteNameStringView = ConcreteName<std::string_view>;
using ConcreteNameString     = ConcreteName<std::string>;

} // namespace SP

namespace std {

template <>
struct hash<SP::ConcreteNameStringView> {
    std::size_t operator()(const SP::ConcreteNameStringView& name) const noexcept {
        return std::hash<std::string_view>{}(name.getName());
    }
};

template <>
struct hash<SP::ConcreteNameString> {
    std::size_t operator()(const SP::ConcreteNameString& name) const noexcept {
        return std::hash<std::string_view>{}(name.getName());
    }
};

} // namespace std