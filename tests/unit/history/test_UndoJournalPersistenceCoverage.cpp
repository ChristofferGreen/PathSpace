#define private public
#include "history/UndoJournalPersistence.hpp"
#undef private
#include "history/UndoJournalState.hpp"

#include "third_party/doctest.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace SP::History::UndoJournal;
using SP::Expected;

namespace {

auto makeEntry(int seq) -> JournalEntry {
    JournalEntry entry;
    entry.operation     = OperationKind::Insert;
    entry.path          = "/doc/value";
    entry.sequence      = static_cast<std::uint64_t>(seq);
    entry.timestampMs   = static_cast<std::uint64_t>(1000 + seq);
    entry.monotonicNs   = static_cast<std::uint64_t>(seq * 10);
    entry.barrier       = false;
    entry.value.present = false;
    entry.inverseValue.present = false;
    return entry;
}

auto tempPath(std::string_view suffix) -> std::filesystem::path {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto name = "undo_journal_cov_" + std::to_string(::getpid()) + "_" + std::to_string(now);
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(dir);
    return dir / suffix;
}

auto writeHeader(std::filesystem::path const& path,
                 std::uint32_t magic,
                 std::uint16_t version,
                 std::uint32_t reserved = 0u) -> void {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    REQUIRE(std::fwrite(&magic, sizeof(magic), 1, file) == 1);
    REQUIRE(std::fwrite(&version, sizeof(version), 1, file) == 1);
    REQUIRE(std::fwrite(&reserved, sizeof(reserved), 1, file) == 1);
    std::fclose(file);
}

auto appendEntryPayload(std::filesystem::path const& path,
                        std::uint32_t length,
                        std::vector<std::byte> const& payload) -> void {
    std::FILE* file = std::fopen(path.string().c_str(), "ab");
    REQUIRE(file != nullptr);
    REQUIRE(std::fwrite(&length, sizeof(length), 1, file) == 1);
    if (!payload.empty()) {
        REQUIRE(std::fwrite(payload.data(), 1, payload.size(), file) == payload.size());
    }
    std::fclose(file);
}

} // namespace

TEST_SUITE("history.journal.persistence.coverage") {
    TEST_CASE("flush without open is a no-op") {
        auto path = tempPath("noop.log");
        JournalFileWriter writer(path);
        auto result = writer.flush();
        CHECK(result.has_value());
    }

    TEST_CASE("open fails when parent path is a file") {
        auto parent = tempPath("parent_file");
        {
            std::FILE* file = std::fopen(parent.string().c_str(), "wb");
            REQUIRE(file != nullptr);
            std::fclose(file);
        }
        auto path = parent / "child.log";
        JournalFileWriter writer(path);
        auto result = writer.open(false);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::UnknownError);
        CHECK(result.error().message->find("Failed to create journal directory") != std::string::npos);
    }

    TEST_CASE("replayJournal propagates callback errors") {
        auto path = tempPath("callback.log");
        {
            JournalFileWriter writer(path);
            REQUIRE(writer.open(false));
            REQUIRE(writer.append(makeEntry(1)).has_value());
        }

        auto replay = replayJournal(path, [&](JournalEntry&&) -> Expected<void> {
            return std::unexpected(SP::Error{SP::Error::Code::UnknownError, "callback error"});
        });
        CHECK_FALSE(replay.has_value());
        CHECK(replay.error().code == SP::Error::Code::UnknownError);
    }

    TEST_CASE("compactJournal reports directory creation errors") {
        auto parent = tempPath("compact_parent_file");
        {
            std::FILE* file = std::fopen(parent.string().c_str(), "wb");
            REQUIRE(file != nullptr);
            std::fclose(file);
        }
        auto target = parent / "journal.log";
        std::vector<JournalEntry> entries{makeEntry(1)};
        auto result = compactJournal(target, entries, false);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::UnknownError);
        CHECK(result.error().message->find("Failed to create journal directory") != std::string::npos);
    }

    TEST_CASE("replayJournal reports missing file") {
        auto path = tempPath("missing.log");
        auto result = replayJournal(path, [](JournalEntry&&) -> Expected<void> { return {}; });
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::NotFound);
    }

    TEST_CASE("replayJournal reports open errors for directories") {
        auto path = tempPath("replay_dir");
        std::filesystem::create_directories(path);

        auto result = replayJournal(path, [](JournalEntry&&) -> Expected<void> { return {}; });
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::UnknownError);
    }

