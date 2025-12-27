#include "ScreenshotSlot.hpp"

#include <pathspace/core/Error.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <type_traits>
#include <thread>
#include <utility>
#include <mutex>
#include <unordered_map>

using namespace std::chrono_literals;

namespace {

auto make_error(std::string message, SP::Error::Code code = SP::Error::Code::UnknownError) -> SP::Error {
    return SP::Error{code, std::move(message)};
}

bool is_not_found(SP::Error const& error) {
    return error.code == SP::Error::Code::NoSuchPath || error.code == SP::Error::Code::NoObjectFound;
}

template <typename T>
auto replace_value(SP::PathSpace& space, std::string const& path, T&& value) -> SP::Expected<void> {
    while (true) {
        auto removed = space.take<std::remove_cvref_t<T>>(path, SP::Pop{});
        if (!removed) {
            auto const& error = removed.error();
            if (is_not_found(error) || error.code == SP::Error::Code::InvalidType) {
                break;
            }
            return std::unexpected(error);
        }
    }
    auto inserted = space.insert(path, std::forward<T>(value));
    if (!inserted.errors.empty()) {
        return std::unexpected(inserted.errors.front());
    }
    return {};
}

template <typename T>
auto clear_value(SP::PathSpace& space, std::string const& path) -> void {
    auto removed = space.take<T>(path);
    if (!removed) {
        auto const& error = removed.error();
        if (!is_not_found(error) && error.code != SP::Error::Code::InvalidType) {
            // Swallow benign "not found" cases; other errors propagate via logs.
        }
    }
}

} // namespace

