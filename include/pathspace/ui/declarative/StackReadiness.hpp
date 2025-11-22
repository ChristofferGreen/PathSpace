#pragma once

#include <pathspace/core/Error.hpp>

#include <chrono>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace SP {
class PathSpace;
}

namespace SP::UI::Declarative {

struct StackReadinessOptions {
    std::chrono::milliseconds timeout{0};
    std::chrono::milliseconds poll_interval{std::chrono::milliseconds{25}};
    bool verbose{false};
    std::function<void(std::string_view)> log;
};

auto WaitForStackChildren(SP::PathSpace& space,
                          std::string const& stack_root,
                          std::span<const std::string_view> required_children,
                          StackReadinessOptions const& options = {}) -> SP::Expected<void>;

} // namespace SP::UI::Declarative
