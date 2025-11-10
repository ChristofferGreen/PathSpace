#include <pathspace/PathSpace.hpp>
#include <pathspace/history/UndoableSpace.hpp>

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

using namespace SP;
using namespace SP::History;

namespace {

enum class Command {
    Export,
    Import
};

struct ParsedArguments {
    Command                 command;
    std::filesystem::path   historyDir;
    std::filesystem::path   filePath;
    std::string             rootPath;
    std::optional<std::filesystem::path> persistenceRootOverride;
    std::optional<std::string>           namespaceOverride;
    bool                                  fsyncData      = true;
    bool                                  applyOptions   = true;
};

struct PersistenceLayout {
    std::filesystem::path baseRoot;
    std::string           nsToken;
    std::string           encodedRoot;
    std::filesystem::path expectedHistoryDir;
};

void print_usage() {
    std::cout << "Usage:\n"
                 "  pathspace_history_savefile export --root <path> --history-dir <dir> --out <file> [--no-fsync] [--persistence-root <dir>] [--namespace <token>]\n"
                 "  pathspace_history_savefile import --root <path> --history-dir <dir> --in <file> [--no-apply-options] [--persistence-root <dir>] [--namespace <token>]\n"
                 "\n"
                 "Arguments:\n"
                 "  --root <path>            Concrete history root path (e.g. /doc)\n"
                 "  --history-dir <dir>      Directory containing state.meta and entries/ for the undo root\n"
                 "  --out <file>             Savefile path for export (history.journal.v1)\n"
                 "  --in <file>              Savefile path to import\n"
                 "  --persistence-root <dir> Override the persistence root directory (defaults to parent of --history-dir)\n"
                 "  --namespace <token>      Override the persistence namespace (defaults to parent directory name of --history-dir)\n"
                 "  --no-fsync               Skip fsync when writing savefiles (export)\n"
                 "  --no-apply-options       Preserve current persistence options instead of applying savefile options (import)\n"
                 "  --help                   Show this message\n";
}

std::string error_code_to_string(Error::Code code) {
    switch (code) {
    case Error::Code::InvalidError: return "InvalidError";
    case Error::Code::UnknownError: return "UnknownError";
    case Error::Code::NoSuchPath: return "NoSuchPath";
    case Error::Code::InvalidPath: return "InvalidPath";
    case Error::Code::InvalidPathSubcomponent: return "InvalidPathSubcomponent";
    case Error::Code::InvalidType: return "InvalidType";
    case Error::Code::Timeout: return "Timeout";
    case Error::Code::MalformedInput: return "MalformedInput";
    case Error::Code::InvalidPermissions: return "InvalidPermissions";
    case Error::Code::SerializationFunctionMissing: return "SerializationFunctionMissing";
    case Error::Code::UnserializableType: return "UnserializableType";
    case Error::Code::NoObjectFound: return "NoObjectFound";
    case Error::Code::TypeMismatch: return "TypeMismatch";
    case Error::Code::NotFound: return "NotFound";
    case Error::Code::NotSupported: return "NotSupported";
    }
    return "UnknownError";
}

[[nodiscard]] std::string format_error(Error const& error) {
    std::ostringstream oss;
    oss << error_code_to_string(error.code);
    if (error.message && !error.message->empty()) {
        oss << ": " << *error.message;
    }
    return oss.str();
}

std::optional<ParsedArguments> parse_arguments(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return std::nullopt;
    }

    std::string_view command{argv[1]};
    if (command == "--help" || command == "-h") {
        print_usage();
        std::exit(EXIT_SUCCESS);
    }

    ParsedArguments args;
    if (command == "export") {
        args.command = Command::Export;
    } else if (command == "import") {
        args.command = Command::Import;
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage();
        return std::nullopt;
    }

    for (int i = 2; i < argc; ++i) {
        std::string_view option{argv[i]};
        auto require_value = [&](std::string_view name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << name << " requires a value\n";
                throw std::runtime_error("missing argument");
            }
            ++i;
            return std::string{argv[i]};
        };

        try {
            if (option == "--root") {
                args.rootPath = require_value(option);
            } else if (option == "--history-dir") {
                args.historyDir = std::filesystem::path(require_value(option));
            } else if (option == "--out") {
                args.filePath = std::filesystem::path(require_value(option));
            } else if (option == "--in") {
                args.filePath = std::filesystem::path(require_value(option));
            } else if (option == "--persistence-root") {
                args.persistenceRootOverride = std::filesystem::path(require_value(option));
            } else if (option == "--namespace") {
                args.namespaceOverride = require_value(option);
            } else if (option == "--no-fsync") {
                args.fsyncData = false;
            } else if (option == "--no-apply-options") {
                args.applyOptions = false;
            } else if (option == "--help" || option == "-h") {
                print_usage();
                std::exit(EXIT_SUCCESS);
            } else {
                std::cerr << "Unknown option: " << option << "\n";
                return std::nullopt;
            }
        } catch (std::runtime_error const&) {
            return std::nullopt;
        }
    }

    if (args.rootPath.empty()) {
        std::cerr << "--root is required\n";
        return std::nullopt;
    }
    if (args.historyDir.empty()) {
        std::cerr << "--history-dir is required\n";
        return std::nullopt;
    }
    if (args.filePath.empty()) {
        std::cerr << (args.command == Command::Export ? "--out" : "--in") << " is required\n";
        return std::nullopt;
    }

    if (args.command == Command::Export && !args.applyOptions) {
        std::cerr << "--no-apply-options is only valid for import\n";
        return std::nullopt;
    }
    if (args.command == Command::Import && !args.fsyncData) {
        std::cerr << "--no-fsync is only valid for export\n";
        return std::nullopt;
    }

    return args;
}

