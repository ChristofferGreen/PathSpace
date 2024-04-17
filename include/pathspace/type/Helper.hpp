#pragma once
#include <parallel_hashmap/phmap.h>

namespace SP {
struct PathSpace;
using NodeDataHashMap = phmap::parallel_flat_hash_map<SP::ConcreteName, std::variant<SP::NodeData, std::unique_ptr<SP::PathSpace>>>;
} // namespace SP

namespace cereal {
template <class Archive, typename... Types>
void serialize(Archive& ar, SP::NodeDataHashMap &hashMap) {
    for (const auto& item : hashMap)
        ar(cereal::make_nvp(std::string(item.first.getName()), item.second));
}
} // namespace cereal