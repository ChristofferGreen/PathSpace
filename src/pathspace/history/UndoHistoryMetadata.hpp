#pragma once

#include "core/Error.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SP::History::UndoMetadata {

struct EntryMetadata {
    std::size_t  generation = 0;
    std::size_t  bytes      = 0;
    std::uint64_t timestampMs = 0;
};

struct StateMetadata {
    std::size_t              liveGeneration = 0;
    std::vector<std::size_t> undoGenerations;
    std::vector<std::size_t> redoGenerations;
    bool                     manualGc       = false;
    std::size_t              ramCacheEntries = 0;
};

[[nodiscard]] auto encodeEntryMeta(EntryMetadata const& meta) -> std::vector<std::byte>;
[[nodiscard]] auto parseEntryMeta(std::span<const std::byte> data) -> Expected<EntryMetadata>;

[[nodiscard]] auto encodeStateMeta(StateMetadata const& meta) -> std::vector<std::byte>;
[[nodiscard]] auto parseStateMeta(std::span<const std::byte> data) -> Expected<StateMetadata>;

} // namespace SP::History::UndoMetadata
