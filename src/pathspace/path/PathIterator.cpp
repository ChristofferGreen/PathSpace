#include "PathIterator.hpp"

namespace SP {

PathIterator::PathIterator(std::string_view path) noexcept
    : path{path}, current{path.begin()}, segment_end{path.begin()} {
    findNextComponent();
}
PathIterator::PathIterator(char const* const path) noexcept
    : PathIterator{std::string_view{path}} {}

PathIterator::PathIterator(IteratorType first, IteratorType last) noexcept
    : path{first, static_cast<size_t>(last - first)},
      current{path.begin()},
      segment_end{path.begin()} {
    skipSlashes(current);
    segment_end = findNextSlash(current);
    updateCurrentSegment();
}

void PathIterator::findNextComponent() noexcept {
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

auto PathIterator::toString() const noexcept -> std::string {
    return std::string{this->path};
}

auto PathIterator::toStringView() const noexcept -> std::string_view {
    return std::string_view{this->path};
}

auto PathIterator::currentComponent() const noexcept -> std::string_view {
    return this->current_segment;
}

auto PathIterator::next() const noexcept -> PathIterator {
    auto iter = *this;
    ++iter;
    return iter;
}

auto PathIterator::operator*() const noexcept -> value_type {
    return current_segment;
}

auto PathIterator::operator->() const noexcept -> pointer {
    return &current_segment;
}

auto PathIterator::operator++() noexcept -> PathIterator& {
    if (!isAtEnd()) {
        current = segment_end;
        findNextComponent();
    }
    return *this;
}

auto PathIterator::operator++(int) noexcept -> PathIterator {
    PathIterator tmp = *this;
    ++*this;
    return tmp;
}

auto PathIterator::operator==(const PathIterator& other) const noexcept -> bool {
    return current == other.current;
}

auto PathIterator::validate(ValidationLevel const& level) const noexcept -> std::optional<Error> {
    switch (level) {
        case ValidationLevel::Basic:
            return this->validateBasic();
        case ValidationLevel::Full:
            return this->validateFull();
        default:
            return std::nullopt;
    }
}

auto PathIterator::validateBasic() const noexcept -> std::optional<Error> {
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

auto PathIterator::validateFull() const noexcept -> std::optional<Error> {
    auto result = validate_path_impl(std::string_view(this->path));
    if (result.code != ValidationError::Code::None)
        return Error{Error::Code::InvalidPath, get_error_message(result.code)};
    return std::nullopt;
}

bool PathIterator::isAtStart() const noexcept {
    return this->current == this->path.begin() + 1; // Skip the leading and mandatory '/'
}

auto PathIterator::isAtFinalComponent() const noexcept -> bool {
    return segment_end == path.end();
}

bool PathIterator::isAtEnd() const noexcept {
    // We're at the end if we can't find any more components
    // (current == segment_end means no component found)
    return current == path.end() || current == segment_end;
}

auto PathIterator::skipSlashes(IteratorType& it) noexcept -> void {
    while (it != path.end() && *it == '/')
        ++it;
}

auto PathIterator::findNextSlash(IteratorType it) noexcept -> IteratorType {
    while (it != path.end() && *it != '/')
        ++it;
    return it;
}

auto PathIterator::updateCurrentSegment() noexcept -> void {
    current_segment = path.substr(current - path.begin(), segment_end - current);
}

} // namespace SP