#include <pathspace/examples/cli/ExampleCli.hpp>
#include <pathspace/examples/paint/PaintExampleApp.hpp>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#error "pathspace_screenshot_cli currently requires macOS/CommonCrypto"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <system_error>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct CliArgs {
    std::filesystem::path manifest_path{"docs/images/paint_example_baselines.json"};
    std::string tag{"1280x800"};
    std::optional<std::filesystem::path> baseline_override;
    std::optional<std::filesystem::path> screenshot_output;
    std::optional<std::filesystem::path> diff_output;
    std::optional<std::filesystem::path> metrics_output;
    std::optional<double> tolerance_override;
};

void print_usage() {
    std::cerr << "Usage: pathspace_screenshot_cli [options]\n"
              << "Options:\n"
              << "  --manifest <path>         Path to baseline manifest JSON (default docs/images/paint_example_baselines.json)\n"
              << "  --tag <name>              Capture tag inside the manifest (default 1280x800)\n"
              << "  --baseline <path>         Override baseline PNG path (must match manifest entry)\n"
              << "  --screenshot-output <path> Output PNG path (default build/artifacts/paint_example/<tag>_screenshot.png)\n"
              << "  --diff-output <path>      Diff PNG path (default build/artifacts/paint_example/<tag>_diff.png)\n"
              << "  --metrics-output <path>   Metrics JSON path (default build/artifacts/paint_example/<tag>_metrics.json)\n"
              << "  --tolerance <value>       Override max mean absolute error threshold\n";
}

std::optional<CliArgs> parse_cli(int argc, char** argv) {
    CliArgs args;
    using ExampleCli = SP::Examples::CLI::ExampleCli;
    ExampleCli cli;
    cli.set_program_name("pathspace_screenshot_cli");
    cli.set_unknown_argument_handler([&](std::string_view token) {
        std::cerr << "pathspace_screenshot_cli: unknown flag '" << token << "'\n";
        return false;
    });

    auto to_path = [](std::string_view text) {
        return std::filesystem::path(std::string(text.begin(), text.end()));
    };

    auto add_required_path = [&](std::string_view name, std::filesystem::path& target) {
        ExampleCli::ValueOption option{};
        option.on_value = [&, option_name = std::string(name)](std::optional<std::string_view> value)
                              -> ExampleCli::ParseError {
            if (!value || value->empty()) {
                return option_name + " requires a path";
            }
            target = to_path(*value);
            return std::nullopt;
        };
        cli.add_value(name, std::move(option));
    };

    auto add_optional_path = [&](std::string_view name, std::optional<std::filesystem::path>& target) {
        ExampleCli::ValueOption option{};
        option.on_value = [&, option_name = std::string(name)](std::optional<std::string_view> value)
                              -> ExampleCli::ParseError {
            if (!value || value->empty()) {
                return option_name + " requires a path";
            }
            target = to_path(*value);
            return std::nullopt;
        };
        cli.add_value(name, std::move(option));
    };

    ExampleCli::ValueOption tag_option{};
    tag_option.on_value = [&](std::optional<std::string_view> value) -> ExampleCli::ParseError {
        if (!value || value->empty()) {
            return std::string("--tag requires a value");
        }
        args.tag.assign(value->begin(), value->end());
        return std::nullopt;
    };

    add_required_path("--manifest", args.manifest_path);
    cli.add_value("--tag", std::move(tag_option));
    add_optional_path("--baseline", args.baseline_override);
    add_optional_path("--screenshot-output", args.screenshot_output);
    add_optional_path("--diff-output", args.diff_output);
    add_optional_path("--metrics-output", args.metrics_output);
    cli.add_double("--tolerance", {.on_value = [&](double value) { args.tolerance_override = value; }});

    auto help_handler = [] {
        print_usage();
        std::exit(0);
    };
    cli.add_flag("--help", {.on_set = help_handler});
    cli.add_flag("-h", {.on_set = help_handler});

    if (!cli.parse(argc, argv)) {
        return std::nullopt;
    }
    return args;
}

std::optional<std::string> read_file(std::filesystem::path const& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return content;
}

