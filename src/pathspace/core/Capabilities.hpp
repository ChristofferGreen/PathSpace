#pragma once
#include <set>
#include <string>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#include "path/GlobPath.hpp"
#include "path/ConcretePath.hpp"

namespace SP {

struct Capabilities {
    enum class Type {
        READ,
        WRITE,
        EXECUTE,
        All
    };
    static auto All() -> Capabilities {
        Capabilities c;
        c.capabilities["/**"].insert(Type::All);
        return c;
    }

    void addCapability(char const * const path, Type const type) {
        this->capabilities[path].insert(type);
    }

    void addCapability(GlobPathString const &path, Type const type) {
        this->capabilities[path].insert(type);
    }

    auto hasCapability(ConcretePathStringView const &path, Type const capability) const -> bool {
        for (const auto &capabilityEntry : capabilities)
            if (capabilityEntry.first == path) // Check if the path matches using wildcard matching
                if (capabilityEntry.second.find(capability) != capabilityEntry.second.end()) // Check if the capability is present in the set for this path
                    return true;
        return false;
    }    

    bool removeCapability(GlobPathString const &path, Type const capability) {
        auto it = this->capabilities.find(path);
        if (it != this->capabilities.end())
            return it->second.erase(capability) > 0;
        return false;
    }
private:
    std::map<GlobPathString, std::set<Type>> capabilities;
};

}