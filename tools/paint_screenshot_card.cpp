#include "inspector/PaintScreenshotCard.hpp"
#include "core/Error.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

using namespace SP;
using namespace SP::Inspector;

namespace {

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

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path metrics_path;
    std::size_t max_runs = 10;
    bool emit_json_stdout = false;
    std::optional<std::filesystem::path> output_json_path;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--metrics-json" && i + 1 < argc) {
            metrics_path = argv[++i];
            continue;
        }
        if (arg == "--max-runs" && i + 1 < argc) {
            try {
                max_runs = static_cast<std::size_t>(std::stoul(argv[++i]));
            } catch (...) {
                std::cerr << "Invalid value for --max-runs\n";
                return EXIT_FAILURE;
            }
            continue;
        }
        if (arg == "--json") {
            emit_json_stdout = true;
            continue;
        }
        if (arg == "--output-json" && i + 1 < argc) {
            output_json_path = std::filesystem::path{argv[++i]};
            continue;
        }
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return EXIT_SUCCESS;
        }
        std::cerr << "Unknown argument: " << arg << "\n";
        printUsage();
        return EXIT_FAILURE;
    }

    if (metrics_path.empty()) {
        printUsage();
        return EXIT_FAILURE;
    }

    auto runs = LoadPaintScreenshotRunsFromJson(metrics_path, max_runs);
    if (!runs) {
        std::cerr << "Failed to parse diagnostics: "
                  << describeError(runs.error()) << "\n";
        return EXIT_FAILURE;
    }

    PaintScreenshotCardOptions options{};
    options.max_runs = max_runs;
    auto card = BuildPaintScreenshotCardFromRuns(std::move(*runs), options);
    auto json_payload = SerializePaintScreenshotCard(card);

    if (output_json_path) {
        std::ofstream out(*output_json_path, std::ios::trunc);
        if (!out) {
            std::cerr << "Failed to open " << output_json_path->string() << " for writing\n";
            return EXIT_FAILURE;
        }
        out << json_payload << "\n";
    }

    if (emit_json_stdout) {
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
