#include <pathspace/history/UndoJournalEntry.hpp>
#include <pathspace/history/UndoJournalPersistence.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

using namespace SP;
using namespace SP::History;

namespace {

struct Summary {
    std::uint64_t entries      = 0;
    std::uint64_t undoCount    = 0;
    std::uint64_t redoCount    = 0;
    std::uint64_t trimmedCount = 0;
};

auto inspectJournal(std::filesystem::path const& journalPath) -> std::optional<Summary> {
    Summary summary{};

    auto replay = UndoJournal::replayJournal(
        journalPath,
        [&](UndoJournal::JournalEntry&& entry) -> Expected<void> {
            summary.entries += 1;
            if (entry.barrier) {
                summary.trimmedCount += 1;
            }
            if (entry.operation == UndoJournal::OperationKind::Insert) {
                summary.undoCount += 1;
            } else if (entry.operation == UndoJournal::OperationKind::Take) {
                summary.redoCount += 1;
            }
            return Expected<void>{};
        });

    if (!replay) {
        std::cerr << "Failed to read journal: "
                  << replay.error().message.value_or("unknown") << "\n";
        return std::nullopt;
    }

    return summary;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: pathspace_history_inspect <history_root>" << std::endl;
        return EXIT_FAILURE;
    }

    std::filesystem::path root{argv[1]};
    auto journalPath = root / "journal.log";
    if (!std::filesystem::exists(journalPath)) {
        std::cerr << "No journal.log found under " << root << std::endl;
        return EXIT_FAILURE;
    }

    auto summary = inspectJournal(journalPath);
    if (!summary) {
        return EXIT_FAILURE;
    }

    std::cout << "PathSpace journal summary" << std::endl;
    std::cout << "  path: " << journalPath << std::endl;
    std::cout << "  entries: " << summary->entries << std::endl;
    std::cout << "  insert operations: " << summary->undoCount << std::endl;
    std::cout << "  take operations: " << summary->redoCount << std::endl;
    std::cout << "  barrier entries: " << summary->trimmedCount << std::endl;
    std::cout << std::endl;
    std::cout << "NOTE: Snapshot-based inspection has been removed."
              << " This tool now reports journal statistics only." << std::endl;

    return EXIT_SUCCESS;
}