#ifndef _WIN32
    TEST_CASE("replayJournal reports open errors for unreadable files") {
        auto path = tempPath("replay_unreadable.log");
        {
            std::FILE* file = std::fopen(path.string().c_str(), "wb");
            REQUIRE(file != nullptr);
            std::fclose(file);
        }

        auto originalPerms = std::filesystem::status(path).permissions();
        auto noReadPerms = originalPerms
                           & ~std::filesystem::perms::owner_read
                           & ~std::filesystem::perms::group_read
                           & ~std::filesystem::perms::others_read;
        std::error_code ec;
        std::filesystem::permissions(path, noReadPerms, ec);
        REQUIRE_FALSE(ec);

        auto result = replayJournal(path, [](JournalEntry&&) -> Expected<void> { return {}; });
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::UnknownError);

        std::filesystem::permissions(path, originalPerms, ec);
        CHECK_FALSE(ec);
    }
#endif

    TEST_CASE("replayJournal rejects invalid header magic") {
        auto path = tempPath("bad_magic.log");
        writeHeader(path, JournalFileMagic + 1u, JournalFileVersion);

        auto result = replayJournal(path, [](JournalEntry&&) -> Expected<void> { return {}; });
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::MalformedInput);
    }

    TEST_CASE("replayJournal rejects unsupported header version") {
        auto path = tempPath("bad_version.log");
        writeHeader(path, JournalFileMagic, static_cast<std::uint16_t>(JournalFileVersion + 1u));

        auto result = replayJournal(path, [](JournalEntry&&) -> Expected<void> { return {}; });
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::MalformedInput);
    }

    TEST_CASE("replayJournal reports truncated entry payload") {
        auto path = tempPath("truncated_payload.log");
        writeHeader(path, JournalFileMagic, JournalFileVersion);

        std::vector<std::byte> payload{std::byte{0xAB}, std::byte{0xCD}};
        appendEntryPayload(path, 4u, payload);

        auto result = replayJournal(path, [](JournalEntry&&) -> Expected<void> { return {}; });
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::MalformedInput);
        CHECK(result.error().message->find("Truncated journal entry payload") != std::string::npos);
    }

    TEST_CASE("replayJournal rejects a record length larger than the file without allocating it") {
        auto path = tempPath("huge_length.log");
        writeHeader(path, JournalFileMagic, JournalFileVersion);
        appendEntryPayload(path, 0xFFFFFFFFu, std::vector<std::byte>(8, std::byte{0}));

        auto strict = replayJournal(path, [](JournalEntry&&) -> Expected<void> { return {}; });
        REQUIRE_FALSE(strict.has_value());
        CHECK(strict.error().code == SP::Error::Code::MalformedInput);

        JournalReplayReport report;
        auto tolerant = replayJournal(path,
                                      [](JournalEntry&&) -> Expected<void> { return {}; },
                                      JournalReplayOptions{.tolerateTornTail = true},
                                      &report);
        CHECK(tolerant.has_value());
        CHECK(report.tornTail);
        CHECK(report.validBytes == 10); // only the 10-byte header (magic, version, reserved) is good
    }

    TEST_CASE("replayJournal recovers the entries before a torn last record") {
        auto path = tempPath("torn_tail.log");
        std::uint64_t sizeAfterFirst  = 0;
        std::uint64_t sizeAfterSecond = 0;
        {
            JournalFileWriter writer(path);
            REQUIRE(writer.append(makeEntry(1), false).has_value());
            REQUIRE(writer.flush().has_value());
            sizeAfterFirst = std::filesystem::file_size(path);
            REQUIRE(writer.append(makeEntry(2), false).has_value());
            REQUIRE(writer.flush().has_value());
            sizeAfterSecond = std::filesystem::file_size(path);
        }
        REQUIRE(sizeAfterSecond > sizeAfterFirst + 3);
        std::filesystem::resize_file(path, sizeAfterSecond - 3); // crash in the middle of record 2

        auto strict = replayJournal(path, [](JournalEntry&&) -> Expected<void> { return {}; });
        CHECK_FALSE(strict.has_value());

        std::vector<std::uint64_t> sequences;
        JournalReplayReport        report;
        auto                       tolerant = replayJournal(
            path,
            [&](JournalEntry&& entry) -> Expected<void> {
                sequences.push_back(entry.sequence);
                return {};
            },
            JournalReplayOptions{.tolerateTornTail = true},
            &report);
        REQUIRE(tolerant.has_value());
        CHECK(sequences == std::vector<std::uint64_t>{1});
        CHECK(report.tornTail);
        CHECK(report.validBytes == sizeAfterFirst);
    }

    TEST_CASE("replayJournal reports a partial length field as a torn tail") {
        auto path = tempPath("partial_length.log");
        std::uint64_t sizeAfterFirst = 0;
        {
            JournalFileWriter writer(path);
            REQUIRE(writer.append(makeEntry(1), false).has_value());
            REQUIRE(writer.flush().has_value());
            sizeAfterFirst = std::filesystem::file_size(path);
        }
        {
            std::FILE* file = std::fopen(path.string().c_str(), "ab");
            REQUIRE(file != nullptr);
            std::uint8_t const partial[2] = {0x10, 0x00};
            REQUIRE(std::fwrite(partial, 1, sizeof(partial), file) == sizeof(partial));
            std::fclose(file);
        }

        std::size_t        count = 0;
        JournalReplayReport report;
        auto               result = replayJournal(
            path,
            [&](JournalEntry&&) -> Expected<void> {
                ++count;
                return {};
            },
            JournalReplayOptions{},
            &report);
        CHECK(result.has_value());
        CHECK(count == 1);
        CHECK(report.tornTail);
        CHECK(report.validBytes == sizeAfterFirst);
    }

    TEST_CASE("replayJournal reports a clean file as fully valid") {
        auto path = tempPath("clean.log");
        {
            JournalFileWriter writer(path);
            REQUIRE(writer.append(makeEntry(1), false).has_value());
            REQUIRE(writer.append(makeEntry(2), false).has_value());
            REQUIRE(writer.flush().has_value());
        }

        JournalReplayReport report;
        auto                result = replayJournal(path,
                                    [](JournalEntry&&) -> Expected<void> { return {}; },
                                    JournalReplayOptions{.tolerateTornTail = true},
                                    &report);
        CHECK(result.has_value());
        CHECK_FALSE(report.tornTail);
        CHECK(report.validBytes == std::filesystem::file_size(path));
    }

    TEST_CASE("replayJournal surfaces entry decode failures") {
        auto path = tempPath("bad_entry.log");
        writeHeader(path, JournalFileMagic, JournalFileVersion);

        appendEntryPayload(path, 0u, {});

        auto result = replayJournal(path, [](JournalEntry&&) -> Expected<void> { return {}; });
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::MalformedInput);
        CHECK(result.error().message->find("Journal entry missing magic header") != std::string::npos);
    }

    TEST_CASE("JournalFileWriter open rejects invalid on-disk header") {
        auto path = tempPath("invalid_header.log");
        writeHeader(path, JournalFileMagic + 1u, JournalFileVersion);

        JournalFileWriter writer(path);
        auto result = writer.open(false);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::MalformedInput);
        CHECK(result.error().message->find("magic mismatch") != std::string::npos);
    }

    TEST_CASE("JournalFileWriter open reports write failures for readonly directories") {
        auto dir = tempPath("readonly_dir");
        std::filesystem::create_directories(dir);
        std::filesystem::permissions(dir,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
                                     std::filesystem::perm_options::replace);

        auto path = dir / "journal.log";
        JournalFileWriter writer(path);
        auto result = writer.open(false);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::UnknownError);
        CHECK(result.error().message->find("Failed to create journal file") != std::string::npos);

        std::filesystem::permissions(dir,
                                     std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    TEST_CASE("JournalFileWriter append reports write failures on read-only handle") {
        auto path = tempPath("readonly_handle.log");
        writeHeader(path, JournalFileMagic, JournalFileVersion);

        JournalFileWriter writer(path);
        writer.handle = std::fopen(path.string().c_str(), "rb");
        REQUIRE(writer.handle != nullptr);

        auto result = writer.append(makeEntry(1));
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::UnknownError);

        std::fclose(writer.handle);
        writer.handle = nullptr;
    }
}
