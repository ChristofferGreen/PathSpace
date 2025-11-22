#include <pathspace/PathSpace.hpp>
#include <pathspace/examples/cli/ExampleCli.hpp>
#include <pathspace/history/UndoableSpace.hpp>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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
                 "  --history-dir <dir>      Directory containing journal.log for the undo root\n"
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
    case Error::Code::CapacityExceeded: return "CapacityExceeded";
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

struct CliParseResult {
    bool show_help = false;
    std::optional<ParsedArguments> arguments;
};

auto parse_arguments(int argc, char** argv) -> std::optional<CliParseResult> {
    auto log_error = [](std::string_view message) {
        std::cerr << "pathspace_history_savefile: " << message << "\n";
    };

    if (argc < 2) {
        log_error("missing command");
        print_usage();
        return std::nullopt;
    }

    CliParseResult result{};

    std::string_view command_token{argv[1]};
    if (command_token == "--help" || command_token == "-h") {
        result.show_help = true;
        return result;
    }

    ParsedArguments args{};
    if (command_token == "export") {
        args.command = Command::Export;
    } else if (command_token == "import") {
        args.command = Command::Import;
    } else {
        log_error(std::string{"unknown command '"} + std::string{command_token} + "'");
        print_usage();
        return std::nullopt;
    }

    using SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("pathspace_history_savefile");
    cli.set_error_logger([](std::string const& text) { std::cerr << text << "\n"; });
    cli.set_unknown_argument_handler([&](std::string_view token) {
        std::cerr << "pathspace_history_savefile: unknown option '" << token << "'\n";
        return false;
    });

    cli.add_flag("--help", {.on_set = [&] { result.show_help = true; }});
    cli.add_alias("-h", "--help");

    ExampleCli::ValueOption root_option{};
    root_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--root requires a path"};
        }
        args.rootPath = std::string{*value};
        return std::nullopt;
    };
    cli.add_value("--root", std::move(root_option));

    ExampleCli::ValueOption history_dir_option{};
    history_dir_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--history-dir requires a path"};
        }
        args.historyDir = std::filesystem::path(std::string{*value});
        return std::nullopt;
    };
    cli.add_value("--history-dir", std::move(history_dir_option));

    bool saw_out = false;
    bool saw_in  = false;

    ExampleCli::ValueOption out_option{};
    out_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--out requires a path"};
        }
        args.filePath = std::filesystem::path(std::string{*value});
        saw_out = true;
        return std::nullopt;
    };
    cli.add_value("--out", std::move(out_option));

    ExampleCli::ValueOption in_option{};
    in_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--in requires a path"};
        }
        args.filePath = std::filesystem::path(std::string{*value});
        saw_in = true;
        return std::nullopt;
    };
    cli.add_value("--in", std::move(in_option));

    ExampleCli::ValueOption persistence_root_option{};
    persistence_root_option.on_value = [&](std::optional<std::string_view> value)
        -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--persistence-root requires a path"};
        }
        args.persistenceRootOverride = std::filesystem::path(std::string{*value});
        return std::nullopt;
    };
    cli.add_value("--persistence-root", std::move(persistence_root_option));

    ExampleCli::ValueOption namespace_option{};
    namespace_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--namespace requires a value"};
        }
        args.namespaceOverride = std::string{*value};
        return std::nullopt;
    };
    cli.add_value("--namespace", std::move(namespace_option));

    cli.add_flag("--no-fsync", {.on_set = [&] { args.fsyncData = false; }});
    cli.add_flag("--no-apply-options", {.on_set = [&] { args.applyOptions = false; }});

    std::vector<char*> forwarded;
    forwarded.reserve(std::max(2, argc - 1));
    forwarded.push_back(argv[0]);
    for (int i = 2; i < argc; ++i) {
        forwarded.push_back(argv[i]);
    }

    if (!cli.parse(static_cast<int>(forwarded.size()), forwarded.data())) {
        return std::nullopt;
    }

    if (result.show_help) {
        return result;
    }

    if (args.rootPath.empty()) {
        log_error("--root is required");
        return std::nullopt;
    }
    if (args.historyDir.empty()) {
        log_error("--history-dir is required");
        return std::nullopt;
    }

    if (args.command == Command::Export) {
        if (!saw_out || saw_in) {
            log_error("export requires --out and must not specify --in");
            return std::nullopt;
        }
    } else {
        if (!saw_in || saw_out) {
            log_error("import requires --in and must not specify --out");
            return std::nullopt;
        }
    }

    if (args.filePath.empty()) {
        log_error("missing savefile path");
        return std::nullopt;
    }

    if (args.command == Command::Export && !args.applyOptions) {
        log_error("--no-apply-options is only valid for import");
        return std::nullopt;
    }
    if (args.command == Command::Import && !args.fsyncData) {
        log_error("--no-fsync is only valid for export");
        return std::nullopt;
    }

    result.arguments = args;
    return result;
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
    auto cli = parse_arguments(argc, argv);
    if (!cli) {
        return EXIT_FAILURE;
    }
    if (cli->show_help) {
        print_usage();
        return EXIT_SUCCESS;
    }
    if (!cli->arguments) {
        return EXIT_FAILURE;
    }

    auto const& args = *cli->arguments;
    auto layoutOpt = derive_layout(args);
    if (!layoutOpt) {
        return EXIT_FAILURE;
    }

    switch (args.command) {
    case Command::Export:
        return run_export(args, *layoutOpt);
    case Command::Import:
        return run_import(args, *layoutOpt);
    }

    return EXIT_FAILURE;
}
