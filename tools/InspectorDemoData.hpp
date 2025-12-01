#pragma once

#include "PathSpace.hpp"

#include <cstdint>
#include <string>

namespace SP::Inspector {

inline auto SeedInspectorDemoData(PathSpace& space) -> void {
    auto insert_string = [&](std::string const& path, std::string value) {
        auto result = space.insert(path, std::move(value));
        (void)result;
    };
    auto insert_uint = [&](std::string const& path, std::uint64_t value) {
        auto result = space.insert(path, value);
        (void)result;
    };
    auto insert_bool = [&](std::string const& path, bool value) {
        auto result = space.insert(path, value);
        (void)result;
    };

    insert_string("/demo/widgets/button/meta/label", "Declarative Button");
    insert_bool("/demo/widgets/button/state/enabled", true);
    insert_string("/demo/widgets/button/log/lastPress", "n/a");
    insert_uint("/demo/widgets/slider/state/value", 42);
    insert_uint("/demo/widgets/slider/state/range/min", 0);
    insert_uint("/demo/widgets/slider/state/range/max", 100);
    insert_string("/demo/widgets/list/items/alpha/meta/label", "Alpha");
    insert_string("/demo/widgets/list/items/beta/meta/label", "Beta");
    insert_string("/demo/widgets/list/items/gamma/meta/label", "Gamma");
    insert_bool("/demo/widgets/list/items/beta/state/selected", true);
    insert_uint("/demo/metrics/widgets_total", 5);
}

} // namespace SP::Inspector
