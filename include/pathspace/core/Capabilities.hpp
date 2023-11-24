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
    void addCapability(SpacePath const &path, Type const type) {
        capabilities[path].insert(type);
    }

    auto hasCapability(SpacePath const &path, Type const capability) const -> bool {
        const auto it = SpacePath::findWithWildcard(capabilities, path);
        if (it != capabilities.end()) {
            return it->second.contains(capability);
        }
        return false;
    }

    bool removeCapability(const SpacePath& path, Type capability) {
        auto it = capabilities.find(path);
        if (it != capabilities.end()) {
            return it->second.erase(capability) > 0;
        }
        return false;
    }
};

}