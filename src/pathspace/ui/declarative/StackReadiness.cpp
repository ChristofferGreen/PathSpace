#include <pathspace/ui/declarative/StackReadiness.hpp>

#include <pathspace/PathSpace.hpp>
#include <pathspace/path/ConcretePath.hpp>
#include <pathspace/ui/WidgetSharedTypes.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr auto kVerboseEnvFlags = std::to_array({"PATHSPACE_UI_DEBUG_STACK_LAYOUT",
                                                 "PAINT_EXAMPLE_DEBUG_LAYOUT"});

auto is_truthy_env(char const* value) -> bool {
    if (value == nullptr) {
        return false;
    }
    if (*value == '\0') {
        return true;
    }
    std::string_view view{value};
    if (view == "0" || view == "false" || view == "FALSE" || view == "off" || view == "OFF") {
        return false;
    }
    return true;
}

auto env_verbose_enabled() -> bool {
    for (auto const* flag : kVerboseEnvFlags) {
        if (is_truthy_env(std::getenv(flag))) {
            return true;
        }
    }
    return false;
}

auto log_message(SP::UI::Declarative::StackReadinessOptions const& options,
                 bool verbose,
                 std::string const& message) -> void {
    if (!verbose) {
        return;
    }
    if (options.log) {
        options.log(message);
        return;
    }
    std::cerr << message << '\n';
}

auto format_missing(std::string const& children_root,
                    std::span<const std::string_view> missing) -> std::string {
    std::string line = "waiting for stack children at '";
    line.append(children_root);
    line.append("', missing");
    for (auto child : missing) {
        line.push_back(' ');
        line.append(child.data(), child.size());
    }
    return line;
}

} // namespace

namespace SP::UI::Declarative {

auto WaitForStackChildren(SP::PathSpace& space,
                          std::string const& stack_root,
                          std::span<const std::string_view> required_children,
                          StackReadinessOptions const& options) -> SP::Expected<void> {
    if (required_children.empty()) {
        return {};
    }

    auto verbose = options.verbose || env_verbose_enabled();
    auto poll_interval = options.poll_interval.count() > 0 ? options.poll_interval
                                                          : std::chrono::milliseconds{25};
    auto timeout = options.timeout.count() > 0 ? options.timeout : poll_interval;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<std::string_view> last_missing;
    last_missing.reserve(required_children.size());

    auto compute_missing = [&](std::vector<std::string> const& children) {
        std::vector<std::string_view> missing;
        missing.reserve(required_children.size());
        for (auto child : required_children) {
            auto it = std::find_if(children.begin(),
                                   children.end(),
                                   [&](std::string const& name) { return name == child; });
            if (it == children.end()) {
                missing.push_back(child);
            }
        }
        return missing;
    };

    while (std::chrono::steady_clock::now() < deadline) {
        auto view = SP::UI::Runtime::Widgets::WidgetChildren(space, stack_root);
        auto missing = compute_missing(view.names);
        if (missing.empty()) {
            log_message(options,
                        verbose,
                        "stack ready at '" + view.root + "' with "
                            + std::to_string(view.names.size()) + " children");
            return {};
        }
        if (verbose && missing != last_missing) {
            log_message(options, verbose, format_missing(view.root, missing));
            last_missing = missing;
        }
        std::this_thread::sleep_for(poll_interval);
    }

    auto view = SP::UI::Runtime::Widgets::WidgetChildren(space, stack_root);
    auto missing = compute_missing(view.names);
    auto active_root = view.root.empty()
                           ? SP::UI::Runtime::Widgets::WidgetChildrenPath(stack_root)
                           : view.root;
    auto message = format_missing(active_root, missing);
    log_message(options, verbose, message);
    return std::unexpected(SP::Error{SP::Error::Code::Timeout, std::move(message)});
}

} // namespace SP::UI::Declarative
