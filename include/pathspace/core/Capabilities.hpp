#pragma once

#include <set>
#include <string>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#include "GlobPath.hpp"
#include "Path.hpp"

namespace SP {

class Capabilities {
public:
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
private:
    std::map<std::string, std::set<Type>> capabilities;

public:
    void addCapability(char const * const path, Type const type) {
        this->capabilities[path].insert(type);
    }

    void addCapability(GlobPath const &path, Type const type) {
        this->capabilities[path.toString()].insert(type);
    }

    auto hasCapability(Path const &path, Type const capability) const -> bool {
        for (const auto &capabilityEntry : capabilities)
            if (GlobPath{capabilityEntry.first} == path) // Check if the path matches using wildcard matching
                if (capabilityEntry.second.find(capability) != capabilityEntry.second.end()) // Check if the capability is present in the set for this path
                    return true;
        return false;
    }    

    bool removeCapability(const GlobPath& path, Type capability) {
        auto it = this->capabilities.find(path.toString());
        if (it != this->capabilities.end())
            return it->second.erase(capability) > 0;
        return false;
    }
};

}