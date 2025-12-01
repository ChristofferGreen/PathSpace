#include "PathSpace.hpp"
#include "examples/cli/ExampleCli.hpp"
#include "inspector/InspectorDemoData.hpp"
#include "tools/PathSpaceJsonExporter.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

struct DumpJsonOptions {
    std::string              root            = "/";
    std::size_t              maxDepth        = 2;
    std::size_t              maxChildren     = 32;
    std::size_t              maxQueueEntries = 4;
    bool                     includeValues   = true;
    bool                     includeNested   = true;
    bool                     includePlaceholders = true;
    bool                     includeDiagnostics = true;
    int                      indent          = 2;
    std::optional<std::filesystem::path> outputPath;
    bool                     demo            = false;
};

void print_usage() {
    std::cout << "Usage: pathspace_dump_json [options]\n"
                 "Options:\n"
                 "  --root <path>              Root path to export (default /)\n"
                 "  --max-depth <n>            Maximum depth relative to root (default 2)\n"
                 "  --max-children <n>         Maximum children per node (default 32, 0 = unlimited)\n"
                 "  --max-queue-entries <n>    Maximum queue entries per node (default 4)\n"
                 "  --indent <n>               JSON indent (default 2, -1 for compact)\n"
                 "  --output <file>            Write JSON to file instead of stdout\n"
                 "  --no-values                Skip value sampling (structure only)\n"
                 "  --no-nested                Do not traverse nested spaces\n"
                 "  --no-placeholders          Omit opaque placeholders for unsupported values\n"
                 "  --no-diagnostics           Omit per-node diagnostics block\n"
                 "  --demo                     Seed the demo inspector tree before dumping\n"
                 "  --help                     Show this message\n";
}

auto parse_size(std::string_view text, std::size_t& target, std::string_view name)
    -> SP::Examples::CLI::ExampleCli::ParseError {
    if (text.empty()) {
        return std::string{name} + " requires a value";
    }
    std::size_t value = 0;
    auto        result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        return std::string{name} + " must be numeric";
    }
    target = value;
    return std::nullopt;
}

auto parse_int(std::string_view text, int& target, std::string_view name)
    -> SP::Examples::CLI::ExampleCli::ParseError {
    if (text.empty()) {
        return std::string{name} + " requires a value";
    }
    int value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) {
        return std::string{name} + " must be numeric";
    }
    target = value;
    return std::nullopt;
}

auto parse_cli(int argc, char** argv) -> std::optional<DumpJsonOptions> {
    using SP::Examples::CLI::ExampleCli;
    DumpJsonOptions options;

    ExampleCli cli;
    cli.set_program_name("pathspace_dump_json");
    cli.set_error_logger([](std::string const& message) { std::cerr << message << "\n"; });
    cli.set_unknown_argument_handler([](std::string_view token) {
        std::cerr << "Unknown flag '" << token << "'" << std::endl;
        return false;
    });

    ExampleCli::ValueOption rootOption{};
    rootOption.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--root requires a value"};
        }
        options.root.assign(value->begin(), value->end());
        return std::nullopt;
    };
    cli.add_value("--root", std::move(rootOption));

    ExampleCli::ValueOption depthOption{};
    depthOption.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value) {
            return std::string{"--max-depth requires a value"};
        }
        return parse_size(*value, options.maxDepth, "--max-depth");
    };
    cli.add_value("--max-depth", std::move(depthOption));

    ExampleCli::ValueOption childrenOption{};
    childrenOption.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value) {
            return std::string{"--max-children requires a value"};
        }
        return parse_size(*value, options.maxChildren, "--max-children");
    };
    cli.add_value("--max-children", std::move(childrenOption));

    ExampleCli::ValueOption queueOption{};
    queueOption.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value) {
            return std::string{"--max-queue-entries requires a value"};
        }
        return parse_size(*value, options.maxQueueEntries, "--max-queue-entries");
    };
    cli.add_value("--max-queue-entries", std::move(queueOption));

    ExampleCli::ValueOption indentOption{};
    indentOption.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value) {
            return std::string{"--indent requires a value"};
        }
        return parse_int(*value, options.indent, "--indent");
    };
    cli.add_value("--indent", std::move(indentOption));

    ExampleCli::ValueOption outputOption{};
    outputOption.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--output requires a file"};
        }
        options.outputPath = std::filesystem::path(std::string{*value});
        return std::nullopt;
    };
    cli.add_value("--output", std::move(outputOption));

    cli.add_flag("--no-values", {.on_set = [&] { options.includeValues = false; }});
    cli.add_flag("--no-nested", {.on_set = [&] { options.includeNested = false; }});
    cli.add_flag("--no-placeholders", {.on_set = [&] { options.includePlaceholders = false; }});
    cli.add_flag("--no-diagnostics", {.on_set = [&] { options.includeDiagnostics = false; }});
    cli.add_flag("--demo", {.on_set = [&] { options.demo = true; }});

    auto helpHandler = [] {
        print_usage();
        std::exit(0);
    };
    cli.add_flag("--help", {.on_set = helpHandler});
    cli.add_flag("-h", {.on_set = helpHandler});

    if (!cli.parse(argc, argv)) {
        return std::nullopt;
    }
    return options;
}

auto write_output(std::string const& jsonString, std::optional<std::filesystem::path> const& output)
    -> bool {
    if (!output) {
        std::cout << jsonString << std::endl;
        return true;
    }
    std::filesystem::path destination = *output;
    if (auto parent = destination.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    std::ofstream stream(destination, std::ios::binary);
    if (!stream.is_open()) {
        std::cerr << "Failed to open output file '" << destination.string() << "'" << std::endl;
        return false;
    }
    stream << jsonString;
    if (!stream.good()) {
        std::cerr << "Failed to write JSON output" << std::endl;
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    auto cliOptions = parse_cli(argc, argv);
    if (!cliOptions) {
        print_usage();
        return EXIT_FAILURE;
    }

    SP::PathSpace space;
    if (cliOptions->demo) {
        SP::Inspector::SeedInspectorDemoData(space);
    }

    SP::PathSpaceJsonOptions jsonOptions;
    jsonOptions.visit.root                = cliOptions->root;
    jsonOptions.visit.maxDepth            = cliOptions->maxDepth;
    jsonOptions.visit.maxChildren         = cliOptions->maxChildren;
    jsonOptions.visit.includeValues       = cliOptions->includeValues;
    jsonOptions.visit.includeNestedSpaces = cliOptions->includeNested;
    jsonOptions.maxQueueEntries           = cliOptions->maxQueueEntries;
    jsonOptions.includeOpaquePlaceholders = cliOptions->includePlaceholders;
    jsonOptions.includeDiagnostics        = cliOptions->includeDiagnostics;
    jsonOptions.dumpIndent                = cliOptions->indent;

    auto jsonString = space.toJSON(jsonOptions);
    if (!jsonString) {
        std::cerr << "Export failed: " << SP::describeError(jsonString.error()) << std::endl;
        return EXIT_FAILURE;
    }

    if (!write_output(*jsonString, cliOptions->outputPath)) {
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
