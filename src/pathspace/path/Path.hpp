#pragma once

namespace SP {

template<typename T>
struct Path {
    Path() = default;
    Path(T const &path);

    auto isValid() const -> bool;
    auto getPath() const -> T const&;
protected:
    T path;
};

} // namespace SP
