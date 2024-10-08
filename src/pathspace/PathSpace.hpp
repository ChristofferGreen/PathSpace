#pragma once

#include "PathSpaceBase.hpp"
#include "PathSpaceLeaf.hpp"

namespace SP {
class PathSpace : public PathSpaceBase {
public:
protected:
    virtual InsertReturn insertImpl(GlobPathStringView const& path, InputData const& data, InOptions const& options) override {
        InsertReturn ret;
        if (!path.isValid()) {
            ret.errors.emplace_back(Error::Code::InvalidPath, std::string("The path was not valid: ").append(path.getPath()));
            return ret;
        }
        bool const isConcretePath = path.isConcrete();
        auto constructedPath = isConcretePath ? ConstructiblePath{path} : ConstructiblePath{};
        if (!this->inFunctionPointer(isConcretePath, constructedPath, data, options))
            this->inInternal(constructedPath, path.begin(), path.end(), InputData{data}, options, ret);
        return ret;
    };

    virtual auto readImpl(ConcretePathStringView const& path,
                          InputMetadata const& inputMetadata,
                          OutOptions const& options,
                          Capabilities const& capabilities,
                          void* obj) const -> Expected<int> {
        // ToDo: Make sure options.doPop is set to false
        return const_cast<PathSpace*>(this)->outInternal(path.begin(), path.end(), inputMetadata, obj, options, capabilities);
    }
};

} // namespace SP