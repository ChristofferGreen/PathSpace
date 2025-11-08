#pragma once

#include "history/UndoJournalEntry.hpp"

#include "core/Error.hpp"

#include <filesystem>
#include <functional>
#include <span>

namespace SP::History::UndoJournal {

inline constexpr std::uint32_t kJournalFileMagic   = 0x50534A46; // 'PSJF'
inline constexpr std::uint16_t kJournalFileVersion = 1;

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

[[nodiscard]] auto replayJournal(
    std::filesystem::path const& path,
    std::function<Expected<void>(JournalEntry&&)> const& onEntry) -> Expected<void>;

[[nodiscard]] auto compactJournal(
    std::filesystem::path const& path,
    std::span<JournalEntry const> entries,
    bool fsyncTarget) -> Expected<void>;

} // namespace SP::History::UndoJournal
