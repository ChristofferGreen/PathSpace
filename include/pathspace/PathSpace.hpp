#pragma once

#include <expected>
#include <string>
#include <unordered_map>
#include <vector>
#include <functional>
#include <chrono>
#include <memory>

#include "core/Error.hpp"
#include "core/Capabilities.hpp"
#include "core/TimeToLive.hpp"
#include "core/PathNode.hpp"
#include "core/SpacePath.hpp"
#include "core/ExecutionOptions.hpp"

namespace SP {

template<typename T>
using Expected = std::expected<T, Error>;

class PathSpace {
private:
    PathNode root;
    std::unordered_map<std::string, std::function<Expected<void>(const SpacePath&)>> subscribers;

public:
    template<typename T>
    auto insert(const SpacePath& path,
                const T& data,
                const Capabilities& capabilities = Capabilities::All(),
                const std::optional<TimeToLive>& ttl = std::nullopt) -> std::optional<Error> {
        if (!capabilities.hasCapability(path, Capabilities::Type::READ)) {
            return Error{Error::Code::CapabilityWriteMissing, "Write capability check failed"};
        }

        // Navigate to the correct node in the path hierarchy, or create it if it doesn't exist.
        /*auto node = navigateToNode(path); // Pseudo-function to demonstrate the idea
        if (!node) {
            // The navigateToNode would create the node if it doesn't exist, so if it's still nullptr, there's an error
            return Error{Error::Code::NO_SUCH_PATH, "Path does not exist and could not be created"};
        }

        // Insert the data
        node->insertData(data); // Pseudo-function for inserting data into the node

        // If a TTL is provided, setup the expiration mechanism
        if (ttl) {
            // setupTTL(node, *ttl); // Pseudo-function to demonstrate setting up the TTL for the node
        }*/

        return std::nullopt; // Return no error on success
    }

    // Read data from the PathSpace
    template<typename T>
    Expected<T> read(const SpacePath& path, const Capabilities& capabilities = Capabilities::All()) const;

    // Grab data from the PathSpace (read and remove)
    template<typename T>
    Expected<T> grab(const SpacePath& path, const Capabilities& capabilities = Capabilities::All());

    // Subscribe to changes at a given path
    Expected<void> subscribe(const SpacePath& path, std::function<void(const SpacePath&)> callback,
                             const Capabilities& capabilities = Capabilities::All());

    // Visit a node in the PathSpace
    template<typename T>
    Expected<void> visit(const SpacePath& path, std::function<void(T&)> visitor,
                         const Capabilities& capabilities = Capabilities::All());

    // More methods based on the described functionality can be added here
};

}