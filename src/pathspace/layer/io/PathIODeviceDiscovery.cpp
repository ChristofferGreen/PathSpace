#include "layer/io/PathIODeviceDiscovery.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace SP {

void PathIODeviceDiscovery::addSimulatedDevice(std::string cls, SimDevice dev) {
    auto norm = normalizeClass_(std::move(cls));
    {
        std::lock_guard<std::mutex> lg(mutex_);
        devices_[norm][dev.id] = std::move(dev);
    }
    notifyUpdates_(norm);
}

void PathIODeviceDiscovery::removeSimulatedDevice(std::string cls, int id) {
    auto norm = normalizeClass_(std::move(cls));
    bool changed = false;
    {
        std::lock_guard<std::mutex> lg(mutex_);
        auto it = devices_.find(norm);
        if (it != devices_.end()) {
            auto& inner = it->second;
            auto  jt    = inner.find(id);
            if (jt != inner.end()) {
                inner.erase(jt);
                changed = true;
            }
            if (inner.empty()) {
                devices_.erase(it);
            }
        }
    }
    if (changed) {
        notifyUpdates_(norm);
    }
}

void PathIODeviceDiscovery::clearAll() {
    {
        std::lock_guard<std::mutex> lg(mutex_);
        devices_.clear();
    }
    notifyUpdates_({});
}

auto PathIODeviceDiscovery::out(Iterator const& path,
                                InputMetadata const& inputMetadata,
                                Out const& /*options*/,
                                void* obj) -> std::optional<Error> {
    // Only std::string reads are supported
    if (inputMetadata.typeInfo != &typeid(std::string)) {
        return Error{Error::Code::TypeMismatch, "PathIODeviceDiscovery only supports std::string reads"};
    }
    if (obj == nullptr) {
        return Error{Error::Code::MalformedInput, "Null output pointer for PathIODeviceDiscovery::out"};
    }

    // Use the iterator tail (current->end) to support nested mounts correctly.
    std::string tail = std::string(path.currentToEnd()); // may be "" for root beneath mount

    // Normalize: strip leading slash
    std::string rel;
    if (!tail.empty() && tail.front() == '/')
        rel = tail.substr(1);
    else
        rel = tail;

    // Split components
    std::vector<std::string> parts;
    {
        size_t i = 0;
        while (i < rel.size()) {
            size_t j = rel.find('/', i);
            if (j == std::string::npos) {
                parts.emplace_back(rel.substr(i));
                break;
            } else {
                parts.emplace_back(rel.substr(i, j - i));
                i = j + 1;
            }
        }
        // If rel is empty (root), parts stays empty
        if (parts.size() == 1 && parts[0].empty()) parts.clear();
    }

    std::string outStr;
    if (parts.empty()) {
        // Root: list classes present
        outStr = listClasses_();
    } else if (parts.size() == 1) {
        // Class: list device IDs
        outStr = listDeviceIds_(parts[0]);
        if (outStr.empty()) {
            // If mounted under a parent space (mountPrefix_ set), treat single-component path
            // as a request for the root class listing (useful for outMinimal under nested spaces).
            std::string mountCopy;
            {
                std::lock_guard<std::mutex> lg(mutex_);
                mountCopy = mountPrefix_;
            }
            if (!mountCopy.empty()) {
                outStr = listClasses_();
            } else {
                return Error{Error::Code::NotFound, "No devices found for class: " + parts[0]};
            }
        }
    } else if (parts.size() == 3) {
        // Class / id / {meta|capabilities}
        auto const& cls   = parts[0];
        auto const& idStr = parts[1];
        auto const& leaf  = parts[2];

        int id = -1;
        try {
            id = std::stoi(idStr);
        } catch (...) {
            return Error{Error::Code::InvalidPath, "Invalid device id"};
        }

        if (leaf == "meta") {
            auto metaOpt = getMeta_(cls, id);
            if (!metaOpt.has_value())
                return Error{Error::Code::NotFound, "Device not found"};
            outStr = formatMeta_(*metaOpt);
        } else if (leaf == "capabilities") {
            auto capsOpt = getCapabilities_(cls, id);
            if (!capsOpt.has_value())
                return Error{Error::Code::NotFound, "Device not found"};
            outStr = joinLines_(*capsOpt);
        } else {
            return Error{Error::Code::NotFound, "Unknown leaf under device: " + leaf};
        }
    } else {
        return Error{Error::Code::InvalidPath, "Unsupported discovery path"};
    }

    *reinterpret_cast<std::string*>(obj) = std::move(outStr);
    return std::nullopt;
}

