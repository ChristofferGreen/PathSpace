#include <pathspace/ui/declarative/WidgetEventTrellis.hpp>

#include "WidgetEventTrellisWorker.hpp"

#include <mutex>
#include <unordered_map>

namespace SP::UI::Declarative {
namespace {
std::mutex g_worker_mutex;
std::unordered_map<PathSpace*, std::shared_ptr<WidgetEventTrellisWorker>> g_workers;
} // namespace

auto CreateWidgetEventTrellis(PathSpace& space,
                              WidgetEventTrellisOptions const& options) -> SP::Expected<bool> {
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    auto it = g_workers.find(&space);
    if (it != g_workers.end() && it->second) {
        return false;
    }

    auto worker = std::make_shared<WidgetEventTrellisWorker>(space, options);
    auto started = worker->start();
    if (!started) {
        return std::unexpected(started.error());
    }
    g_workers[&space] = worker;
    return true;
}

auto ShutdownWidgetEventTrellis(PathSpace& space) -> void {
    std::shared_ptr<WidgetEventTrellisWorker> worker;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        auto it = g_workers.find(&space);
        if (it != g_workers.end()) {
            worker = std::move(it->second);
            g_workers.erase(it);
        }
    }
    if (worker) {
        worker->stop();
    }
}

} // namespace SP::UI::Declarative
