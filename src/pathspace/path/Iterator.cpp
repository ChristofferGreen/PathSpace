#include "Iterator.hpp"

namespace SP {

Iterator::Iterator(std::string_view pathIn) noexcept
    : storage{std::string(pathIn)},
      path{storage},
      current{storage.begin()},
      segment_end{storage.begin()} {
    findNextComponent();
}
Iterator::Iterator(char const* const pathIn) noexcept
    : Iterator{std::string_view{pathIn}} {}

Iterator::Iterator(IteratorType first, IteratorType last) noexcept
    : storage{std::string(first, last)},
      path{storage},
      current{storage.begin()},
      segment_end{storage.begin()} {
    skipSlashes(current);
    segment_end = findNextSlash(current);
    updateCurrentSegment();
}

void Iterator::findNextComponent() noexcept {
    // Skip any leading separators
    while (current != storage.end() && *current == '/') {
        ++current;
    }

    // Find end of component (next separator or end)
    segment_end = current;
    while (segment_end != storage.end() && *segment_end != '/') {
        ++segment_end;
    }

    updateCurrentSegment();
}

auto Iterator::toString() const noexcept -> std::string {
    return std::string{this->path};
}

auto Iterator::toStringView() const noexcept -> std::string_view {
    return std::string_view{this->path};
}

auto Iterator::currentComponent() const noexcept -> std::string_view {
    return this->current_segment;
}

auto Iterator::startToCurrent() const noexcept -> std::string_view {
    // start at the first character after the leading '/'
    size_t startIdx = 0;
    if (!this->storage.empty() && this->storage.front() == '/') {
        startIdx = 1;
    }
    size_t currentIdx = static_cast<size_t>(this->current - this->storage.begin());
    // Drop a trailing separator so the slice reflects completed components only.
    if (currentIdx > startIdx && this->storage[currentIdx - 1] == '/') {
        currentIdx -= 1;
    }
    size_t len = currentIdx >= startIdx ? currentIdx - startIdx : 0;
    return std::string_view{this->storage.data() + startIdx, len};
}

auto Iterator::currentToEnd() const noexcept -> std::string_view {
    size_t currentIdx = static_cast<size_t>(this->current - this->storage.begin());
    size_t len        = static_cast<size_t>(this->storage.size() - currentIdx);
    return std::string_view{this->storage.data() + currentIdx, len};
}

auto Iterator::next() const noexcept -> Iterator {
    auto iter = *this;
    ++iter;
    return iter;
}

auto Iterator::operator*() const noexcept -> value_type {
    return current_segment;
}

auto Iterator::operator->() const noexcept -> pointer {
    return &current_segment;
}

auto Iterator::operator++() noexcept -> Iterator& {
    if (!isAtEnd()) {
        current = segment_end;
        findNextComponent();
    }
    return *this;
}

auto Iterator::operator++(int) noexcept -> Iterator {
    Iterator tmp = *this;
    ++*this;
    return tmp;
}

auto Iterator::operator==(const Iterator& other) const noexcept -> bool {
    if (this->path != other.path) {
        return false;
    }
    auto offset      = static_cast<size_t>(this->current - this->storage.begin());
    auto otherOffset = static_cast<size_t>(other.current - other.storage.begin());
    return offset == otherOffset;
}

auto Iterator::validate(ValidationLevel const& level) const noexcept -> std::optional<Error> {
    switch (level) {
        case ValidationLevel::Basic:
            return this->validateBasic();
        case ValidationLevel::Full:
            return this->validateFull();
        default:
            return std::nullopt;
    }
}

auto Iterator::validateBasic() const noexcept -> std::optional<Error> {
    // Handle empty path
    if (this->path.empty()) {
        return Error{Error::Code::InvalidPath, "Empty path"};
    }

    // Path must start with forward slash
    if (this->path[0] != '/') {
        return Error{Error::Code::InvalidPath, "Path must start with '/'"};
    }

    // Path cannot end with slash unless it's the root path
    if (this->path.size() > 1 && this->path.back() == '/')
        return Error{Error::Code::InvalidPath, "Path ends with slash"};

    return std::nullopt;
}

auto Iterator::validateFull() const noexcept -> std::optional<Error> {
    auto result = validate_path_impl(std::string_view(this->path));
    if (result.code != ValidationError::Code::None) {
        return Error{Error::Code::InvalidPath, get_error_message(result.code)};
    }
    return std::nullopt;
}

bool Iterator::isAtStart() const noexcept {
    auto it = this->storage.begin();
    while (it != this->storage.end() && *it == '/') ++it;
    return this->current == it;
}

auto Iterator::isAtFinalComponent() const noexcept -> bool {
    return segment_end == storage.end();
}

bool Iterator::isAtEnd() const noexcept {
    return this->current == this->storage.end();
}

auto Iterator::skipSlashes(IteratorType& it) noexcept -> void {
    while (it != storage.end() && *it == '/')
        ++it;
}

auto Iterator::findNextSlash(IteratorType it) noexcept -> IteratorType {
    while (it != storage.end() && *it != '/')
        ++it;
    return it;
}

auto Iterator::updateCurrentSegment() noexcept -> void {
    current_segment = path.substr(static_cast<size_t>(current - storage.begin()), static_cast<size_t>(segment_end - current));
}

} // namespace SP
