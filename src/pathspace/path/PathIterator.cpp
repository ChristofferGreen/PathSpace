#include "PathIterator.hpp"

namespace SP {

PathIterator::PathIterator(std::string_view path) noexcept
    : path{path}, current{path.begin()}, segment_end{path.begin()} {
    findNextComponent();
}

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

bool PathIterator::isAtStart() const noexcept {
    return this->current == this->path.begin() + 1; // Skip the leading and mandatory '/'
}

bool PathIterator::isAtEnd() const noexcept {
    // We're at the end if we can't find any more components
    // (current == segment_end means no component found)
    return current == path.end() || current == segment_end;
}

auto PathIterator::fullPath() const noexcept -> std::string_view {
    return path;
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