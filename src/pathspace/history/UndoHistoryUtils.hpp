#pragma once

#include "core/Error.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace SP::History::UndoUtils {

[[nodiscard]] auto toMillis(std::chrono::system_clock::time_point tp) -> std::uint64_t;
[[nodiscard]] auto fromMillis(std::uint64_t millis)
    -> std::chrono::system_clock::time_point;
[[nodiscard]] auto generateSpaceUuid() -> std::string;

[[nodiscard]] auto fsyncFileDescriptor(int fd) -> Expected<void>;
[[nodiscard]] auto fsyncDirectory(std::filesystem::path const& dir) -> Expected<void>;

[[nodiscard]] auto writeFileAtomic(std::filesystem::path const& path,
                                   std::span<const std::byte> data,
                                   bool fsyncData,
                                   bool binary) -> Expected<void>;
[[nodiscard]] auto writeTextFileAtomic(std::filesystem::path const& path,
                                       std::string const& text,
                                       bool fsyncData) -> Expected<void>;

[[nodiscard]] auto readBinaryFile(std::filesystem::path const& path) -> Expected<std::vector<std::byte>>;
[[nodiscard]] auto readTextFile(std::filesystem::path const& path) -> Expected<std::string>;

void removePathIfExists(std::filesystem::path const& path);
[[nodiscard]] auto fileSizeOrZero(std::filesystem::path const& path) -> std::uintmax_t;

inline constexpr std::size_t  MaxUnsupportedLogEntries = 16;

inline constexpr std::string_view UnsupportedNestedMessage =
    "History does not yet support nested PathSpaces";
inline constexpr std::string_view UnsupportedExecutionMessage =
    "History does not yet support nodes containing tasks or futures";
inline constexpr std::string_view UnsupportedSerializationMessage =
    "Unable to serialize node payload for history";

