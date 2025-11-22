#include "inspector/PaintScreenshotCard.hpp"
#include "core/Error.hpp"

#include <pathspace/examples/cli/ExampleCli.hpp>

#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

using namespace SP;
using namespace SP::Inspector;

namespace {

struct PaintScreenshotCardCliOptions {
    bool                                  show_help        = false;
    bool                                  emit_json_stdout = false;
    std::size_t                           max_runs         = 10;
    std::filesystem::path                 metrics_path;
    std::optional<std::filesystem::path>  output_json_path;
};

auto severityToString(PaintScreenshotSeverity severity) -> std::string_view {
    switch (severity) {
    case PaintScreenshotSeverity::MissingData:
        return "missing";
    case PaintScreenshotSeverity::WaitingForCapture:
        return "waiting";
    case PaintScreenshotSeverity::Healthy:
        return "healthy";
    case PaintScreenshotSeverity::Attention:
        return "attention";
    }
    return "missing";
}

auto printUsage() {
    std::cerr << "Usage: pathspace_paint_screenshot_card --metrics-json <path> "
                 "[--max-runs N] [--json] [--output-json <path>]\n";
}

auto parse_cli(int argc, char** argv) -> std::optional<PaintScreenshotCardCliOptions> {
    using SP::Examples::CLI::ExampleCli;
    PaintScreenshotCardCliOptions options{};

    ExampleCli cli;
    cli.set_program_name("pathspace_paint_screenshot_card");
    cli.set_error_logger([](std::string const& text) { std::cerr << text << "\n"; });
    cli.set_unknown_argument_handler([&](std::string_view token) {
        std::cerr << "pathspace_paint_screenshot_card: unknown argument '" << token << "'\n";
        return false;
    });

    cli.add_flag("--help", {.on_set = [&] { options.show_help = true; }});
    cli.add_alias("-h", "--help");
    cli.add_flag("--json", {.on_set = [&] { options.emit_json_stdout = true; }});

    ExampleCli::ValueOption metrics_option{};
    metrics_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--metrics-json requires a path"};
        }
        options.metrics_path = std::filesystem::path(std::string{*value});
        return std::nullopt;
    };
    cli.add_value("--metrics-json", std::move(metrics_option));

    ExampleCli::ValueOption max_runs_option{};
    max_runs_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--max-runs requires a value"};
        }
        int parsed = 0;
        auto begin = value->data();
        auto end   = begin + value->size();
        auto res   = std::from_chars(begin, end, parsed);
        if (res.ec != std::errc{} || parsed <= 0) {
            return std::string{"--max-runs expects a positive integer"};
        }
        options.max_runs = static_cast<std::size_t>(parsed);
        return std::nullopt;
    };
    cli.add_value("--max-runs", std::move(max_runs_option));

    ExampleCli::ValueOption output_option{};
    output_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string{"--output-json requires a path"};
        }
        options.output_json_path = std::filesystem::path(std::string{*value});
        return std::nullopt;
    };
    cli.add_value("--output-json", std::move(output_option));

    if (!cli.parse(argc, argv)) {
        return std::nullopt;
    }

    if (!options.show_help && options.metrics_path.empty()) {
        std::cerr << "pathspace_paint_screenshot_card: --metrics-json is required\n";
        return std::nullopt;
    }

    return options;
}

} // namespace

int main(int argc, char** argv) {
    auto cli = parse_cli(argc, argv);
    if (!cli) {
        return EXIT_FAILURE;
    }
    if (cli->show_help) {
        printUsage();
        return EXIT_SUCCESS;
    }

    auto runs = LoadPaintScreenshotRunsFromJson(cli->metrics_path, cli->max_runs);

    if (!runs) {
        std::cerr << "Failed to parse diagnostics: "
                  << describeError(runs.error()) << "\n";
        return EXIT_FAILURE;
    }

    PaintScreenshotCardOptions options{};
    options.max_runs = cli->max_runs;
    auto card = BuildPaintScreenshotCardFromRuns(std::move(*runs), options);
    auto json_payload = SerializePaintScreenshotCard(card);

    if (cli->output_json_path) {
        std::ofstream out(*cli->output_json_path, std::ios::trunc);
        if (!out) {
            std::cerr << "Failed to open " << cli->output_json_path->string()
                      << " for writing\n";
            return EXIT_FAILURE;
        }
        out << json_payload << "\n";
    }

    if (cli->emit_json_stdout) {
        std::cout << json_payload << std::endl;
        return EXIT_SUCCESS;
    }

    std::cout << "Paint Example Screenshot Diagnostics\n";
    std::cout << "severity: " << severityToString(card.severity) << "\n";
    std::cout << "summary : " << card.summary << "\n";
    if (card.manifest.revision) {
        std::cout << "manifest revision: " << *card.manifest.revision << "\n";
    }
    if (card.manifest.tag) {
        std::cout << "tag: " << *card.manifest.tag << "\n";
    }
    if (card.manifest.renderer) {
        std::cout << "renderer: " << *card.manifest.renderer << "\n";
    }
    if (card.manifest.width && card.manifest.height) {
        std::cout << "frame size: " << *card.manifest.width << "x"
                  << *card.manifest.height << "\n";
    }

    if (card.last_run) {
        auto const& run = *card.last_run;
        std::cout << "\nLast Run:\n";
        if (run.timestamp_iso) {
            std::cout << "  timestamp: " << *run.timestamp_iso << "\n";
        } else if (run.timestamp_ns) {
            std::cout << "  timestamp_ns: " << *run.timestamp_ns << "\n";
        }
        if (run.status) {
            std::cout << "  status   : " << *run.status << "\n";
        }
        if (run.hardware_capture) {
            std::cout << "  hardware : " << (*run.hardware_capture ? "true" : "false") << "\n";
        }
        if (run.mean_error) {
            std::cout << "  mean_error: " << std::setprecision(6) << *run.mean_error << "\n";
        }
        if (run.max_channel_delta) {
            std::cout << "  max_delta: " << *run.max_channel_delta << "\n";
        }
        if (run.screenshot_path) {
            std::cout << "  screenshot: " << *run.screenshot_path << "\n";
        }
        if (run.diff_path && !run.diff_path->empty()) {
            std::cout << "  diff: " << *run.diff_path << "\n";
        }
    }

    if (!card.recent_runs.empty()) {
        std::cout << "\nRecent Runs (" << card.recent_runs.size() << "):\n";
        for (auto const& run : card.recent_runs) {
            std::cout << "  - ";
            if (run.timestamp_iso) {
                std::cout << *run.timestamp_iso << " ";
            } else if (run.timestamp_ns) {
                std::cout << *run.timestamp_ns << " ";
            }
            if (run.status) {
                std::cout << *run.status;
            } else {
                std::cout << "unknown";
            }
            if (run.mean_error) {
                std::cout << " (mean_error=" << std::setprecision(6) << *run.mean_error << ")";
            }
            std::cout << "\n";
        }
    }

    return EXIT_SUCCESS;
}
