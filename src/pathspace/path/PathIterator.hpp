#pragma once
#include <string_view>

namespace SP {

class PathIterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = std::string_view;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const value_type*;
    using reference         = const value_type&;
    using IteratorType      = std::string_view::const_iterator;

    explicit PathIterator(std::string_view path) noexcept;

    [[nodiscard]] auto operator*() const noexcept -> value_type;
    [[nodiscard]] auto operator->() const noexcept -> pointer;
    auto               operator++() noexcept -> PathIterator&;
    auto               operator++(int) noexcept -> PathIterator;
    [[nodiscard]] auto operator==(const PathIterator& other) const noexcept -> bool;

    [[nodiscard]] auto isAtStart() const noexcept -> bool;
    [[nodiscard]] auto isAtEnd() const noexcept -> bool;
    [[nodiscard]] auto fullPath() const noexcept -> std::string_view;

private:
    PathIterator(IteratorType first, IteratorType last) noexcept;

    auto               skipSlashes(IteratorType& it) noexcept -> void;
    [[nodiscard]] auto findNextSlash(IteratorType it) noexcept -> IteratorType;
    auto               updateCurrentSegment() noexcept -> void;
    void               findNextComponent() noexcept;

    std::string_view path;            // The complete path we're iterating over
    std::string_view current_segment; // View of the current path component
    IteratorType     current;         // Iterator to start of current component
    IteratorType     segment_end;     // Iterator to end of current component
};

} // namespace SP