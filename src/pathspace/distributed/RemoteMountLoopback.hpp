#pragma once

#include "distributed/RemoteMountManager.hpp"
#include "distributed/RemoteMountServer.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace SP::Distributed::Loopback {

class Session final : public RemoteMountSession {
public:
    explicit Session(std::shared_ptr<RemoteMountServer> server)
        : server_(std::move(server)) {}

    auto open(MountOpenRequest const& request) -> Expected<MountOpenResponse> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleMountOpen(request);
    }

    auto read(ReadRequest const& request) -> Expected<ReadResponse> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleRead(request);
    }

    auto insert(InsertRequest const& request) -> Expected<InsertResponse> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleInsert(request);
    }

    auto take(TakeRequest const& request) -> Expected<TakeResponse> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleTake(request);
    }

    auto waitSubscribe(WaitSubscriptionRequest const& request)
        -> Expected<WaitSubscriptionAck> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleWaitSubscribe(request);
    }

    auto nextNotification(std::string const& subscription_id,
                          std::chrono::milliseconds timeout)
        -> Expected<std::optional<Notification>> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }

        auto start = std::chrono::steady_clock::now();
        while (true) {
            if (auto note = server_->nextNotification(subscription_id); note.has_value()) {
                return note;
            }
            if (timeout.count() == 0) {
                return std::optional<Notification>{};
            }
            if (timeout.count() > 0
                && std::chrono::steady_clock::now() - start >= timeout) {
                return std::optional<Notification>{};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    }

    auto heartbeat(Heartbeat const& heartbeat) -> Expected<void> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleHeartbeat(heartbeat);
    }

    auto streamNotifications(std::string const&    session_id,
                             std::chrono::milliseconds timeout,
                             std::size_t             max_batch)
        -> Expected<std::vector<Notification>> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return server_->handleNotificationStream(session_id, timeout, max_batch);
    }

private:
    std::shared_ptr<RemoteMountServer> server_;
};

class Factory final : public RemoteMountSessionFactory {
public:
    explicit Factory(std::shared_ptr<RemoteMountServer> server)
        : server_(std::move(server)) {}

    auto create(RemoteMountClientOptions const&)
        -> Expected<std::shared_ptr<RemoteMountSession>> override {
        if (!server_) {
            return std::unexpected(Error{Error::Code::UnknownError, "server unavailable"});
        }
        return std::make_shared<Session>(server_);
    }

private:
    std::shared_ptr<RemoteMountServer> server_;
};

[[nodiscard]] inline auto makeFactory(std::shared_ptr<RemoteMountServer> server)
    -> std::shared_ptr<RemoteMountSessionFactory> {
    return std::make_shared<Factory>(std::move(server));
}

} // namespace SP::Distributed::Loopback
