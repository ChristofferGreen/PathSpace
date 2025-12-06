#pragma once

#include "core/Error.hpp"
#include "core/In.hpp"
#include "core/InsertReturn.hpp"
#include "core/Out.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace SP {
class PathSpace;
}

namespace SP::Distributed {

auto insertTypedPayloadFromBytes(PathSpace&        space,
                                 std::string_view path,
                                 std::string_view type_name,
                                 std::span<const std::byte> bytes,
                                 In const& options = In{}) -> Expected<InsertReturn>;

auto takeTypedPayloadToBytes(PathSpace&        space,
                             std::string_view path,
                             std::string_view type_name,
                             Out const& options) -> Expected<std::vector<std::byte>>;

} // namespace SP::Distributed