std::optional<std::string> compute_sha256(std::filesystem::path const& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<char> buffer(1 << 15);
    while (stream) {
        stream.read(buffer.data(), buffer.size());
        auto read_bytes = stream.gcount();
        if (read_bytes > 0) {
            CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(read_bytes));
        }
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    static char const* kHex = "0123456789abcdef";
    std::string hex;
    hex.resize(CC_SHA256_DIGEST_LENGTH * 2);
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        hex[i * 2] = kHex[(digest[i] >> 4) & 0xF];
        hex[i * 2 + 1] = kHex[digest[i] & 0xF];
    }
    return hex;
}

auto default_artifact_path(std::string const& tag, std::string_view suffix) -> std::filesystem::path {
    auto base = std::filesystem::path("build") / "artifacts" / "paint_example";
    std::filesystem::create_directories(base);
    std::string sanitized = tag;
    std::replace(sanitized.begin(), sanitized.end(), '/', '_');
    return base / ("paint_example_" + sanitized + std::string{suffix});
}

} // namespace

int main(int argc, char** argv) {
    auto parsed_args_opt = parse_cli(argc, argv);
    if (!parsed_args_opt) {
        print_usage();
        return 1;
    }
    auto const& parsed_args = *parsed_args_opt;

    auto manifest_path = std::filesystem::absolute(parsed_args.manifest_path);
    auto manifest_content = read_file(manifest_path);
    if (!manifest_content) {
        std::cerr << "pathspace_screenshot_cli: failed to read manifest at " << manifest_path << "\n";
        return 1;
    }

    nlohmann::json manifest_json{};
    try {
        manifest_json = nlohmann::json::parse(*manifest_content);
    } catch (std::exception const& err) {
        std::cerr << "pathspace_screenshot_cli: manifest parse error: " << err.what() << "\n";
        return 1;
    }

    auto captures_it = manifest_json.find("captures");
    if (captures_it == manifest_json.end() || !captures_it->is_object()) {
        std::cerr << "pathspace_screenshot_cli: manifest missing 'captures' object\n";
        return 1;
    }
    auto entry_it = captures_it->find(parsed_args.tag);
    if (entry_it == captures_it->end()) {
        std::cerr << "pathspace_screenshot_cli: manifest missing tag '" << parsed_args.tag << "'\n";
        return 1;
    }
    auto const& entry = *entry_it;
    int width = entry.value("width", 0);
    int height = entry.value("height", 0);
    if (width <= 0 || height <= 0) {
        std::cerr << "pathspace_screenshot_cli: manifest entry '" << parsed_args.tag << "' missing width/height\n";
        return 1;
    }

    auto manifest_baseline_rel = entry.value("path", std::string{});
    if (manifest_baseline_rel.empty()) {
        std::cerr << "pathspace_screenshot_cli: manifest entry '" << parsed_args.tag << "' missing baseline path\n";
        return 1;
    }
    auto resolve_relative_path = [&](std::filesystem::path rel) {
        if (rel.is_absolute()) {
            return rel;
        }
        auto search = manifest_path.parent_path();
        while (!search.empty()) {
            auto candidate = (search / rel).lexically_normal();
            if (std::filesystem::exists(candidate)) {
                return candidate;
            }
            auto next = search.parent_path();
            if (next == search) {
                break;
            }
            search = next;
        }
        return (manifest_path.parent_path() / rel).lexically_normal();
    };
    auto baseline_resolved = resolve_relative_path(std::filesystem::path{manifest_baseline_rel});
    auto baseline_path = parsed_args.baseline_override
                             ? std::filesystem::absolute(*parsed_args.baseline_override)
                             : std::filesystem::absolute(baseline_resolved);
    if (!std::filesystem::exists(baseline_path)) {
        std::cerr << "pathspace_screenshot_cli: baseline PNG not found: " << baseline_path << "\n";
        return 1;
    }

    std::optional<std::string> recorded_sha;
    if (entry.contains("sha256") && entry["sha256"].is_string()) {
        recorded_sha = entry["sha256"].get<std::string>();
    }
    if (recorded_sha) {
        auto actual_sha = compute_sha256(baseline_path);
        if (!actual_sha) {
            std::cerr << "pathspace_screenshot_cli: failed to compute sha256 for " << baseline_path << "\n";
            return 1;
        }
        if (*actual_sha != *recorded_sha) {
            std::cerr << "pathspace_screenshot_cli: baseline hash mismatch for tag '" << parsed_args.tag << "'\n"
                      << "  manifest: " << *recorded_sha << "\n"
                      << "  actual  : " << *actual_sha << "\n"
                      << "Re-run scripts/paint_example_capture.py --tags " << parsed_args.tag << "\n";
            return 1;
        }
    }

    auto screenshot_path = parsed_args.screenshot_output
                               ? std::filesystem::absolute(*parsed_args.screenshot_output)
                               : default_artifact_path(parsed_args.tag, "_screenshot.png");
    auto diff_path = parsed_args.diff_output
                         ? std::filesystem::absolute(*parsed_args.diff_output)
                         : default_artifact_path(parsed_args.tag, "_diff.png");
    auto metrics_path = parsed_args.metrics_output
                            ? std::filesystem::absolute(*parsed_args.metrics_output)
                            : default_artifact_path(parsed_args.tag, "_metrics.json");

    double tolerance = parsed_args.tolerance_override.value_or(entry.value("tolerance", 0.0015));

    PathSpaceExamples::CommandLineOptions options;
    options.width = width;
    options.height = height;
    options.headless = true;
    options.gpu_smoke = true;
    options.screenshot_require_present = true;
    options.screenshot_max_mean_error = tolerance;
    options.screenshot_path = screenshot_path;
    options.screenshot_compare_path = baseline_path;
    options.screenshot_diff_path = diff_path;
    options.screenshot_metrics_path = metrics_path;
    options.baseline_metadata.manifest_revision = manifest_json.value("manifest_revision", 0);
    options.baseline_metadata.tag = parsed_args.tag;
    options.baseline_metadata.sha256 = recorded_sha;
    options.baseline_metadata.width = width;
    options.baseline_metadata.height = height;
    options.baseline_metadata.renderer = entry.value("renderer", std::string{});
    options.baseline_metadata.captured_at = entry.value("captured_at", std::string{});
    options.baseline_metadata.commit = entry.value("commit", std::string{});
    options.baseline_metadata.notes = entry.value("notes", std::string{});
    options.baseline_metadata.tolerance = tolerance;
    options.screenshot_telemetry_namespace = "paint_example";
    options.screenshot_telemetry_root = "/diagnostics/ui/screenshot";

    setenv("PATHSPACE_ENABLE_METAL_UPLOADS", "1", 1);
    setenv("PATHSPACE_UI_METAL", "ON", 1);
    setenv("PAINT_EXAMPLE_BASELINE_TAG", parsed_args.tag.c_str(), 1);

    auto run_warmup_capture = [&]() {
        auto warmup = options;
        warmup.screenshot_compare_path.reset();
        warmup.screenshot_diff_path.reset();
        warmup.screenshot_metrics_path.reset();
        warmup.baseline_metadata = {};
        warmup.screenshot_max_mean_error = 1.0;
        auto warmup_path = default_artifact_path(parsed_args.tag, "_warmup.png");
        warmup.screenshot_path = warmup_path;
        int rc = PathSpaceExamples::RunPaintExample(warmup);
        if (rc != 0) {
            std::cerr << "pathspace_screenshot_cli: warm-up capture exited with code " << rc
                      << " (continuing)\n";
        }
        std::error_code ec;
        std::filesystem::remove(warmup_path, ec);
    };

    run_warmup_capture();

    auto run_capture = [&](int attempt) -> int {
        auto attempt_options = options;
        auto child = fork();
        if (child == -1) {
            std::perror("pathspace_screenshot_cli: fork");
            return -1;
        }
        if (child == 0) {
            int rc = PathSpaceExamples::RunPaintExample(std::move(attempt_options));
            std::exit(rc);
        }
        int status = 0;
        if (waitpid(child, &status, 0) == -1) {
            std::perror("pathspace_screenshot_cli: waitpid");
            return -1;
        }
        if (WIFEXITED(status)) {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status)) {
            std::cerr << "pathspace_screenshot_cli: capture attempt " << attempt
                      << " terminated by signal " << WTERMSIG(status) << "\n";
            return 128 + WTERMSIG(status);
        }
        return -1;
    };

    constexpr int kMaxAttempts = 6;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        int exit_code = run_capture(attempt);
        if (exit_code == 0) {
            return 0;
        }
        if (attempt == kMaxAttempts) {
            std::cerr << "pathspace_screenshot_cli: capture failed after " << kMaxAttempts
                      << " attempts\n";
            return exit_code;
        }
        std::cerr << "pathspace_screenshot_cli: capture failed (attempt " << attempt
                  << "), retrying after 0.5s\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return 1;
}