namespace SP::UI::Screenshot {

namespace {

std::mutex g_slot_ephemeral_mutex;
std::unordered_map<std::string, SlotEphemeralData> g_slot_ephemeral;

} // namespace

ScopedScreenshotToken::ScopedScreenshotToken(PathSpace* space, std::string path, bool held)
    : space_(space)
    , path_(std::move(path))
    , held_(held) {}

ScopedScreenshotToken::ScopedScreenshotToken(ScopedScreenshotToken&& other) noexcept
    : space_(other.space_)
    , path_(std::move(other.path_))
    , held_(other.held_) {
    other.space_ = nullptr;
    other.held_ = false;
}

ScopedScreenshotToken& ScopedScreenshotToken::operator=(ScopedScreenshotToken&& other) noexcept {
    if (this != &other) {
        release();
        space_ = other.space_;
        path_ = std::move(other.path_);
        held_ = other.held_;
        other.space_ = nullptr;
        other.held_ = false;
    }
    return *this;
}

ScopedScreenshotToken::~ScopedScreenshotToken() { release(); }

void ScopedScreenshotToken::release() {
    if (space_ && held_) {
        auto inserted = space_->insert(path_, true);
        (void)inserted;
    }
    held_ = false;
    space_ = nullptr;
    path_.clear();
}

auto AcquireScreenshotToken(PathSpace& space,
                            std::string const& token_path,
                            std::chrono::milliseconds timeout) -> SP::Expected<ScopedScreenshotToken> {
    auto ensure = space.insert(token_path, true);
    if (!ensure.errors.empty()) {
        auto const& error = ensure.errors.front();
        if (error.code != SP::Error::Code::InvalidType) {
            return std::unexpected(error);
        }
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::chrono::milliseconds attempt_window = std::chrono::milliseconds{50};

    while (std::chrono::steady_clock::now() < deadline) {
        auto now = std::chrono::steady_clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        if (remaining <= 0ms) {
            break;
        }
        auto window = std::min(remaining, attempt_window);
        auto token = space.take<bool>(token_path, SP::Block{window});
        if (token) {
            return ScopedScreenshotToken{&space, token_path, true};
        }
        auto const& error = token.error();
        if (is_not_found(error)) {
            auto reset = space.insert(token_path, true);
            if (!reset.errors.empty() && !is_not_found(reset.errors.front())) {
                return std::unexpected(reset.errors.front());
            }
            continue;
        }
        if (error.code == SP::Error::Code::Timeout) {
            continue;
        }
        return std::unexpected(error);
    }

    return std::unexpected(make_error("failed to acquire screenshot token before timeout", SP::Error::Code::Timeout));
}

auto MakeScreenshotSlotPaths(SP::UI::WindowPath const& window, std::string const& view_name)
    -> ScreenshotSlotPaths {
    ScreenshotSlotPaths paths{};
    paths.base = std::string{"/ui/screenshot"} + window.getPath() + "/" + view_name;
    paths.token = paths.base + "/token";
    paths.output_png = paths.base + "/output_png";
    paths.baseline_png = paths.base + "/baseline_png";
    paths.diff_png = paths.base + "/diff_png";
    paths.metrics_json = paths.base + "/metrics_json";
    paths.capture_mode = paths.base + "/capture_mode";
    paths.frame_index = paths.base + "/frame_index";
    paths.deadline_ns = paths.base + "/deadline_ns";
    paths.width = paths.base + "/width";
    paths.height = paths.base + "/height";
    paths.force_software = paths.base + "/force_software";
    paths.allow_software_fallback = paths.base + "/allow_software_fallback";
    paths.present_when_force_software = paths.base + "/present_when_force_software";
    paths.verify_output_matches_framebuffer = paths.base + "/verify_output_matches_framebuffer";
    paths.verify_max_mean_error = paths.base + "/verify_max_mean_error";
    paths.max_mean_error = paths.base + "/max_mean_error";
    paths.require_present = paths.base + "/require_present";
    paths.armed = paths.base + "/armed";
    paths.status = paths.base + "/status";
    paths.artifact = paths.base + "/artifact";
    paths.mean_error = paths.base + "/mean_error";
    paths.backend = paths.base + "/backend";
    paths.completed_at_ns = paths.base + "/completed_at_ns";
    paths.error = paths.base + "/error";
    return paths;
}

void RegisterSlotEphemeral(std::string const& slot_base, SlotEphemeralData data) {
    std::lock_guard<std::mutex> lock(g_slot_ephemeral_mutex);
    g_slot_ephemeral[slot_base] = std::move(data);
}

auto ConsumeSlotEphemeral(std::string const& slot_base) -> std::optional<SlotEphemeralData> {
    std::lock_guard<std::mutex> lock(g_slot_ephemeral_mutex);
    auto it = g_slot_ephemeral.find(slot_base);
    if (it == g_slot_ephemeral.end()) {
        return std::nullopt;
    }
    auto data = std::move(it->second);
    g_slot_ephemeral.erase(it);
    return data;
}

auto WriteScreenshotSlotRequest(PathSpace& space,
                                ScreenshotSlotPaths const& paths,
                                ScreenshotSlotRequest const& request) -> SP::Expected<void> {
    // Clear optional lanes from previous runs.
    clear_value<std::uint64_t>(space, paths.frame_index);
    clear_value<std::uint64_t>(space, paths.deadline_ns);
    clear_value<std::string>(space, paths.baseline_png);
    clear_value<std::string>(space, paths.diff_png);
    clear_value<std::string>(space, paths.metrics_json);
    clear_value<double>(space, paths.verify_max_mean_error);
    clear_value<double>(space, paths.mean_error);
    clear_value<std::string>(space, paths.error);

    auto required = replace_value(space, paths.output_png, request.output_png.string());
    if (!required) {
        return required;
    }
    if (request.baseline_png) {
        if (auto status = replace_value(space, paths.baseline_png, request.baseline_png->string()); !status) {
            return status;
        }
    }
    if (request.diff_png) {
        if (auto status = replace_value(space, paths.diff_png, request.diff_png->string()); !status) {
            return status;
        }
    }
    if (request.metrics_json) {
        if (auto status = replace_value(space, paths.metrics_json, request.metrics_json->string()); !status) {
            return status;
        }
    }

    if (auto status = replace_value(space, paths.capture_mode, request.capture_mode); !status) {
        return status;
    }
    if (request.frame_index) {
        if (auto status = replace_value(space, paths.frame_index, *request.frame_index); !status) {
            return status;
        }
    }
    if (request.deadline_ns) {
        if (auto status = replace_value(space, paths.deadline_ns, *request.deadline_ns); !status) {
            return status;
        }
    }

    if (auto status = replace_value(space, paths.width, request.width); !status) {
        return status;
    }
    if (auto status = replace_value(space, paths.height, request.height); !status) {
        return status;
    }

    if (auto status = replace_value(space, paths.force_software, request.force_software); !status) {
        return status;
    }
    if (auto status = replace_value(space, paths.allow_software_fallback, request.allow_software_fallback); !status) {
        return status;
    }
    if (auto status = replace_value(space, paths.present_when_force_software, request.present_when_force_software); !status) {
        return status;
    }
    if (auto status = replace_value(space, paths.verify_output_matches_framebuffer, request.verify_output_matches_framebuffer); !status) {
        return status;
    }
    if (auto status = replace_value(space, paths.require_present, request.require_present); !status) {
        return status;
    }
    if (auto status = replace_value(space, paths.max_mean_error, request.max_mean_error); !status) {
        return status;
    }
    if (request.verify_max_mean_error) {
        if (auto status = replace_value(space, paths.verify_max_mean_error, *request.verify_max_mean_error); !status) {
            return status;
        }
    }

    if (auto status = replace_value(space, paths.status, std::string{"pending"}); !status) {
        return status;
    }
    // Arm last so the presenter only observes fully-written requests.
    if (auto status = replace_value(space, paths.armed, true); !status) {
        return status;
    }

    return {};
}

auto WaitForScreenshotSlotResult(PathSpace& space,
                                 ScreenshotSlotPaths const& paths,
                                 std::chrono::milliseconds timeout) -> SP::Expected<ScreenshotSlotResult> {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    ScreenshotSlotResult result{};

    while (true) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            (void)WriteScreenshotSlotTimeout(space,
                                             paths,
                                             std::string_view{"unknown"},
                                             std::string_view{"screenshot request timed out"});
            return std::unexpected(make_error("screenshot request timed out", SP::Error::Code::Timeout));
        }
        auto status_value = space.read<std::string, std::string>(paths.status);
        if (!status_value) {
            auto const& error = status_value.error();
            if (is_not_found(error)) {
                std::this_thread::sleep_for(std::chrono::milliseconds{10});
                continue;
            }
            return std::unexpected(error);
        }
        if (status_value->empty() || *status_value == "pending") {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
            continue;
        }
        result.status = *status_value;
        break;
    }

