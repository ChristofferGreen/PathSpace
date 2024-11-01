#pragma once

namespace SP {

template <typename T>
struct Path {
    Path() = default;
    Path(T const& path);

    auto isValid() const -> bool;
    auto getPath() const -> T const&;
    auto setPath(T const& path) -> void;
    auto size() const -> size_t;
    auto empty() const -> bool;

protected:
    T path;
};

} // namespace SP