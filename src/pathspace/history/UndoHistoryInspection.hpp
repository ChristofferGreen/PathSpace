#pragma once

#include "history/CowSubtreePrototype.hpp"
#include "history/UndoableSpace.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace SP::History::Inspection {

struct DecodedValue {
    std::string path;
    std::string typeName;
    std::string category;
    std::string summary;
    std::size_t bytes = 0;
    std::uint64_t digest = 0;
};

struct SnapshotSummary {
    std::vector<DecodedValue> values;
};

struct ModifiedValue {
    DecodedValue before;
    DecodedValue after;
};

struct SnapshotDiff {
    std::vector<DecodedValue> added;
    std::vector<DecodedValue> removed;
    std::vector<ModifiedValue> modified;
};

auto decodeSnapshot(CowSubtreePrototype::Snapshot const& snapshot,
                    std::string_view rootPath) -> SnapshotSummary;

auto diffSnapshots(CowSubtreePrototype::Snapshot const& baseline,
                   CowSubtreePrototype::Snapshot const& updated,
                   std::string_view rootPath) -> SnapshotDiff;

auto historyStatsToJson(HistoryStats const& stats) -> std::string;
auto lastOperationToJson(std::optional<HistoryLastOperation> const& op) -> std::string;

} // namespace SP::History::Inspection
