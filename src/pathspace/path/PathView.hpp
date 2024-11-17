#pragma once
#include "path/ConcretePathIterator.hpp"
#include "path/GlobPathIterator.hpp"

#include <iterator>
#include <ranges>

namespace SP {

struct PathViewGlob {
    PathViewGlob(GlobPathIteratorStringView begin, GlobPathIteratorStringView end) : current_(begin), end_(end) {}

    auto currentComponent() const {
        return *current_;
    }
    auto isFinalComponent() const {
        return std::next(current_) == end_;
    }

    auto advance() -> PathViewGlob& {
        ++current_;
        return *this;
    }

    auto next() const -> PathViewGlob {
        PathViewGlob p = *this;
        return p.advance();
    }

    auto current() const {
        return current_;
    }

    auto end() const {
        return end_;
    }

private:
    GlobPathIteratorStringView current_;
    GlobPathIteratorStringView end_;
};

struct PathViewConcrete {
    PathViewConcrete(ConcretePathIteratorStringView begin, ConcretePathIteratorStringView end) : current_(begin), end_(end) {}

    auto currentComponent() const {
        return *current_;
    }
    auto isFinalComponent() const {
        return std::next(current_) == end_;
    }

    auto advance() -> PathViewConcrete& {
        ++current_;
        return *this;
    }

    auto next() const -> PathViewConcrete {
        PathViewConcrete p = *this;
        return p.advance();
    }

    auto current() const {
        return current_;
    }

    auto end() const {
        return end_;
    }

private:
    ConcretePathIteratorStringView current_;
    ConcretePathIteratorStringView end_;
};

} // namespace SP