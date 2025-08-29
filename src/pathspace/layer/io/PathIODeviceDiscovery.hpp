#pragma once
#include "PathSpace.hpp"
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
 * - "/" or ""                  -> list of classes present (one per line), e.g., "mouse\nkeyboards\n"
 * - "/<class>"                 -> list of device IDs (one per line), e.g., "0\n1\n"
 * - "/<class>/<id>/meta"      -> metadata (one key=value per line)
 * - "/<class>/<id>/capabilities" -> capabilities (one per line)
 *
 * Notes:
 * - Read/write semantics: only std::string reads via out(); writes are unsupported and return InvalidPermissions.
 * - Blocking (Out.doBlock) is ignored; discovery returns immediately (non-blocking).
 * - On updates (add/remove), the provider notifies waiters via the shared context (if present).
 *   Notifications are emitted on the mount prefix (if known) and class subpaths for simple wakeups.
 */
class PathIODeviceDiscovery final : public PathSpaceBase {
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

    // Add or update a device under a class (e.g., "mouse", "keyboards").
    // If the class name isn't one of the known tokens, it will be lowercased and used as-is.
    void addSimulatedDevice(std::string cls, SimDevice dev);

    // Remove a device (no-op if not present).
    void removeSimulatedDevice(std::string cls, int id);

    // Clear all simulated devices.
    void clearAll();

    // ---- PathSpaceBase overrides ----

    // Discovery is read-only; writes are unsupported by default.
    auto in(Iterator const& /*path*/, InputData const& /*data*/) -> InsertReturn override {
        return InsertReturn{.errors = {Error{Error::Code::InvalidPermissions, "PathIODeviceDiscovery does not support in()"}}};
    }

    // Serve discovery information as std::string at the paths described above.
    auto out(Iterator const& path, InputMetadata const& inputMetadata, Out const& /*options*/, void* obj) -> std::optional<Error> override;

    auto shutdown() -> void override;

    auto notify(std::string const& notificationPath) -> void override;

    // Capture mount prefix to enable targeted notifications on retarget or updates.
    void adoptContextAndPrefix(std::shared_ptr<PathSpaceContext> context, std::string prefix) override;

private:
    static std::string toLower_(std::string s);
    static std::string normalizeClass_(std::string cls);

    std::string listClasses_() const;
    std::string listDeviceIds_(std::string cls) const;
    std::optional<SimDevice> getMeta_(std::string cls, int id) const;
    std::optional<std::vector<std::string>> getCapabilities_(std::string cls, int id) const;
    static std::string formatMeta_(SimDevice const& d);
    static std::string joinLines_(std::vector<std::string> const& items);
    void notifyUpdates_(std::string const& cls);

private:
    // class -> (id -> device)
    std::unordered_map<std::string, std::map<int, SimDevice>> devices_;
    mutable std::mutex                                        mutex_;
    std::string                                               mountPrefix_;
};

} // namespace SP