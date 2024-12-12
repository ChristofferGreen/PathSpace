#pragma once
#include "core/Error.hpp"
#include "path/validation.hpp"
#include <optional>
#include <string_view>

namespace SP {

class Iterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = std::string_view;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const value_type*;
    using reference         = const value_type&;
    using IteratorType      = std::string_view::const_iterator;

    explicit Iterator(char const* const path) noexcept;
    explicit Iterator(std::string_view path) noexcept;

    [[nodiscard]] auto operator*() const noexcept -> value_type;
    [[nodiscard]] auto operator->() const noexcept -> pointer;
    auto               operator++() noexcept -> Iterator&;
    auto               operator++(int) noexcept -> Iterator;
    [[nodiscard]] auto operator==(const Iterator& other) const noexcept -> bool;

    [[nodiscard]] auto isAtStart() const noexcept -> bool;
    [[nodiscard]] auto isAtFinalComponent() const noexcept -> bool;
    [[nodiscard]] auto isAtEnd() const noexcept -> bool;
    [[nodiscard]] auto validate(ValidationLevel const& level) const noexcept -> std::optional<Error>;
    [[nodiscard]] auto toString() const noexcept -> std::string;
    [[nodiscard]] auto toStringView() const noexcept -> std::string_view;
    [[nodiscard]] auto currentComponent() const noexcept -> std::string_view;
    [[nodiscard]] auto next() const noexcept -> Iterator;

private:
    Iterator(IteratorType first, IteratorType last) noexcept;

    auto               skipSlashes(IteratorType& it) noexcept -> void;
    auto               updateCurrentSegment() noexcept -> void;
    void               findNextComponent() noexcept;
    [[nodiscard]] auto findNextSlash(IteratorType it) noexcept -> IteratorType;
    [[nodiscard]] auto validateBasic() const noexcept -> std::optional<Error>;
    [[nodiscard]] auto validateFull() const noexcept -> std::optional<Error>;

    std::string_view path;            // The complete path we're iterating over
    std::string_view current_segment; // View of the current path component
    IteratorType     current;         // Iterator to start of current component
    IteratorType     segment_end;     // Iterator to end of current component
};

} // namespace SP