#include <pathspace/examples/cli/ExampleCli.hpp>
#include <pathspace/history/UndoJournalEntry.hpp>
#include <pathspace/history/UndoJournalPersistence.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

using namespace SP;
using namespace SP::History;

namespace {

struct HistoryInspectCliOptions {
    bool                        show_help   = false;
    std::optional<std::filesystem::path> history_root;
};

void print_usage() {
    std::cout << "Usage: pathspace_history_inspect [--history-root <dir>] [history_root]\n"
                 "       pathspace_history_inspect --help\n";
}

auto parse_cli(int argc, char** argv) -> std::optional<HistoryInspectCliOptions> {
    using SP::Examples::CLI::ExampleCli;
    HistoryInspectCliOptions options{};

    ExampleCli cli;
    cli.set_program_name("pathspace_history_inspect");
    cli.set_error_logger([](std::string const& message) { std::cerr << message << "\n"; });

    cli.add_flag("--help", {.on_set = [&] { options.show_help = true; }});
    cli.add_alias("-h", "--help");

    ExampleCli::ValueOption history_root_option{};
    history_root_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--history-root requires a directory"};
        }
        options.history_root = std::filesystem::path(std::string{*value});
        return std::nullopt;
    };
    cli.add_value("--history-root", std::move(history_root_option));

    cli.set_unknown_argument_handler([&](std::string_view token) {
        if (token.empty()) {
            return true;
        }
        if (options.history_root) {
            std::cerr << "pathspace_history_inspect: multiple history roots specified ('"
                      << token << "')\n";
            return false;
        }
        options.history_root = std::filesystem::path(std::string{token});
        return true;
    });

    if (!cli.parse(argc, argv)) {
        return std::nullopt;
    }

    return options;
}

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
    auto cli = parse_cli(argc, argv);
    if (!cli) {
        return EXIT_FAILURE;
    }
    if (cli->show_help) {
        print_usage();
        return EXIT_SUCCESS;
    }
    if (!cli->history_root) {
        std::cerr << "pathspace_history_inspect: missing history root" << std::endl;
        print_usage();
        return EXIT_FAILURE;
    }

    auto journalPath = *cli->history_root / "journal.log";
    if (!std::filesystem::exists(journalPath)) {
        std::cerr << "No journal.log found under " << cli->history_root->string() << std::endl;
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
