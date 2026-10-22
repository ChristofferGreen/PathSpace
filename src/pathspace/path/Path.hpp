#pragma once

namespace SP {

template<typename T>
struct Path {
    Path() = default;
    Path(T const &path);

    [[nodiscard]] auto isValid() const -> bool;
    [[nodiscard]] auto getPath() const -> T const&;

protected:
    T path;
};

} // namespace SP
