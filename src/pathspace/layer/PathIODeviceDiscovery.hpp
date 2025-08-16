#pragma once
#include "layer/PathIO.hpp"
#include "path/Iterator.hpp"
#include "core/Error.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace SP {

/**
 * PathIODeviceDiscovery — simulation-backed device discovery for a /dev-like namespace.
 *
 * Intent:
 * - Provide a simple, mount-agnostic discovery surface for input/output devices.
 * - Expose a stable, text-based interface (std::string) for listing classes, devices, and metadata.
 * - Backed by an in-memory simulation map; platform backends can also feed/update devices.
 *
 * Path conventions (relative to the mount point):
 * - "/" or ""                  -> list of classes present (one per line), e.g., "mice\nkeyboards\n"
 * - "/<class>"                 -> list of device IDs (one per line), e.g., "0\n1\n"
 * - "/<class>/<id>/meta"      -> metadata (one key=value per line)
 * - "/<class>/<id>/capabilities" -> capabilities (one per line)
 *
 * Notes:
 * - Base PathIO read/write semantics apply; unsupported types return TypeMismatch.
 * - Blocking (Out.doBlock) is ignored; discovery returns immediately (non-blocking).
 * - On updates (add/remove), the provider notifies waiters via the shared context (if present).
 *   Notifications are emitted on the mount prefix (if known) and class subpaths for simple wakeups.
 */
class PathIODeviceDiscovery final : public PathIO {
public:
    struct SimDevice {
        int                      id = 0;
        std::string              vendor;
        std::string              product;
        std::string              connection;   // e.g., "USB", "Bluetooth"
        std::vector<std::string> capabilities; // e.g., {"wheel", "buttons:3"}
    };

    PathIODeviceDiscovery() = default;

    // ---- Simulation / backend update API (thread-safe) ----

    // Add or update a device under a class (e.g., "mice", "keyboards").
    // If the class name isn't one of the known tokens, it will be lowercased and used as-is.
    void addSimulatedDevice(std::string cls, SimDevice dev) {
        auto norm = normalizeClass_(cls);
        {
            std::lock_guard<std::mutex> lg(mutex_);
            devices_[norm][dev.id] = std::move(dev);
        }
        notifyUpdates_(norm);
    }

    // Remove a device (no-op if not present).
    void removeSimulatedDevice(std::string cls, int id) {
        auto norm = normalizeClass_(cls);
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

    // Clear all simulated devices.
    void clearAll() {
        {
            std::lock_guard<std::mutex> lg(mutex_);
            devices_.clear();
        }
        notifyUpdates_({});
    }

    // ---- PathSpaceBase overrides ----

    // Discovery is read-only; writes are unsupported by default.
    auto in(Iterator const& /*path*/, InputData const& /*data*/) -> InsertReturn override {
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "PathIODeviceDiscovery does not support in()"}}};
    }

    // Serve discovery information as std::string at the paths described above.
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& /*options*/, void* obj) -> std::optional<Error> override {
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

    auto shutdown() -> void override {
        // No special state to manage
    }

    auto notify(std::string const& notificationPath) -> void override {
        // This provider does not rely on external notifications; ignore.
        (void)notificationPath;
    }

    // Capture mount prefix to enable targeted notifications on retarget or updates.
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override {
        PathSpaceBase::adoptContextAndPrefix(std::move(context), prefix);
        std::lock_guard<std::mutex> lg(mutex_);
        mountPrefix_ = std::move(prefix);
    }

private:
    static std::string toLower_(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    static std::string normalizeClass_(std::string cls) {
        auto s = toLower_(std::move(cls));
        // Normalize common synonyms
        if (s == "mouse") s = "mice";
        if (s == "keyboard") s = "keyboards";
        if (s == "gamepad") s = "gamepads";
        if (s == "touchscreen") s = "touch";
        return s;
    }

    std::string listClasses_() const {
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

    std::string listDeviceIds_(std::string cls) const {
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

    std::optional<SimDevice> getMeta_(std::string cls, int id) const {
        auto norm = normalizeClass_(std::move(cls));
        std::lock_guard<std::mutex> lg(mutex_);
        auto it = devices_.find(norm);
        if (it == devices_.end()) return std::nullopt;
        auto jt = it->second.find(id);
        if (jt == it->second.end()) return std::nullopt;
        return jt->second;
    }

    std::optional<std::vector<std::string>> getCapabilities_(std::string cls, int id) const {
        auto norm = normalizeClass_(std::move(cls));
        std::lock_guard<std::mutex> lg(mutex_);
        auto it = devices_.find(norm);
        if (it == devices_.end()) return std::nullopt;
        auto jt = it->second.find(id);
        if (jt == it->second.end()) return std::nullopt;
        return jt->second.capabilities;
    }

    static std::string formatMeta_(SimDevice const& d) {
        std::ostringstream oss;
        oss << "id=" << d.id << '\n';
        oss << "vendor=" << d.vendor << '\n';
        oss << "product=" << d.product << '\n';
        oss << "connection=" << d.connection;
        return oss.str();
    }

    static std::string joinLines_(std::vector<std::string> const& items) {
        std::ostringstream oss;
        bool first = true;
        for (auto const& v : items) {
            if (!first) oss << '\n';
            oss << v;
            first = false;
        }
        return oss.str();
    }

    void notifyUpdates_(std::string const& cls) {
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

private:
    // class -> (id -> device)
    std::unordered_map<std::string, std::map<int, SimDevice>> devices_;
    mutable std::mutex                                        mutex_;
    std::string                                               mountPrefix_;
};

} // namespace SP