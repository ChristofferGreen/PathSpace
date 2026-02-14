#include "PathView.hpp"
#include "core/Error.hpp"

#include <optional>
#include <string>

namespace SP {

namespace {

auto joinCanonical(std::string const& prefix, std::string const& suffix) -> std::string {
    if (prefix.empty() || prefix == "/") {
        return suffix.empty() ? std::string{"/"} : suffix;
    }
    if (suffix.empty() || suffix == "/") {
        return prefix;
    }
    std::string joined = prefix;
    bool prefixHasSlash = !joined.empty() && joined.back() == '/';
    bool suffixHasSlash = !suffix.empty() && suffix.front() == '/';
    if (prefixHasSlash && suffixHasSlash) {
        joined.pop_back();
    } else if (!prefixHasSlash && !suffixHasSlash) {
        joined.push_back('/');
    }
    joined.append(suffix);
    ConcretePathString canonical{joined};
    auto               normalized = canonical.canonicalized();
    if (normalized) {
        return normalized->getPath();
    }
    return joined;
}

auto stripPrefix(std::string const& absolute, std::string const& prefix) -> std::optional<std::string> {
    if (prefix.empty() || prefix == "/") {
        return absolute;
    }
    if (absolute.rfind(prefix, 0) == 0) {
        auto remainder = absolute.substr(prefix.size());
        if (remainder.empty()) {
            return std::string{"/"};
        }
        if (remainder.front() != '/') {
            return std::string{"/"} + remainder;
        }
        return remainder;
    }
    return std::nullopt;
}

} // namespace

auto PathView::in(Iterator const& path, InputData const& data) -> InsertReturn {
    if (!this->space)
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "PathSpace not set"}}};
    // Check write permission for the path
    auto perm = permission(path);
    if (!perm.write) {
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "Write permission denied for path: " + path.toString()}}};
    }

    // If we have a root path, prepend it
    Iterator fullPath = root.empty() ? path : Iterator{root + "/" + path.toString()};

    // Forward to base class implementation
    return this->space->in(fullPath, data);
}

auto PathView::out(Iterator const& path, InputMetadata const& inputMetadata, Out const& options, void* obj) -> std::optional<Error> {
    if (!this->space)
        return Error{Error::Code::InvalidPermissions, "PathSpace not set"};
    // Check read permission for the path
    auto perm = permission(path);
    if (!perm.read) {
        return Error{Error::Code::InvalidPermissions, "Read permission denied for path: " + path.toString()};
    }

    // If we have a root path, prepend it
    Iterator fullPath = root.empty() ? path : Iterator{root + "/" + path.toString()};

    // Forward to base class implementation
    return this->space->out(fullPath, inputMetadata, options, obj);
}

auto PathView::shutdown() -> void {
    if (!this->space)
        return;
    this->space->shutdown();
}

auto PathView::notify(std::string const& notificationPath) -> void {
    if (!this->space)
        return;
    this->space->notify(notificationPath);
}

auto PathView::visit(PathVisitor const& visitor, VisitOptions const& options) -> Expected<void> {
    if (!this->space) {
        return std::unexpected(Error{Error::Code::InvalidPermissions, "PathSpace not set"});
    }

    VisitOptions mapped = options;
    mapped.root        = joinCanonical(this->root, options.root);

    auto viewVisitor = [&](PathEntry const& upstreamEntry, ValueHandle& handle) -> VisitControl {
        auto viewPath = stripPrefix(upstreamEntry.path, this->root);
        if (!viewPath) {
            return VisitControl::SkipChildren;
        }
        Iterator iter{*viewPath};
        auto     perm = permission(iter);
        if (!perm.read) {
            return VisitControl::SkipChildren;
        }
        PathEntry remapped = upstreamEntry;
        remapped.path      = *viewPath;
        return visitor(remapped, handle);
    };

    return this->space->visit(viewVisitor, mapped);
}

auto PathView::getRootNode() -> Node* {
    if (!this->space)
        return nullptr;
    return this->space->getRootNode();
}

namespace testing {
auto joinCanonicalForTest(std::string const& prefix, std::string const& suffix) -> std::string {
    return joinCanonical(prefix, suffix);
}

auto stripPrefixForTest(std::string const& absolute, std::string const& prefix) -> std::optional<std::string> {
    return stripPrefix(absolute, prefix);
}
} // namespace testing

} // namespace SP
