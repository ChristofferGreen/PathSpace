#pragma once

#include "core/Error.hpp"
#include "history/CowSubtreePrototype.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace SP::History::UndoSnapshotCodec {

auto encodeSnapshot(CowSubtreePrototype::Snapshot const& snapshot)
    -> Expected<std::vector<std::byte>>;

auto decodeSnapshot(CowSubtreePrototype& prototype, std::span<const std::byte> data)
    -> Expected<CowSubtreePrototype::Snapshot>;

auto snapshotFileStem(std::size_t generation) -> std::string;

auto loadSnapshotFromFile(CowSubtreePrototype& prototype,
                          std::filesystem::path const& path)
    -> Expected<CowSubtreePrototype::Snapshot>;

} // namespace SP::History::UndoSnapshotCodec
