#pragma once

#include <set>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#include "SpacePath.hpp"

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
        c.capabilities[SpacePath("/*")].insert(Type::All);
        return c;
    }
private:
    std::map<SpacePath, std::set<Type>> capabilities;

public:
    void addCapability(const SpacePath& path, Type type) {
        capabilities[path].insert(type);
    }

    // Removes a capability from a specific path
    bool hasCapability(const SpacePath& path, Type capability) const {
        auto it = capabilities.find(path);
        if (it != capabilities.end()) {
            return it->second.find(capability) != it->second.end();
        }
        return false;
    }

    // Removes a capability from the specified path
    bool removeCapability(const SpacePath& path, Type capability) {
        auto it = capabilities.find(path);
        if (it != capabilities.end()) {
            return it->second.erase(capability) > 0;
        }
        return false;
    }
};

}