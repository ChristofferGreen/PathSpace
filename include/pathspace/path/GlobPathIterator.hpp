#pragma once
#include "GlobName.hpp"

#include <string>
#include <string_view>

namespace SP {

template<typename T>
struct GlobPathIterator {
    using SIterator = T::const_iterator;
    GlobPathIterator(SIterator const &iter, SIterator const &endIter);

    auto operator++() -> GlobPathIterator&;
    auto operator==(GlobPathIterator const &other) const -> bool;
    auto operator*() const ->  GlobName;
    auto operator->() const -> GlobName;
private:
    auto skipSlashes() -> void;
    SIterator current;
    SIterator end;
};

}