    auto artifact = space.read<std::string, std::string>(paths.artifact);
    if (artifact && !artifact->empty()) {
        result.artifact = *artifact;
    } else if (!artifact) {
        auto const& error = artifact.error();
        if (!is_not_found(error)) {
            return std::unexpected(error);
        }
    }

    auto mean_error = space.read<double>(paths.mean_error);
    if (mean_error) {
        result.mean_error = *mean_error;
    } else if (!is_not_found(mean_error.error())) {
        return std::unexpected(mean_error.error());
    }

    auto backend = space.read<std::string, std::string>(paths.backend);
    if (backend && !backend->empty()) {
        result.backend = *backend;
    } else if (!backend && !is_not_found(backend.error())) {
        return std::unexpected(backend.error());
    }

    auto completed_at = space.read<std::uint64_t>(paths.completed_at_ns);
    if (completed_at) {
        result.completed_at_ns = *completed_at;
    } else if (!is_not_found(completed_at.error())) {
        return std::unexpected(completed_at.error());
    }

    auto error_text = space.read<std::string, std::string>(paths.error);
    if (error_text && !error_text->empty()) {
        result.error = *error_text;
    } else if (!error_text && !is_not_found(error_text.error())) {
        return std::unexpected(error_text.error());
    }

    return result;
}

