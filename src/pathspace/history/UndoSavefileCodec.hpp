#pragma once

#include "core/Error.hpp"
#include "history/UndoJournalEntry.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace SP::History::UndoSavefile {

struct OptionsBlock {
    std::size_t   maxEntries           = 0;
    std::size_t   maxBytesRetained     = 0;
    std::size_t   maxDiskBytes         = 0;
    std::uint64_t keepLatestForMs      = 0;
    bool          manualGarbageCollect = false;
};

struct Document {
    std::string                         rootPath;
    OptionsBlock                        options;
    std::uint64_t                       nextSequence = 0;
    std::size_t                         undoCount    = 0;
    std::vector<UndoJournal::JournalEntry> entries;
};

inline constexpr std::uint32_t SavefileMagic   = 0x504A4E4C; // 'PJNL'
inline constexpr std::uint32_t SavefileVersion = 1;

auto encode(Document const& document) -> Expected<std::vector<std::byte>>;
auto decode(std::span<const std::byte> data) -> Expected<Document>;

} // namespace SP::History::UndoSavefile
