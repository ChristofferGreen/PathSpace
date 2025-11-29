#include <pathspace/ui/screenshot/DeclarativeScreenshotCli.hpp>

#include <pathspace/core/Error.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace SP::UI::Screenshot {

namespace {

auto parse_bool_string(std::string_view text) -> std::optional<bool> {
    if (text.empty()) {
        return std::nullopt;
    }
    std::string lowered{text};
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lowered == "1" || lowered == "true" || lowered == "yes" || lowered == "on") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no" || lowered == "off") {
        return false;
    }
    return std::nullopt;
}

auto to_path(std::string_view text) -> std::filesystem::path {
    return std::filesystem::path(std::string(text));
}

} // namespace

void RegisterDeclarativeScreenshotCliOptions(SP::Examples::CLI::ExampleCli& cli,
                                             DeclarativeScreenshotCliOptions& options) {
    using ExampleCli = SP::Examples::CLI::ExampleCli;
    auto add_path_option = [&](std::string_view flag,
                               std::optional<std::filesystem::path>& target) {
        ExampleCli::ValueOption opt{};
        opt.on_value = [&, option_name = std::string(flag)](std::optional<std::string_view> value)
            -> ExampleCli::ParseError {
            if (!value || value->empty()) {
                return option_name + " requires a path";
            }
            target = to_path(*value);
            return std::nullopt;
        };
        cli.add_value(flag, std::move(opt));
    };

    add_path_option("--screenshot", options.output_png);
    add_path_option("--screenshot-compare", options.baseline_png);
    add_path_option("--screenshot-diff", options.diff_png);
    add_path_option("--screenshot-metrics", options.metrics_json);

    cli.add_double("--screenshot-max-mean-error", {.on_value = [&](double value) {
                        options.max_mean_error = value;
                    }});
    cli.add_flag("--screenshot-require-present", {.on_set = [&] {
                       options.require_present = true;
                   }});
    cli.add_flag("--screenshot-force-software", {.on_set = [&] {
                       options.force_software = true;
                       options.allow_software_fallback = true;
                   }});
    cli.add_flag("--screenshot-allow-software-fallback", {.on_set = [&] {
                       options.allow_software_fallback = true;
                   }});
}

void ApplyDeclarativeScreenshotEnvOverrides(DeclarativeScreenshotCliOptions& options) {
    if (const char* value = std::getenv("PATHSPACE_SCREENSHOT_FORCE_SOFTWARE")) {
        if (auto parsed = parse_bool_string(value)) {
            options.force_software = *parsed;
            if (*parsed) {
                options.allow_software_fallback = true;
            }
        }
    }
}

bool DeclarativeScreenshotRequested(DeclarativeScreenshotCliOptions const& options) {
    return options.output_png.has_value();
}

auto CaptureDeclarativeScreenshotIfRequested(
    SP::PathSpace& space,
    SP::UI::ScenePath const& scene,
    SP::UI::WindowPath const& window,
    std::string_view view_name,
    int width,
    int height,
    DeclarativeScreenshotCliOptions const& cli_options,
    std::function<SP::Expected<void>()> pose,
    std::function<void(SP::UI::Screenshot::DeclarativeScreenshotOptions&)> configure)
    -> SP::Expected<void> {
    if (!DeclarativeScreenshotRequested(cli_options)) {
        return {};
    }
    if (pose) {
        auto pose_status = pose();
        if (!pose_status) {
            return pose_status;
        }
    }

    SP::UI::Screenshot::DeclarativeScreenshotOptions options{};
    options.width = width;
    options.height = height;
    options.output_png = cli_options.output_png;
    options.baseline_png = cli_options.baseline_png;
    options.diff_png = cli_options.diff_png;
    options.metrics_json = cli_options.metrics_json;
    options.max_mean_error = cli_options.max_mean_error;
    options.require_present = cli_options.require_present || options.baseline_png.has_value();
    options.force_software = cli_options.force_software;
    options.allow_software_fallback = cli_options.allow_software_fallback;
    options.wait_for_runtime_metrics = cli_options.wait_for_runtime_metrics;
    options.mark_dirty_before_publish = cli_options.mark_dirty_before_publish;
    options.view_name = std::string(view_name);
    options.baseline_metadata = cli_options.baseline_metadata;

    if (configure) {
        configure(options);
    }

    auto capture = SP::UI::Screenshot::CaptureDeclarative(space, scene, window, options);
    if (!capture) {
        return std::unexpected(capture.error());
    }
    if (!cli_options.allow_software_fallback && !options.force_software && !capture->hardware_capture) {
        return std::unexpected(SP::Error{SP::Error::Code::UnknownError,
                                         "hardware capture unavailable; rerun with --screenshot-force-software or --screenshot-allow-software-fallback"});
    }
    return {};
}

} // namespace SP::UI::Screenshot