std::string encode_root_token(std::string_view root) {
    if (root.empty() || root == "/") {
        return "__root";
    }
    std::string encoded;
    encoded.reserve(root.size());
    for (char c : root) {
        encoded.push_back(c == '/' ? '_' : c);
    }
    return encoded;
}

std::optional<PersistenceLayout> derive_layout(ParsedArguments const& args) {
    auto historyDirAbs = std::filesystem::absolute(args.historyDir);
    auto encodedName   = historyDirAbs.filename();
    if (encodedName.empty()) {
        std::cerr << "Unable to determine encoded root directory from --history-dir\n";
        return std::nullopt;
    }
    auto parent = historyDirAbs.parent_path();
    if (parent.empty()) {
        std::cerr << "Unable to determine namespace directory from --history-dir\n";
        return std::nullopt;
    }
    auto grandParent = parent.parent_path();
    if (grandParent.empty() && !args.persistenceRootOverride) {
        std::cerr << "Unable to determine persistence root; use --persistence-root\n";
        return std::nullopt;
    }

    PersistenceLayout layout;
    layout.encodedRoot = encodedName.string();
    layout.nsToken     = args.namespaceOverride ? *args.namespaceOverride
                                                : (parent.filename().empty() ? std::string{} : parent.filename().string());
    layout.baseRoot    = args.persistenceRootOverride ? *args.persistenceRootOverride : grandParent;
    if (layout.baseRoot.empty()) {
        std::cerr << "Unable to derive persistence root; use --persistence-root\n";
        return std::nullopt;
    }
    layout.expectedHistoryDir = std::filesystem::absolute(layout.baseRoot / layout.nsToken / layout.encodedRoot);

    if (layout.nsToken.empty()) {
        std::cerr << "Unable to derive persistence namespace; use --namespace\n";
        return std::nullopt;
    }

    // Validate root encoding if we have a namespace token
    auto encodedRootExpected = encode_root_token(args.rootPath);
    if (encodedRootExpected != layout.encodedRoot) {
        std::cerr << "Encoded root directory (" << layout.encodedRoot
                  << ") does not match encoded --root (" << encodedRootExpected << ")\n";
        return std::nullopt;
    }

    auto expectedDir = layout.expectedHistoryDir;
    auto providedDir = historyDirAbs;
    if (!args.persistenceRootOverride && !args.namespaceOverride) {
        if (expectedDir != providedDir) {
            std::cerr << "Derived persistence location " << expectedDir
                      << " does not match --history-dir " << providedDir << "\n";
            return std::nullopt;
        }
    } else if (expectedDir != providedDir) {
        std::cout << "Info: using derived persistence directory " << expectedDir
                  << " (overrides supplied); provided --history-dir was " << providedDir << "\n";
    }

    return layout;
}

std::unique_ptr<UndoableSpace> make_undoable_space(PersistenceLayout const& layout) {
    auto inner = std::make_unique<PathSpace>();
    HistoryOptions defaults;
    defaults.persistHistory       = true;
    defaults.persistenceRoot      = layout.baseRoot.string();
    defaults.persistenceNamespace = layout.nsToken;
    defaults.restoreFromPersistence = true;
    defaults.allowNestedUndo      = true;
    defaults.useMutationJournal   = true;
    return std::make_unique<UndoableSpace>(std::move(inner), defaults);
}

int run_export(ParsedArguments const& args, PersistenceLayout const& layout) {
    auto undoable = make_undoable_space(layout);
    auto rootView = ConcretePathStringView{args.rootPath};

    auto enable = undoable->enableHistory(rootView);
    if (!enable) {
        std::cerr << "Failed to enable history: " << format_error(enable.error()) << "\n";
        return EXIT_FAILURE;
    }

    if (!args.fsyncData) {
        std::cout << "Warning: export will skip fsync; resulting file may lose durability on crash.\n";
    }

    if (auto parent = args.filePath.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            std::cerr << "Failed to create directories for " << parent << ": " << ec.message() << "\n";
            return EXIT_FAILURE;
        }
    }

    auto exportResult = undoable->exportHistorySavefile(rootView, args.filePath, args.fsyncData);
    if (!exportResult) {
        std::cerr << "Export failed: " << format_error(exportResult.error()) << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Exported history for " << args.rootPath << " to " << args.filePath << "\n";
    return EXIT_SUCCESS;
}

int run_import(ParsedArguments const& args, PersistenceLayout const& layout) {
    if (!std::filesystem::exists(args.filePath)) {
        std::cerr << "Savefile does not exist: " << args.filePath << "\n";
        return EXIT_FAILURE;
    }

    auto undoable = make_undoable_space(layout);
    auto rootView = ConcretePathStringView{args.rootPath};

    auto enable = undoable->enableHistory(rootView);
    if (!enable) {
        std::cerr << "Failed to enable history: " << format_error(enable.error()) << "\n";
        return EXIT_FAILURE;
    }

    auto importResult =
        undoable->importHistorySavefile(rootView, args.filePath, args.applyOptions);
    if (!importResult) {
        std::cerr << "Import failed: " << format_error(importResult.error()) << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Imported history from " << args.filePath << " into " << layout.expectedHistoryDir
              << " (root " << args.rootPath << ")\n";
    return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char** argv) {
    auto argsOpt = parse_arguments(argc, argv);
    if (!argsOpt) {
        return EXIT_FAILURE;
    }

    auto layoutOpt = derive_layout(*argsOpt);
    if (!layoutOpt) {
        return EXIT_FAILURE;
    }

    switch (argsOpt->command) {
    case Command::Export:
        return run_export(*argsOpt, *layoutOpt);
    case Command::Import:
        return run_import(*argsOpt, *layoutOpt);
    }

    return EXIT_FAILURE;
}