auto PathIODeviceDiscovery::shutdown() -> void {
    // No special state to manage
}

auto PathIODeviceDiscovery::notify(std::string const& /*notificationPath*/) -> void {
    // This provider does not rely on external notifications; ignore.
}

void PathIODeviceDiscovery::adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) {
    PathSpaceBase::adoptContextAndPrefix(std::move(context), prefix);
    std::lock_guard<std::mutex> lg(mutex_);
    mountPrefix_ = std::move(prefix);
}

std::string PathIODeviceDiscovery::toLower_(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string PathIODeviceDiscovery::normalizeClass_(std::string cls) {
    auto s = toLower_(std::move(cls));
    // Normalize common synonyms (canonicalize to singular 'mouse')
    if (s == "mice") s = "mouse";
    if (s == "keyboard") s = "keyboards";
    if (s == "gamepad") s = "gamepads";
    if (s == "touchscreen") s = "touch";
    return s;
}

std::string PathIODeviceDiscovery::listClasses_() const {
    std::set<std::string> classes;
    std::lock_guard<std::mutex> lg(mutex_);
    for (auto const& kv : devices_) {
        if (!kv.second.empty())
            classes.insert(kv.first);
    }
    std::ostringstream oss;
    bool first = true;
    for (auto const& c : classes) {
        if (!first) oss << '\n';
        oss << c;
        first = false;
    }
    return oss.str();
}

std::string PathIODeviceDiscovery::listDeviceIds_(std::string cls) const {
    auto norm = normalizeClass_(std::move(cls));
    std::ostringstream oss;
    bool first = true;
    std::lock_guard<std::mutex> lg(mutex_);
    auto it = devices_.find(norm);
    if (it == devices_.end()) return {};
    for (auto const& kv : it->second) {
        if (!first) oss << '\n';
        oss << kv.first;
        first = false;
    }
    return oss.str();
}

std::optional<PathIODeviceDiscovery::SimDevice> PathIODeviceDiscovery::getMeta_(std::string cls, int id) const {
    auto norm = normalizeClass_(std::move(cls));
    std::lock_guard<std::mutex> lg(mutex_);
    auto it = devices_.find(norm);
    if (it == devices_.end()) return std::nullopt;
    auto jt = it->second.find(id);
    if (jt == it->second.end()) return std::nullopt;
    return jt->second;
}

std::optional<std::vector<std::string>> PathIODeviceDiscovery::getCapabilities_(std::string cls, int id) const {
    auto norm = normalizeClass_(std::move(cls));
    std::lock_guard<std::mutex> lg(mutex_);
    auto it = devices_.find(norm);
    if (it == devices_.end()) return std::nullopt;
    auto jt = it->second.find(id);
    if (jt == it->second.end()) return std::nullopt;
    return jt->second.capabilities;
}

std::string PathIODeviceDiscovery::formatMeta_(SimDevice const& d) {
    std::ostringstream oss;
    oss << "id=" << d.id << '\n';
    oss << "vendor=" << d.vendor << '\n';
    oss << "product=" << d.product << '\n';
    oss << "connection=" << d.connection;
    return oss.str();
}

std::string PathIODeviceDiscovery::joinLines_(std::vector<std::string> const& items) {
    std::ostringstream oss;
    bool first = true;
    for (auto const& v : items) {
        if (!first) oss << '\n';
        oss << v;
        first = false;
    }
    return oss.str();
}

void PathIODeviceDiscovery::notifyUpdates_(std::string const& cls) {
    if (auto ctx = this->getContext()) {
        std::string mount;
        {
            std::lock_guard<std::mutex> lg(mutex_);
            mount = mountPrefix_;
        }
        if (!mount.empty()) {
            ctx->notify(mount);
            if (!cls.empty()) {
                // Notify class subpath under the mount (best-effort)
                ctx->notify(mount + "/" + cls);
            }
        } else {
            ctx->notifyAll();
        }
    }
}

} // namespace SP