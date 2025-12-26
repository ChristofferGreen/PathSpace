#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/ui/PathTypes.hpp>
#include <pathspace/ui/screenshot/ScreenshotService.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace SP::UI::Screenshot {

struct ScreenshotSlotPaths {
    std::string base;
    std::string token;
    std::string output_png;
    std::string baseline_png;
    std::string diff_png;
    std::string metrics_json;
    std::string capture_mode;
    std::string frame_index;
    std::string deadline_ns;
    std::string width;
    std::string height;
    std::string force_software;
    std::string allow_software_fallback;
    std::string present_when_force_software;
    std::string verify_output_matches_framebuffer;
    std::string verify_max_mean_error;
    std::string max_mean_error;
    std::string require_present;
    std::string armed;
    std::string status;
    std::string artifact;
    std::string mean_error;
    std::string backend;
    std::string completed_at_ns;
    std::string error;
};

struct ScreenshotSlotRequest {
    std::filesystem::path output_png;
    std::optional<std::filesystem::path> baseline_png;
    std::optional<std::filesystem::path> diff_png;
    std::optional<std::filesystem::path> metrics_json;
    std::string capture_mode;
    std::optional<std::uint64_t> frame_index;
    std::optional<std::uint64_t> deadline_ns;
    int width = 0;
    int height = 0;
    bool force_software = false;
    bool allow_software_fallback = false;
    bool present_when_force_software = false;
    bool require_present = false;
    bool verify_output_matches_framebuffer = false;
    double max_mean_error = 0.0015;
    std::optional<double> verify_max_mean_error;
};

struct ScreenshotSlotResult {
    std::string status;
    std::filesystem::path artifact;
    std::optional<double> mean_error;
    std::optional<std::string> backend;
    std::optional<std::uint64_t> completed_at_ns;
    std::optional<std::string> error;
};

struct SlotEphemeralData {
    SP::UI::Screenshot::BaselineMetadata baseline_metadata;
    std::function<SP::Expected<void>(std::filesystem::path const&,
                                     std::optional<std::filesystem::path> const&)> postprocess_png;
    bool verify_output_matches_framebuffer = false;
    std::optional<double> verify_max_mean_error;
};

class ScopedScreenshotToken {
public:
    ScopedScreenshotToken() = default;
    ScopedScreenshotToken(PathSpace* space, std::string path, bool held);
    ScopedScreenshotToken(ScopedScreenshotToken&& other) noexcept;
    ScopedScreenshotToken& operator=(ScopedScreenshotToken&& other) noexcept;
    ScopedScreenshotToken(ScopedScreenshotToken const&) = delete;
    ScopedScreenshotToken& operator=(ScopedScreenshotToken const&) = delete;
    ~ScopedScreenshotToken();

    [[nodiscard]] bool held() const { return held_; }
    void release();

private:
    PathSpace* space_ = nullptr;
    std::string path_;
    bool held_ = false;
};

[[nodiscard]] auto AcquireScreenshotToken(PathSpace& space,
                                          std::string const& token_path,
                                          std::chrono::milliseconds timeout)
    -> SP::Expected<ScopedScreenshotToken>;

[[nodiscard]] auto MakeScreenshotSlotPaths(SP::UI::WindowPath const& window,
                                           std::string const& view_name) -> ScreenshotSlotPaths;

void RegisterSlotEphemeral(std::string const& slot_base, SlotEphemeralData data);
[[nodiscard]] auto ConsumeSlotEphemeral(std::string const& slot_base) -> std::optional<SlotEphemeralData>;

[[nodiscard]] auto WriteScreenshotSlotRequest(PathSpace& space,
                                              ScreenshotSlotPaths const& paths,
                                              ScreenshotSlotRequest const& request) -> SP::Expected<void>;

[[nodiscard]] auto WaitForScreenshotSlotResult(PathSpace& space,
                                               ScreenshotSlotPaths const& paths,
                                               std::chrono::milliseconds timeout) -> SP::Expected<ScreenshotSlotResult>;

[[nodiscard]] auto ReadScreenshotSlotRequest(PathSpace& space,
                                             ScreenshotSlotPaths const& paths,
                                             int default_width,
                                             int default_height) -> SP::Expected<std::optional<ScreenshotSlotRequest>>;

[[nodiscard]] auto WriteScreenshotSlotResult(PathSpace& space,
                                             ScreenshotSlotPaths const& paths,
                                             ScreenshotResult const& result,
                                             std::string_view status,
                                             std::string_view backend,
                                             std::optional<std::string> error_message) -> SP::Expected<void>;

[[nodiscard]] auto WriteScreenshotSlotTimeout(PathSpace& space,
                                              ScreenshotSlotPaths const& paths,
                                              std::string_view backend,
                                              std::string_view error_message) -> SP::Expected<void>;

} // namespace SP::UI::Screenshot
