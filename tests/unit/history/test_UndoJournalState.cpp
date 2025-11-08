#include "history/UndoJournalEntry.hpp"
#include "history/UndoJournalState.hpp"

#include "third_party/doctest.h"

#include <cstdint>
#include <span>
#include <string>

using namespace SP::History::UndoJournal;

namespace {

auto makeEntry(int seq, std::string pathSuffix = {}) -> JournalEntry {
    JournalEntry entry;
    entry.operation    = OperationKind::Insert;
    entry.path         = "/doc/" + pathSuffix;
    entry.timestampMs  = static_cast<std::uint64_t>(1000 + seq);
    entry.monotonicNs  = static_cast<std::uint64_t>(seq * 10);
    entry.sequence     = static_cast<std::uint64_t>(seq);
    entry.barrier      = false;
    entry.value.present = false;
    entry.inverseValue.present = false;
    return entry;
}

auto makeSizedEntry(int seq, std::size_t payloadBytes) -> JournalEntry {
    JournalEntry entry = makeEntry(seq, "large");
    entry.value.present = true;
    entry.value.bytes.resize(payloadBytes);
    entry.inverseValue.present = false;
    return entry;
}

} // namespace

TEST_SUITE("UndoJournalState") {
    TEST_CASE("append tracks undo and redo cursors") {
        JournalState state;
        auto e1 = makeEntry(1, "a");
        auto e2 = makeEntry(2, "b");

        state.append(e1);
        state.append(e2);

        CHECK(state.size() == 2);
        CHECK(state.canUndo());
        CHECK_FALSE(state.canRedo());

        auto undone = state.undo();
        REQUIRE(undone.has_value());
        CHECK(undone->get().sequence == 2);
        CHECK(state.canRedo());

        auto redone = state.redo();
        REQUIRE(redone.has_value());
        CHECK(redone->get().sequence == 2);
        CHECK_FALSE(state.canRedo());
    }

    TEST_CASE("append clears redo tail") {
        JournalState state;
        state.append(makeEntry(1, "a"));
        state.append(makeEntry(2, "b"));
        CHECK(state.size() == 2);

        auto undone = state.undo();
        REQUIRE(undone.has_value());
        CHECK(state.canRedo());

        state.append(makeEntry(3, "c"));
        CHECK_FALSE(state.canRedo());
        CHECK(state.size() == 2); // one redo entry dropped, new entry appended
        auto peekUndo = state.peekUndo();
        REQUIRE(peekUndo.has_value());
        CHECK(peekUndo->get().sequence == 3);
    }

    TEST_CASE("retention trims oldest entries by count") {
        JournalState::RetentionPolicy policy;
        policy.maxEntries = 2;
        JournalState state(policy);

        state.append(makeEntry(1, "a"));
        state.append(makeEntry(2, "b"));
        state.append(makeEntry(3, "c"));

        CHECK(state.size() == 2);
        CHECK(state.entryAt(0).sequence == 2);
        CHECK(state.entryAt(1).sequence == 3);

        auto stats = state.stats();
        CHECK(stats.trimmedEntries == 1);
        CHECK(stats.undoCount == 2);
        CHECK(stats.redoCount == 0);
    }

    TEST_CASE("retention trims by byte budget") {
        JournalState::RetentionPolicy policy;
        policy.maxBytes = 64;
        JournalState state(policy);

        state.append(makeSizedEntry(1, 40));
        state.append(makeSizedEntry(2, 40));
        state.append(makeSizedEntry(3, 40));

        CHECK(state.size() <= 2);
        CHECK(state.size() >= 1);
        CHECK(state.entryAt(state.size() - 1).sequence == 3);

        auto stats = state.stats();
        CHECK(stats.totalEntries == state.size());
        CHECK(stats.trimmedEntries >= 1);
    }

    TEST_CASE("cursor stays aligned after retention") {
        JournalState::RetentionPolicy policy;
        policy.maxEntries = 3;
        JournalState state(policy);

        state.append(makeEntry(1, "a"));
        state.append(makeEntry(2, "b"));
        state.append(makeEntry(3, "c"));

        auto firstUndo = state.undo();
        REQUIRE(firstUndo.has_value());
        CHECK(firstUndo->get().sequence == 3);

        state.append(makeEntry(4, "d")); // clears redo tail
        state.append(makeEntry(5, "e")); // may trim old entries

        CHECK(state.canUndo());
        auto undoAfterTrim = state.undo();
        REQUIRE(undoAfterTrim.has_value());
        CHECK(undoAfterTrim->get().sequence == 5);
    }

    TEST_CASE("serialization round-trips journal entries") {
        JournalState state;
        state.append(makeEntry(1, "a"));
        state.append(makeEntry(2, "b"));
        state.append(makeEntry(3, "c"));

        std::vector<std::vector<std::byte>> serialized;
        serialized.reserve(state.size());

        for (std::size_t i = 0; i < state.size(); ++i) {
            auto encoded = serializeEntry(state.entryAt(i));
            REQUIRE(encoded.has_value());
            serialized.push_back(std::move(encoded.value()));
        }

        JournalState restored;
        for (auto const& buffer : serialized) {
            auto decoded = deserializeEntry(std::span<const std::byte>{buffer.data(), buffer.size()});
            REQUIRE(decoded.has_value());
            restored.append(std::move(decoded.value()));
        }

        CHECK(restored.size() == state.size());
        CHECK(restored.stats().undoCount == restored.size());
        for (std::size_t i = 0; i < restored.size(); ++i) {
            CHECK(restored.entryAt(i).sequence == state.entryAt(i).sequence);
            CHECK(restored.entryAt(i).path == state.entryAt(i).path);
        }

        auto undo = restored.undo();
        REQUIRE(undo.has_value());
        CHECK(undo->get().sequence == 3);
        auto redo = restored.redo();
        REQUIRE(redo.has_value());
        CHECK(redo->get().sequence == 3);
    }
}
