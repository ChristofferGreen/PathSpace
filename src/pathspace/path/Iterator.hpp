#pragma once
#include "core/Error.hpp"
#include "path/validation.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace SP {

class Iterator {
public:
    using iterator_category = std::forward_iterator_tag;
    using value_type        = std::string_view;
    using difference_type   = std::ptrdiff_t;
    using pointer           = const value_type*;
    using reference         = const value_type&;
    using IteratorType      = std::string::const_iterator;

    explicit Iterator(char const* const path) noexcept;
    explicit Iterator(std::string_view path) noexcept;

    // Safe copy semantics: rebind views/iterators to this->storage
    Iterator(const Iterator& other) noexcept
        : storage(other.storage)
        , path(storage)
        , current(storage.begin())
        , segment_end(storage.begin()) {
        // Recompute iterator positions using offsets from other's storage
        size_t curOff = static_cast<size_t>(other.current - other.storage.begin());
        size_t endOff = static_cast<size_t>(other.segment_end - other.storage.begin());
        if (curOff > storage.size()) curOff = storage.size();
        if (endOff > storage.size()) endOff = storage.size();
        current     = storage.begin() + curOff;
        segment_end = storage.begin() + endOff;
        updateCurrentSegment();
    }

    Iterator& operator=(const Iterator& other) noexcept {
        if (this == &other) return *this;
        storage = other.storage;
        path    = std::string_view{storage};
        // Recompute iterator positions using offsets from other's storage
        size_t curOff = static_cast<size_t>(other.current - other.storage.begin());
        size_t endOff = static_cast<size_t>(other.segment_end - other.storage.begin());
        if (curOff > storage.size()) curOff = storage.size();
        if (endOff > storage.size()) endOff = storage.size();
        current     = storage.begin() + curOff;
        segment_end = storage.begin() + endOff;
        updateCurrentSegment();
        return *this;
    }

    // Default move semantics
    Iterator(Iterator&&) noexcept            = default;
    Iterator& operator=(Iterator&&) noexcept = default;

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
    [[nodiscard]] auto startToCurrent() const noexcept -> std::string_view;
    [[nodiscard]] auto currentToEnd() const noexcept -> std::string_view;
    [[nodiscard]] auto next() const noexcept -> Iterator;

private:
    // Testing hook to reach the iterator-range constructor without widening the public API.
    friend struct IteratorTestAccess;

    Iterator(IteratorType first, IteratorType last) noexcept;

    auto               skipSlashes(IteratorType& it) noexcept -> void;
    auto               updateCurrentSegment() noexcept -> void;
    void               findNextComponent() noexcept;
    [[nodiscard]] auto findNextSlash(IteratorType it) noexcept -> IteratorType;
    [[nodiscard]] auto validateBasic() const noexcept -> std::optional<Error>;
    [[nodiscard]] auto validateFull() const noexcept -> std::optional<Error>;

    std::string       storage;         // Own the complete path we're iterating over
    std::string_view  path;            // View of the complete path
    std::string_view  current_segment; // View of the current path component
    IteratorType      current;         // Iterator to start of current component
    IteratorType      segment_end;     // Iterator to end of current component
};

} // namespace SP
