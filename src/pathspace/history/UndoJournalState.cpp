#include "history/UndoJournalState.hpp"

#include <cstddef>

namespace SP::History::UndoJournal {

JournalState::JournalState()
    : JournalState(RetentionPolicy{}) {}

JournalState::JournalState(RetentionPolicy policy)
    : retention(policy) {}

void JournalState::clear() {
    entries.clear();
    cursorIndex    = 0;
    totalBytes     = 0;
    trimmedEntries = 0;
    trimmedBytes   = 0;
}

void JournalState::setRetentionPolicy(RetentionPolicy policy) {
    retention = policy;
    enforceRetention();
}

void JournalState::append(JournalEntry entry, bool enforceRetentionNow) {
    dropRedoTail();
    totalBytes += entryBytes(entry);
    entries.push_back(std::move(entry));
    cursorIndex = entries.size();
    if (enforceRetentionNow) {
        enforceRetention();
    }
}

auto JournalState::canUndo() const -> bool {
    return cursorIndex > 0;
}

auto JournalState::canRedo() const -> bool {
    return cursorIndex < entries.size();
}

auto JournalState::peekUndo() const
    -> std::optional<std::reference_wrapper<JournalEntry const>> {
    if (!canUndo())
        return std::nullopt;
    return entries[cursorIndex - 1];
}

auto JournalState::peekRedo() const
    -> std::optional<std::reference_wrapper<JournalEntry const>> {
    if (!canRedo())
        return std::nullopt;
    return entries[cursorIndex];
}

auto JournalState::undo()
    -> std::optional<std::reference_wrapper<JournalEntry const>> {
    if (!canUndo())
        return std::nullopt;
    cursorIndex -= 1;
    return entries[cursorIndex];
}

auto JournalState::redo()
    -> std::optional<std::reference_wrapper<JournalEntry const>> {
    if (!canRedo())
        return std::nullopt;
    auto& entry = entries[cursorIndex];
    cursorIndex += 1;
    return entry;
}

auto JournalState::entryAt(std::size_t index) const -> JournalEntry const& {
    return entries.at(index);
}

auto JournalState::stats() const -> Stats {
    Stats s;
    s.totalEntries   = entries.size();
    s.undoCount      = cursorIndex;
    s.redoCount      = entries.size() - cursorIndex;
    s.totalBytes     = totalBytes;
    std::size_t undoBytes = 0;
    for (std::size_t i = 0; i < cursorIndex; ++i) {
        undoBytes += entryBytes(entries[i]);
    }
    s.undoBytes = undoBytes;
    s.redoBytes = totalBytes >= undoBytes ? totalBytes - undoBytes : 0;
    s.trimmedEntries = trimmedEntries;
    s.trimmedBytes   = trimmedBytes;
    return s;
}

auto JournalState::entryBytes(JournalEntry const& entry) -> std::size_t {
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

void JournalState::dropRedoTail() {
    while (entries.size() > cursorIndex) {
        totalBytes -= entryBytes(entries.back());
        entries.pop_back();
    }
}

void JournalState::enforceRetention() {
    if (entries.empty())
        return;

    auto exceedsLimits = [&] {
        bool overEntries = retention.maxEntries != 0
                           && entries.size() > retention.maxEntries;
        bool overBytes = retention.maxBytes != 0
                         && totalBytes > retention.maxBytes;
        return overEntries || overBytes;
    };

    while (!entries.empty() && exceedsLimits()) {
        auto bytes = entryBytes(entries.front());
        entries.pop_front();
        totalBytes -= bytes;
        trimmedEntries += 1;
        trimmedBytes += bytes;

        if (cursorIndex > 0)
            cursorIndex -= 1;
    }

    if (cursorIndex > entries.size())
        cursorIndex = entries.size();
}

} // namespace SP::History::UndoJournal
