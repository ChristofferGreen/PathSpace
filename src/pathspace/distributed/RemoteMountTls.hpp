#pragma once

#include "distributed/RemoteMountManager.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace SP::Distributed {

class RemoteMountServer;

struct RemoteMountTlsServerConfig {
    std::string                bind_address{"127.0.0.1"};
    std::uint16_t              port{18443};
    std::string                certificate_path;
    std::string                private_key_path;
    std::string                ca_cert_path;
    bool                       require_client_certificate{true};
    std::size_t                max_concurrent_connections{64};
    std::chrono::milliseconds handshake_timeout{std::chrono::milliseconds{5000}};
};

class RemoteMountTlsServer {
public:
    RemoteMountTlsServer(RemoteMountTlsServerConfig config,
                         std::shared_ptr<RemoteMountServer> server);
    RemoteMountTlsServer(RemoteMountTlsServer const&)            = delete;
    RemoteMountTlsServer& operator=(RemoteMountTlsServer const&) = delete;
    ~RemoteMountTlsServer();

    bool start();
    void stop();

    [[nodiscard]] bool         running() const;
    [[nodiscard]] std::uint16_t port() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::shared_ptr<RemoteMountSessionFactory>
makeTlsSessionFactory(std::optional<RemoteMountTlsClientConfig> default_config = std::nullopt);

} // namespace SP::Distributed
