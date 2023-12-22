#pragma once

#include "core/Error.hpp"
#include "core/Capabilities.hpp"
#include "core/TimeToLive.hpp"
#include "core/PathNode.hpp"
#include "core/ExecutionOptions.hpp"
#include "core/PathNode.hpp"
#include "serialization/InputData.hpp"

#include <parallel_hashmap/phmap.h>

#include <chrono>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace SP {

template<typename T>
using Expected = std::expected<T, Error>;

class PathSpace {
private:
    phmap::parallel_flat_hash_map<ConcreteName, std::unique_ptr<PathSpace>> nodes;
    //phmap::parallel_flat_hash_map<std::string, std::function<Expected<void>(const GlobPathStringView&)>> subscribers;

public:
    template<typename T>
    auto insert(const GlobPathStringView& path,
                const T& data,
                const Capabilities& capabilities = Capabilities::All(),
                const std::optional<TimeToLive>& ttl = std::nullopt) -> Expected<int> {
        InputData input{data};
        //if (!capabilities.hasCapability(path, Capabilities::Type::WRITE))
        //    return std::unexpected({Error::Code::CapabilityWriteMissing, "Write capability check failed"});
        int nbrInserted = 0;

        if(path.isConcrete()) {
            ConcretePathStringView cpath{path.getPath()};
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

        return nbrInserted;
    }

    template<typename T>
    auto read(const GlobPathStringView& path,
              const Capabilities& capabilities = Capabilities::All()) const -> Expected<T>;

    template<typename T>
    auto grab(const GlobPathStringView& path,
              const Capabilities& capabilities = Capabilities::All()) -> Expected<T>;

    auto subscribe(const GlobPathStringView& path,
                   std::function<void(const GlobPathStringView&)> callback,
                   const Capabilities& capabilities = Capabilities::All()) -> Expected<void>;

    template<typename T>
    auto visit(const GlobPathStringView& path,
               std::function<void(T&)> visitor,
               const Capabilities& capabilities = Capabilities::All()) -> Expected<void>;
};

}