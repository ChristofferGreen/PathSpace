#include "Iterator.hpp"

namespace SP {

Iterator::Iterator(std::string_view path) noexcept
    : path{path}, current{path.begin()}, segment_end{path.begin()} {
    findNextComponent();
}
Iterator::Iterator(char const* const path) noexcept
    : Iterator{std::string_view{path}} {}

Iterator::Iterator(IteratorType first, IteratorType last) noexcept
    : path{first, static_cast<size_t>(last - first)},
      current{path.begin()},
      segment_end{path.begin()} {
    skipSlashes(current);
    segment_end = findNextSlash(current);
    updateCurrentSegment();
}

void Iterator::findNextComponent() noexcept {
    // Skip any leading separators
    while (current != path.end() && *current == '/') {
        ++current;
    }

    // Find end of component (next separator or end)
    segment_end = current;
    while (segment_end != path.end() && *segment_end != '/') {
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
    return current == other.current;
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
    if (result.code != ValidationError::Code::None)
        return Error{Error::Code::InvalidPath, get_error_message(result.code)};
    return std::nullopt;
}

bool Iterator::isAtStart() const noexcept {
    return this->current == this->path.begin() + 1; // Skip the leading and mandatory '/'
}

auto Iterator::isAtFinalComponent() const noexcept -> bool {
    return segment_end == path.end();
}

bool Iterator::isAtEnd() const noexcept {
    // We're at the end if we can't find any more components
    // (current == segment_end means no component found)
    return current == path.end() || current == segment_end;
}

auto Iterator::skipSlashes(IteratorType& it) noexcept -> void {
    while (it != path.end() && *it == '/')
        ++it;
}

auto Iterator::findNextSlash(IteratorType it) noexcept -> IteratorType {
    while (it != path.end() && *it != '/')
        ++it;
    return it;
}

auto Iterator::updateCurrentSegment() noexcept -> void {
    current_segment = path.substr(current - path.begin(), segment_end - current);
}

} // namespace SP