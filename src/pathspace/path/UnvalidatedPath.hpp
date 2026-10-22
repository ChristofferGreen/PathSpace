#pragma once

#include "core/Error.hpp"

#include <expected>
#include <string_view>
#include <vector>

namespace SP {

/**
 * Lightweight wrapper around a raw path string that has not been validated yet.
 * Utilities here capture the shared validation logic so higher layers can make
 * the handoff to ConcretePath* types explicit in their APIs.
 */
class UnvalidatedPathView {
public:
    explicit UnvalidatedPathView(std::string_view raw) noexcept;

    [[nodiscard]] auto raw() const noexcept -> std::string_view {
        return raw_;
    }

    [[nodiscard]] auto empty() const noexcept -> bool {
        return raw_.empty();
    }
    [[nodiscard]] auto is_absolute() const noexcept -> bool {
        return !raw_.empty() && raw_.front() == '/';
    }
    [[nodiscard]] auto has_trailing_slash() const noexcept -> bool {
        return !raw_.empty() && raw_.back() == '/';
    }

    [[nodiscard]] auto contains_relative_tokens() const noexcept -> bool;

    [[nodiscard]] auto split_absolute_components() const -> Expected<std::vector<std::string_view>>;

    [[nodiscard]] auto canonicalize_absolute() const -> Expected<std::string>;

private:
    std::string_view raw_;
};

} // namespace SP

