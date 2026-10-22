#pragma once

#include "history/UndoJournalEntry.hpp"

#include "core/Error.hpp"

#include <filesystem>
#include <functional>
#include <span>

namespace SP::History::UndoJournal {

inline constexpr std::uint32_t JournalFileMagic   = 0x50534A46; // 'PSJF'
inline constexpr std::uint16_t JournalFileVersion = 1;

class JournalFileWriter {
public:
    explicit JournalFileWriter(std::filesystem::path path);
    ~JournalFileWriter();

    JournalFileWriter(JournalFileWriter const&)            = delete;
    JournalFileWriter& operator=(JournalFileWriter const&) = delete;
    JournalFileWriter(JournalFileWriter&&) noexcept        = delete;
    JournalFileWriter& operator=(JournalFileWriter&&) noexcept = delete;

    [[nodiscard]] auto open(bool fsyncHeader = false) -> Expected<void>;
    [[nodiscard]] auto append(JournalEntry const& entry, bool fsync = false) -> Expected<void>;
    [[nodiscard]] auto flush() -> Expected<void>;

    [[nodiscard]] auto path() const -> std::filesystem::path const& { return filePath; }

private:
    [[nodiscard]] auto ensureOpened(bool fsyncHeader) -> Expected<void>;
    [[nodiscard]] auto writeHeader(bool fsync) -> Expected<void>;
    [[nodiscard]] auto validateHeader() -> Expected<void>;
    std::filesystem::path filePath;
    std::FILE*            handle = nullptr;
};

struct JournalReplayOptions {
    // An append-only journal whose writer crashed mid-record ends in a partial record. When set,
    // replay stops at that record and succeeds with the entries before it instead of failing; see
    // JournalReplayReport for how much of the file was good.
    bool tolerateTornTail = false;
};

struct JournalReplayReport {
    // Bytes from the start of the file up to the end of the last complete record.
    std::uint64_t validBytes = 0;
    // The file ended inside a record (partial length field or payload).
    bool tornTail = false;
};

// Replays every complete record in order. Without tolerateTornTail an incomplete trailing record
// is an error (MalformedInput); a record length larger than the rest of the file is rejected
// before any memory is allocated for it.
[[nodiscard]] auto replayJournal(
    std::filesystem::path const& path,
    std::function<Expected<void>(JournalEntry&&)> const& onEntry,
    JournalReplayOptions const& options = {},
    JournalReplayReport* report = nullptr) -> Expected<void>;

[[nodiscard]] auto compactJournal(
    std::filesystem::path const& path,
    std::span<JournalEntry const> entries,
    bool fsyncTarget) -> Expected<void>;

} // namespace SP::History::UndoJournal
