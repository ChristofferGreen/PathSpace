#pragma once

#include "core/Error.hpp"
#include "history/UndoHistoryMetadata.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace SP::History::UndoSavefile {

struct OptionsBlock {
    std::size_t        maxEntries        = 0;
    std::size_t        maxBytesRetained  = 0;
    std::size_t        ramCacheEntries   = 0;
    std::size_t        maxDiskBytes      = 0;
    std::uint64_t      keepLatestForMs   = 0;
    bool               manualGarbageCollect = false;
};

struct EntryBlock {
    UndoMetadata::EntryMetadata metadata;
    std::vector<std::byte>      snapshot;
    std::uint64_t               timestampMs = 0;
};

struct Document {
    std::string                     rootPath;
    OptionsBlock                    options;
    UndoMetadata::StateMetadata     stateMetadata;
    EntryBlock                      liveEntry;
    std::vector<EntryBlock>         undoEntries;
    std::vector<EntryBlock>         redoEntries;
};

inline constexpr std::uint32_t SavefileMagic   = 0x50534844; // 'PSHD'
inline constexpr std::uint32_t SavefileVersion = 1;

auto encode(Document const& document) -> std::vector<std::byte>;
auto decode(std::span<const std::byte> data) -> Expected<Document>;

} // namespace SP::History::UndoSavefile

