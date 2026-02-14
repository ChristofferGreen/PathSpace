#include "history/UndoJournalPersistence.hpp"
#include "history/UndoJournalState.hpp"

#include "third_party/doctest.h"

#include <chrono>
#include <cstdint>
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
}
