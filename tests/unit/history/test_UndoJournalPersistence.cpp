#include "history/UndoJournalPersistence.hpp"
#include "history/UndoJournalState.hpp"

#include "third_party/doctest.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <string>
#include <vector>

using namespace SP::History::UndoJournal;
using SP::Expected;

namespace {

auto makeEntry(int seq) -> JournalEntry {
    JournalEntry entry;
    entry.operation    = OperationKind::Insert;
    entry.path         = "/doc/value";
    entry.sequence     = static_cast<std::uint64_t>(seq);
    entry.timestampMs  = static_cast<std::uint64_t>(1000 + seq);
    entry.monotonicNs  = static_cast<std::uint64_t>(seq * 10);
    entry.barrier      = false;
    entry.value.present = false;
    entry.inverseValue.present = false;
    return entry;
}

auto tempPath(std::string_view suffix) -> std::filesystem::path {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    auto name = "undo_journal_test_" + std::to_string(::getpid()) + "_" + std::to_string(now);
    auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(dir);
    return dir / suffix;
}

auto writeHeader(std::filesystem::path const& path, std::uint32_t magic, std::uint16_t version) -> void {
    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    REQUIRE(file != nullptr);
    auto guard = std::unique_ptr<std::FILE, decltype(&std::fclose)>{file, &std::fclose};
    std::uint32_t reserved = 0;
    REQUIRE(std::fwrite(&magic, sizeof(magic), 1, file) == 1);
    REQUIRE(std::fwrite(&version, sizeof(version), 1, file) == 1);
    REQUIRE(std::fwrite(&reserved, sizeof(reserved), 1, file) == 1);
}

} // namespace

TEST_SUITE("history.journal.persistence") {
    TEST_CASE("append and replay journal entries across reopen") {
        auto path = tempPath("journal1.log");

        {
            JournalFileWriter writer(path);
            REQUIRE(writer.open(true));
            CHECK(writer.append(makeEntry(1)).has_value());
            CHECK(writer.append(makeEntry(2)).has_value());
            CHECK(writer.flush().has_value());
        }

        {
            JournalFileWriter writer(path);
            REQUIRE(writer.open(false));
            CHECK(writer.append(makeEntry(3)).has_value());
        }

        std::vector<std::uint64_t> sequences;
        auto replay = replayJournal(path, [&](JournalEntry&& entry) -> Expected<void> {
            sequences.push_back(entry.sequence);
            return {};
        });
        REQUIRE(replay.has_value());

        std::vector<std::uint64_t> expected{1, 2, 3};
        CHECK(sequences == expected);
    }

    TEST_CASE("compact journal rewrites provided entries") {
        auto path = tempPath("journal2.log");

        JournalFileWriter writer(path);
        REQUIRE(writer.open(false));
        CHECK(writer.append(makeEntry(1)).has_value());
        CHECK(writer.append(makeEntry(2)).has_value());
        CHECK(writer.append(makeEntry(3)).has_value());

        std::vector<JournalEntry> retained;
        retained.push_back(makeEntry(2));
        retained.push_back(makeEntry(3));

        REQUIRE(compactJournal(path, retained, false));

        std::vector<std::uint64_t> sequences;
        auto replay = replayJournal(path, [&](JournalEntry&& entry) -> Expected<void> {
            sequences.push_back(entry.sequence);
            return {};
        });
        REQUIRE(replay.has_value());
        std::vector<std::uint64_t> expected{2, 3};
        CHECK(sequences == expected);
    }

    TEST_CASE("replay detects truncation") {
        auto path = tempPath("journal3.log");
        {
            JournalFileWriter writer(path);
            REQUIRE(writer.open(false));
            CHECK(writer.append(makeEntry(1)).has_value());
        }

        auto size = std::filesystem::file_size(path);
        REQUIRE(size > 4);
        std::filesystem::resize_file(path, size - 2);

        auto replay = replayJournal(path, [&](JournalEntry&&) -> Expected<void> {
            return {};
        });
        CHECK_FALSE(replay.has_value());
        CHECK(replay.error().code == SP::Error::Code::MalformedInput);
    }

    TEST_CASE("open fails when target path is an existing directory") {
        auto path = tempPath("journal_dir");
        std::filesystem::create_directories(path);

        JournalFileWriter writer(path);
        bool threw = false;
        try {
            auto res = writer.open(false);
            (void)res;
        } catch (std::filesystem::filesystem_error const&) {
            threw = true;
        }
        CHECK(threw);
    }

    TEST_CASE("open rejects corrupt journal headers") {
        auto path = tempPath("corrupt.log");

        SUBCASE("bad magic") {
            writeHeader(path, JournalFileMagic + 1, JournalFileVersion);
        }

        SUBCASE("bad version") {
            writeHeader(path, JournalFileMagic, static_cast<std::uint16_t>(JournalFileVersion + 1));
        }

        JournalFileWriter writer(path);
        auto result = writer.open(false);
        CHECK_FALSE(result.has_value());
        CHECK(result.error().code == SP::Error::Code::MalformedInput);
    }

    TEST_CASE("replay surfaces missing file and truncated headers") {
        auto missingPath = tempPath("missing.log");
        auto missing = replayJournal(missingPath, [&](JournalEntry&&) -> Expected<void> {
            return {};
        });
        CHECK_FALSE(missing.has_value());
        CHECK(missing.error().code == SP::Error::Code::NotFound);

        auto truncatedPath = tempPath("truncated.log");
        {
            std::FILE* file = std::fopen(truncatedPath.string().c_str(), "wb");
            REQUIRE(file != nullptr);
            std::uint32_t magic = JournalFileMagic;
            REQUIRE(std::fwrite(&magic, sizeof(magic), 1, file) == 1);
            std::fclose(file);
        }

        auto truncated = replayJournal(truncatedPath, [&](JournalEntry&&) -> Expected<void> {
            return {};
        });
        CHECK_FALSE(truncated.has_value());
        CHECK(truncated.error().code == SP::Error::Code::MalformedInput);
    }

    TEST_CASE("compact journal fsync path produces a validated log") {
        auto path = tempPath("journal_fsync.log");

        std::vector<JournalEntry> entries;
        entries.push_back(makeEntry(5));
        entries.push_back(makeEntry(6));

        auto compact = compactJournal(path, entries, true);
        REQUIRE(compact.has_value());

        std::vector<std::uint64_t> sequences;
        auto replay = replayJournal(path, [&](JournalEntry&& entry) -> Expected<void> {
            sequences.push_back(entry.sequence);
            return {};
        });
        REQUIRE(replay.has_value());
        CHECK(sequences == std::vector<std::uint64_t>{5, 6});
    }
}
