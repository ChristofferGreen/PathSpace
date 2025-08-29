#pragma once
#include "PathSpace.hpp"
#include "core/InsertReturn.hpp"
#include "type/InputData.hpp"
#include "path/Iterator.hpp"
#include "core/Error.hpp"

#include <mutex>
#include <string>

namespace SP {

/**
 * PathIOStdOut — a simple sink that prints inserted strings to stdout.
 *
 * Usage:
 * - Write strings via insert at any relative path; the provider prints to stdout with optional prefix/newline.
 * - Reads are unsupported and return InvalidPermissions.
 * - Thread-safe printing: a local mutex guards access to std::cout to avoid interleaving.
 */
class PathIOStdOut final : public PathSpaceBase {
public:
    explicit PathIOStdOut(bool addNewline = true, std::string prefix = {})
        : addNewline_(addNewline), prefix_(std::move(prefix)) {}

    // Print inserted strings to stdout; reject non-string types.
    auto in(Iterator const& path, InputData const& data) -> InsertReturn override;

    // out()/shutdown()/notify overrides
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> override;
    auto shutdown() -> void override;
    auto notify(std::string const& notificationPath) -> void override;

private:
    bool        addNewline_{true};
    std::string prefix_;
    std::mutex  mutex_;
};

} // namespace SP