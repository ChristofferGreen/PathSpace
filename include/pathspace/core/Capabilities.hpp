#pragma once

#include <set>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

#include "GlobPath.hpp"

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
        c.capabilities[GlobPath("/*")].insert(Type::All);
        return c;
    }
private:
    std::map<GlobPath, std::set<Type>> capabilities;

public:
    void addCapability(GlobPath const &path, Type const type) {
        capabilities[path].insert(type);
    }

    auto hasCapability(GlobPath const &path, Type const capability) const -> bool {
        /*const auto it = Path::findWithWildcard(capabilities, path);
        if (it != capabilities.end()) {
            return it->second.contains(capability);
        }*/
        return false;
    }

    bool removeCapability(const GlobPath& path, Type capability) {
        auto it = capabilities.find(path);
        if (it != capabilities.end()) {
            return it->second.erase(capability) > 0;
        }
        return false;
    }
};

}