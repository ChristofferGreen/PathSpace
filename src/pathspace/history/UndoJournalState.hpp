#pragma once

#include "history/UndoJournalEntry.hpp"

#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace SP::History::UndoJournal {

class JournalState {
public:
    struct RetentionPolicy {
        std::size_t maxEntries = 0; // 0 == unlimited
        std::size_t maxBytes   = 0; // 0 == unlimited
    };

    struct Stats {
        std::size_t totalEntries    = 0;
        std::size_t undoCount       = 0;
        std::size_t redoCount       = 0;
        std::size_t totalBytes      = 0;
        std::size_t trimmedEntries  = 0;
        std::size_t trimmedBytes    = 0;
    };

    JournalState();
    explicit JournalState(RetentionPolicy policy);

    void clear();
    void setRetentionPolicy(RetentionPolicy policy);
    [[nodiscard]] auto policy() const -> RetentionPolicy const& { return retention; }

    void append(JournalEntry entry, bool enforceRetention = true);

    [[nodiscard]] auto size() const -> std::size_t { return entries.size(); }
    [[nodiscard]] auto cursor() const -> std::size_t { return cursorIndex; }

    [[nodiscard]] auto canUndo() const -> bool;
    [[nodiscard]] auto canRedo() const -> bool;

    [[nodiscard]] auto peekUndo() const
        -> std::optional<std::reference_wrapper<JournalEntry const>>;
    [[nodiscard]] auto peekRedo() const
        -> std::optional<std::reference_wrapper<JournalEntry const>>;

    [[nodiscard]] auto undo()
        -> std::optional<std::reference_wrapper<JournalEntry const>>;
    [[nodiscard]] auto redo()
        -> std::optional<std::reference_wrapper<JournalEntry const>>;

    [[nodiscard]] auto entryAt(std::size_t index) const -> JournalEntry const&;

    [[nodiscard]] auto stats() const -> Stats;

private:
    static auto entryBytes(JournalEntry const& entry) -> std::size_t;

    void dropRedoTail();
    void enforceRetention();

    std::deque<JournalEntry> entries;
    std::size_t              cursorIndex    = 0;
    RetentionPolicy          retention      = {};
    std::size_t              totalBytes     = 0;
    std::size_t              trimmedEntries = 0;
    std::size_t              trimmedBytes   = 0;
};

} // namespace SP::History::UndoJournal