auto ReadScreenshotSlotRequest(PathSpace& space,
                               ScreenshotSlotPaths const& paths,
                               int default_width,
                               int default_height) -> SP::Expected<std::optional<ScreenshotSlotRequest>> {
    auto armed = space.read<bool>(paths.armed);
    if (!armed) {
        auto const& error = armed.error();
        if (is_not_found(error)) {
            return std::optional<ScreenshotSlotRequest>{};
        }
        return std::unexpected(error);
    }
    if (!*armed) {
        return std::optional<ScreenshotSlotRequest>{};
    }

    ScreenshotSlotRequest request{};
    request.capture_mode = "next_present";
    request.width = default_width;
    request.height = default_height;
    request.verify_output_matches_framebuffer = true;
    request.max_mean_error = 0.0015;

    auto output = space.read<std::string, std::string>(paths.output_png);
    if (!output) {
        return std::unexpected(output.error());
    }
    if (output->empty()) {
        return std::unexpected(make_error("screenshot request missing output_png", SP::Error::Code::InvalidPath));
    }
    request.output_png = std::filesystem::path{*output};

    if (auto baseline = space.read<std::string, std::string>(paths.baseline_png)) {
        if (!baseline->empty()) {
            request.baseline_png = std::filesystem::path{*baseline};
        }
    } else if (!is_not_found(baseline.error())) {
        return std::unexpected(baseline.error());
    }

    if (auto diff = space.read<std::string, std::string>(paths.diff_png)) {
        if (!diff->empty()) {
            request.diff_png = std::filesystem::path{*diff};
        }
    } else if (!is_not_found(diff.error())) {
        return std::unexpected(diff.error());
    }

    if (auto metrics = space.read<std::string, std::string>(paths.metrics_json)) {
        if (!metrics->empty()) {
            request.metrics_json = std::filesystem::path{*metrics};
        }
    } else if (!is_not_found(metrics.error())) {
        return std::unexpected(metrics.error());
    }

    if (auto capture_mode = space.read<std::string, std::string>(paths.capture_mode)) {
        if (!capture_mode->empty()) {
            request.capture_mode = *capture_mode;
        }
    } else if (!is_not_found(capture_mode.error())) {
        return std::unexpected(capture_mode.error());
    }

    if (auto frame_index = space.read<std::uint64_t>(paths.frame_index)) {
        request.frame_index = *frame_index;
    } else if (!is_not_found(frame_index.error())) {
        return std::unexpected(frame_index.error());
    }

    if (auto deadline = space.read<std::uint64_t>(paths.deadline_ns)) {
        request.deadline_ns = *deadline;
    } else if (!is_not_found(deadline.error())) {
        return std::unexpected(deadline.error());
    }

    if (auto width = space.read<int>(paths.width)) {
        request.width = *width;
    } else if (!is_not_found(width.error())) {
        return std::unexpected(width.error());
    }
    if (auto height = space.read<int>(paths.height)) {
        request.height = *height;
    } else if (!is_not_found(height.error())) {
        return std::unexpected(height.error());
    }

    if (auto force_sw = space.read<bool>(paths.force_software)) {
        request.force_software = *force_sw;
    } else if (!is_not_found(force_sw.error())) {
        return std::unexpected(force_sw.error());
    }
    if (auto allow_sw = space.read<bool>(paths.allow_software_fallback)) {
        request.allow_software_fallback = *allow_sw;
    } else if (!is_not_found(allow_sw.error())) {
        return std::unexpected(allow_sw.error());
    }
    if (auto present_when_force = space.read<bool>(paths.present_when_force_software)) {
        request.present_when_force_software = *present_when_force;
    } else if (!is_not_found(present_when_force.error())) {
        return std::unexpected(present_when_force.error());
    }
    if (auto require_present = space.read<bool>(paths.require_present)) {
        request.require_present = *require_present;
    } else if (!is_not_found(require_present.error())) {
        return std::unexpected(require_present.error());
    }
    if (auto verify_output = space.read<bool>(paths.verify_output_matches_framebuffer)) {
        request.verify_output_matches_framebuffer = *verify_output;
    } else if (!is_not_found(verify_output.error())) {
        return std::unexpected(verify_output.error());
    }

    if (auto max_mean_error = space.read<double>(paths.max_mean_error)) {
        request.max_mean_error = *max_mean_error;
    } else if (!is_not_found(max_mean_error.error())) {
        return std::unexpected(max_mean_error.error());
    }
    if (auto verify_mean = space.read<double>(paths.verify_max_mean_error)) {
        request.verify_max_mean_error = *verify_mean;
    } else if (!is_not_found(verify_mean.error())) {
        return std::unexpected(verify_mean.error());
    }

    return std::optional<ScreenshotSlotRequest>{request};
}

auto WriteScreenshotSlotResult(PathSpace& space,
                               ScreenshotSlotPaths const& paths,
                               ScreenshotResult const& result,
                               std::string_view status,
                               std::string_view backend,
                               std::optional<std::string> error_message) -> SP::Expected<void> {
    auto completed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());

    if (auto status_write = replace_value(space, paths.status, std::string{status}); !status_write) {
        return status_write;
    }
    if (auto artifact_write = replace_value(space, paths.artifact, result.artifact.string()); !artifact_write) {
        return artifact_write;
    }
    if (auto backend_write = replace_value(space, paths.backend, std::string{backend}); !backend_write) {
        return backend_write;
    }
    if (result.mean_error) {
        if (auto mean_write = replace_value(space, paths.mean_error, *result.mean_error); !mean_write) {
            return mean_write;
        }
    }
    if (auto completed_write = replace_value(space, paths.completed_at_ns, static_cast<std::uint64_t>(completed.count()));
        !completed_write) {
        return completed_write;
    }
    if (error_message) {
        if (auto err_write = replace_value(space, paths.error, *error_message); !err_write) {
            return err_write;
        }
    }
    if (auto clear_armed = replace_value(space, paths.armed, false); !clear_armed) {
        return clear_armed;
    }
    return {};
}

auto WriteScreenshotSlotTimeout(PathSpace& space,
                                ScreenshotSlotPaths const& paths,
                                std::string_view backend,
                                std::string_view error_message) -> SP::Expected<void> {
    ScreenshotResult empty{};
    empty.artifact = std::filesystem::path{};
    return WriteScreenshotSlotResult(space,
                                     paths,
                                     empty,
                                     "timeout",
                                     backend,
                                     std::string{error_message});
}

} // namespace SP::UI::Screenshot