namespace Paths {
inline constexpr std::string_view HistoryRoot                     = "_history";
inline constexpr std::string_view HistoryStats                    = "_history/stats";
inline constexpr std::string_view HistoryStatsUndoCount           = "_history/stats/undoCount";
inline constexpr std::string_view HistoryStatsRedoCount           = "_history/stats/redoCount";
inline constexpr std::string_view HistoryStatsUndoBytes           = "_history/stats/undoBytes";
inline constexpr std::string_view HistoryStatsRedoBytes           = "_history/stats/redoBytes";
inline constexpr std::string_view HistoryStatsLiveBytes           = "_history/stats/liveBytes";
inline constexpr std::string_view HistoryStatsBytesRetained       = "_history/stats/bytesRetained";
inline constexpr std::string_view HistoryStatsManualGcEnabled     = "_history/stats/manualGcEnabled";
inline constexpr std::string_view HistoryStatsLimits              = "_history/stats/limits";
inline constexpr std::string_view HistoryStatsLimitsMaxEntries    = "_history/stats/limits/maxEntries";
inline constexpr std::string_view HistoryStatsLimitsMaxBytesRetained =
    "_history/stats/limits/maxBytesRetained";
inline constexpr std::string_view HistoryStatsLimitsKeepLatestForMs =
    "_history/stats/limits/keepLatestForMs";
inline constexpr std::string_view HistoryStatsLimitsRamCacheEntries =
    "_history/stats/limits/ramCacheEntries";
inline constexpr std::string_view HistoryStatsLimitsMaxDiskBytes =
    "_history/stats/limits/maxDiskBytes";
inline constexpr std::string_view HistoryStatsLimitsPersistHistory =
    "_history/stats/limits/persistHistory";
inline constexpr std::string_view HistoryStatsLimitsRestoreFromPersistence =
    "_history/stats/limits/restoreFromPersistence";
inline constexpr std::string_view HistoryStatsTrimOperationCount  = "_history/stats/trimOperationCount";
inline constexpr std::string_view HistoryStatsTrimmedEntries      = "_history/stats/trimmedEntries";
inline constexpr std::string_view HistoryStatsTrimmedBytes        = "_history/stats/trimmedBytes";
inline constexpr std::string_view HistoryStatsLastTrimTimestamp   = "_history/stats/lastTrimTimestampMs";
inline constexpr std::string_view HistoryStatsCompactionPrefix    = "_history/stats/compaction";
inline constexpr std::string_view HistoryStatsCompactionRuns      = "_history/stats/compaction/runs";
inline constexpr std::string_view HistoryStatsCompactionEntries   =
    "_history/stats/compaction/entries";
inline constexpr std::string_view HistoryStatsCompactionBytes     = "_history/stats/compaction/bytes";
inline constexpr std::string_view HistoryStatsCompactionLastTimestamp =
    "_history/stats/compaction/lastTimestampMs";
inline constexpr std::string_view HistoryHeadGeneration           = "_history/head/generation";

inline constexpr std::string_view HistoryLastOperationPrefix      = "_history/lastOperation";
inline constexpr std::string_view HistoryLastOperationType        = "_history/lastOperation/type";
inline constexpr std::string_view HistoryLastOperationTimestamp   = "_history/lastOperation/timestampMs";
inline constexpr std::string_view HistoryLastOperationDuration    = "_history/lastOperation/durationMs";
inline constexpr std::string_view HistoryLastOperationSuccess     = "_history/lastOperation/success";
inline constexpr std::string_view HistoryLastOperationUndoBefore  = "_history/lastOperation/undoCountBefore";
inline constexpr std::string_view HistoryLastOperationUndoAfter   = "_history/lastOperation/undoCountAfter";
inline constexpr std::string_view HistoryLastOperationRedoBefore  = "_history/lastOperation/redoCountBefore";
inline constexpr std::string_view HistoryLastOperationRedoAfter   = "_history/lastOperation/redoCountAfter";
inline constexpr std::string_view HistoryLastOperationBytesBefore = "_history/lastOperation/bytesBefore";
inline constexpr std::string_view HistoryLastOperationBytesAfter  = "_history/lastOperation/bytesAfter";
inline constexpr std::string_view HistoryLastOperationMessage     = "_history/lastOperation/message";
inline constexpr std::string_view HistoryLastOperationTag         = "_history/lastOperation/tag";

inline constexpr std::string_view HistoryUnsupported              = "_history/unsupported";
inline constexpr std::string_view HistoryUnsupportedTotalCount    = "_history/unsupported/totalCount";
inline constexpr std::string_view HistoryUnsupportedRecentCount   = "_history/unsupported/recentCount";
inline constexpr std::string_view HistoryUnsupportedRecentPrefix  = "_history/unsupported/recent/";

inline constexpr std::string_view HistoryDiagnosticsRoot        = "diagnostics/history";
inline constexpr std::string_view HistoryDiagnosticsCompatRoot  = "output/v1/diagnostics/history";
inline constexpr std::string_view HistoryDiagnosticsHeadSequence = "head/sequence";
inline constexpr std::string_view HistoryDiagnosticsEntriesPrefix = "entries/";
inline constexpr std::string_view HistoryDiagnosticsEntryPath     = "path";
inline constexpr std::string_view HistoryDiagnosticsEntryTag      = "tag";
inline constexpr std::string_view HistoryDiagnosticsEntryOperation = "operation";
inline constexpr std::string_view HistoryDiagnosticsEntryTimestamp = "timestampMs";
inline constexpr std::string_view HistoryDiagnosticsEntryMonotonic = "monotonicNs";
inline constexpr std::string_view HistoryDiagnosticsEntrySequence  = "sequence";
inline constexpr std::string_view HistoryDiagnosticsEntryBarrier   = "barrier";
inline constexpr std::string_view HistoryDiagnosticsEntryValueBytes = "valueBytes";
inline constexpr std::string_view HistoryDiagnosticsEntryInverseBytes = "inverseBytes";
inline constexpr std::string_view HistoryDiagnosticsEntryHasValue   = "hasValue";
inline constexpr std::string_view HistoryDiagnosticsEntryHasInverse = "hasInverse";

inline constexpr std::string_view CommandUndo              = "_history/undo";
inline constexpr std::string_view CommandRedo              = "_history/redo";
inline constexpr std::string_view CommandGarbageCollect    = "_history/garbage_collect";
inline constexpr std::string_view CommandSetManualGc       = "_history/set_manual_garbage_collect";
inline constexpr std::string_view CommandSetTag            = "_history/set_tag";
} // namespace Paths

} // namespace SP::History::UndoUtils
