#include "history/UndoJournalPersistence.hpp"
#include "history/UndoJournalState.hpp"

#include "third_party/doctest.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
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

} // namespace

TEST_SUITE("UndoJournalPersistence") {
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
}
