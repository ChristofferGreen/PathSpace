#define private public
#include "history/UndoJournalEntry.hpp"
#include "history/UndoJournalState.hpp"
#undef private

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

auto entryBytes(JournalEntry const& entry) -> std::size_t {
    std::size_t bytes = sizeof(OperationKind)
                        + sizeof(entry.timestampMs)
                        + sizeof(entry.monotonicNs)
                        + sizeof(entry.sequence)
                        + sizeof(entry.barrier);
    bytes += entry.path.size();
    bytes += sizeof(std::uint32_t) + entry.tag.size();
    bytes += entry.value.bytes.size();
    bytes += entry.inverseValue.bytes.size();
    return bytes;
}

} // namespace

TEST_SUITE("history.journal.state") {
    TEST_CASE("peek and undo/redo return nullopt when empty") {
        JournalState state;

        CHECK_FALSE(state.peekUndo().has_value());
        CHECK_FALSE(state.peekRedo().has_value());
        CHECK_FALSE(state.undo().has_value());
        CHECK_FALSE(state.redo().has_value());
    }

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

        auto peekRedo = state.peekRedo();
        CHECK_FALSE(peekRedo.has_value());

        auto stats = state.stats();
        CHECK(stats.undoBytes + stats.redoBytes == stats.totalBytes);
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
        policy.maxBytes = 160; // keep at least one entry while still triggering byte-based trimming
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
        CHECK(stats.undoBytes + stats.redoBytes == stats.totalBytes);
    }

    TEST_CASE("append can defer retention when requested") {
        JournalState::RetentionPolicy policy;
        policy.maxEntries = 1;
        JournalState state(policy);

        state.append(makeEntry(1, "a"), false);
        state.append(makeEntry(2, "b"), false);

        CHECK(state.size() == 2);
        auto statsBefore = state.stats();
        CHECK(statsBefore.trimmedEntries == 0);

        state.setRetentionPolicy(policy);
        auto statsAfter = state.stats();
        CHECK(statsAfter.trimmedEntries >= 1);
        CHECK(state.size() == 1);
        CHECK(state.entryAt(0).sequence == 2);
        CHECK(statsAfter.undoBytes + statsAfter.redoBytes == statsAfter.totalBytes);
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

    TEST_CASE("peekRedo returns entry when redo is available") {
        JournalState state;
        state.append(makeEntry(1, "a"));
        state.append(makeEntry(2, "b"));

        auto undone = state.undo();
        REQUIRE(undone.has_value());
        CHECK(state.canRedo());

        auto redoPeek = state.peekRedo();
        REQUIRE(redoPeek.has_value());
        CHECK(redoPeek->get().sequence == 2);
    }

    TEST_CASE("retention clamps cursorIndex when it exceeds entries") {
        JournalState state;
        state.append(makeEntry(1, "a"));
        state.append(makeEntry(2, "b"));

        // Force an out-of-range cursor to exercise the safety clamp.
        state.cursorIndex = state.entries.size() + 5;
        state.setRetentionPolicy(state.policy());

        CHECK(state.cursorIndex == state.entries.size());
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

    TEST_CASE("stats total bytes include tags and inverse payloads") {
        JournalState state;
        JournalEntry entry = makeEntry(1, "tagged");
        entry.tag = "meta";
        entry.value.present = true;
        entry.value.bytes = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
        entry.inverseValue.present = true;
        entry.inverseValue.bytes = {std::byte{0x0A}};

        state.append(entry);

        auto stats = state.stats();
        CHECK(stats.totalBytes == entryBytes(entry));
        CHECK(stats.undoBytes == stats.totalBytes);
        CHECK(stats.redoBytes == 0);
    }

    TEST_CASE("append after undo drops redo bytes from totals") {
        JournalState state;
        auto first = makeSizedEntry(1, 4);
        auto second = makeSizedEntry(2, 12);

        state.append(first);
        state.append(second);
        auto undone = state.undo();
        REQUIRE(undone.has_value());

        auto third = makeSizedEntry(3, 8);
        state.append(third);

        auto stats = state.stats();
        CHECK(state.size() == 2);
        CHECK(stats.totalBytes == entryBytes(first) + entryBytes(third));
        CHECK(stats.undoCount == 2);
        CHECK(stats.redoCount == 0);
    }
}
