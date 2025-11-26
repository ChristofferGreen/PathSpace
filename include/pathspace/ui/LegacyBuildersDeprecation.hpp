#pragma once

#include <pathspace/PathSpace.hpp>
#include <pathspace/app/AppPaths.hpp>
#include <pathspace/path/ConcretePath.hpp>

#include <optional>
#include <string_view>

namespace SP::UI::LegacyBuilders {

class ScopedAllow {
public:
    ScopedAllow();
    ScopedAllow(ScopedAllow const&) = delete;
    ScopedAllow(ScopedAllow&&) = delete;
    auto operator=(ScopedAllow const&) -> ScopedAllow& = delete;
    auto operator=(ScopedAllow&&) -> ScopedAllow& = delete;
    ~ScopedAllow();
};

class ScopedAllowAllThreads {
public:
    ScopedAllowAllThreads();
    ScopedAllowAllThreads(ScopedAllowAllThreads const&) = delete;
    ScopedAllowAllThreads(ScopedAllowAllThreads&&) = delete;
    auto operator=(ScopedAllowAllThreads const&) -> ScopedAllowAllThreads& = delete;
    auto operator=(ScopedAllowAllThreads&&) -> ScopedAllowAllThreads& = delete;
    ~ScopedAllowAllThreads();
};

auto NoteUsage(PathSpace& space,
               std::string_view entry_point,
               std::optional<SP::ConcretePathStringView> path_hint = std::nullopt) -> SP::Expected<void>;

auto NoteUsage(PathSpace const& space,
               std::string_view entry_point,
               std::optional<SP::ConcretePathStringView> path_hint = std::nullopt) -> SP::Expected<void>;

auto SupportWindowDeadline() -> std::string_view;

} // namespace SP::UI::LegacyBuilders

#define PATHSPACE_LEGACY_BUILDER_GUARD(space, entry)                                                         \
    if (auto _legacy_builder_status = ::SP::UI::LegacyBuilders::NoteUsage(space, entry); !_legacy_builder_status) { \
        return std::unexpected(_legacy_builder_status.error());                                              \
    }                                                                                                        \
    static_assert(true, "consume trailing semicolon")
