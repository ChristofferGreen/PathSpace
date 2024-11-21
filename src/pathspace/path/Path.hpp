#pragma once
#include "core/Error.hpp"
#include "path/validation.hpp"

#include <cstddef>
#include <optional>

namespace SP {

template <typename T>
struct Path {
    Path() = default;
    Path(T const& path);

    auto getPath() const -> T const&;
    auto setPath(T const& path) -> void;
    auto size() const -> size_t;
    auto empty() const -> bool;
    auto validateBasic() const -> std::optional<Error>;
    auto validateFull() const -> std::optional<Error>;

    [[nodiscard]] auto validate(ValidationLevel const& level = ValidationLevel::Basic) const noexcept -> std::optional<Error>;

protected:
    T path;
};

} // namespace SP