#include "pathspace/distributed/RemoteMountTls.hpp"

#include "core/Error.hpp"
#include "distributed/RemoteMountProtocol.hpp"
#include "distributed/RemoteMountServer.hpp"
#include "log/TaggedLogger.hpp"

#include <asio.hpp>
#include <asio/ssl.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace SP::Distributed {
namespace {

using TlsStream = asio::ssl::stream<asio::ip::tcp::socket>;

constexpr std::chrono::milliseconds kDefaultNotificationTimeout{250};
constexpr std::chrono::milliseconds kDefaultConnectTimeout{2000};

struct X509Deleter {
    void operator()(X509* cert) const {
        if (cert != nullptr) {
            X509_free(cert);
        }
    }
};
using UniqueX509 = std::unique_ptr<X509, X509Deleter>;

class ScopedSocketCloser {
public:
    explicit ScopedSocketCloser(asio::ip::tcp::socket& socket)
        : socket_(socket) {}

    ~ScopedSocketCloser() {
        std::error_code ec;
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

private:
    asio::ip::tcp::socket& socket_;
};

[[nodiscard]] auto make_transport_error(std::string message) -> Error {
    return Error{Error::Code::UnknownError, std::move(message)};
}

[[nodiscard]] auto make_error_payload(Error const& error) -> ErrorPayload {
    ErrorPayload payload;
    payload.code    = std::string(errorCodeToString(error.code));
    payload.message = error.message.value_or(payload.code);
    payload.retryable = (error.code == Error::Code::Timeout);
    return payload;
}

[[nodiscard]] auto parse_error_payload(ErrorPayload const& payload) -> Error {
    Error::Code code = Error::Code::UnknownError;
    auto label       = payload.code;
    if (label == "invalid_path") {
        code = Error::Code::InvalidPath;
    } else if (label == "invalid_type") {
        code = Error::Code::InvalidType;
    } else if (label == "timeout") {
        code = Error::Code::Timeout;
    } else if (label == "malformed_input") {
        code = Error::Code::MalformedInput;
    } else if (label == "invalid_permissions") {
        code = Error::Code::InvalidPermissions;
    } else if (label == "capacity_exceeded") {
        code = Error::Code::CapacityExceeded;
    } else if (label == "no_such_path") {
        code = Error::Code::NoSuchPath;
    }
    return Error{code, payload.message};
}

[[nodiscard]] auto load_certificate(std::string const& path) -> Expected<UniqueX509> {
    if (path.empty()) {
        return std::unexpected(make_transport_error("certificate path missing"));
    }
    BIO* bio = BIO_new_file(path.c_str(), "r");
    if (!bio) {
        return std::unexpected(make_transport_error("failed to open certificate: " + path));
    }
    UniqueX509 cert(PEM_read_bio_X509(bio, nullptr, nullptr, nullptr));
    BIO_free(bio);
    if (!cert) {
        return std::unexpected(make_transport_error("invalid certificate: " + path));
    }
    return cert;
}

[[nodiscard]] auto fingerprint_from_cert(X509* cert) -> Expected<std::string> {
    unsigned int length = 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    if (X509_digest(cert, EVP_sha256(), digest, &length) != 1) {
        return std::unexpected(make_transport_error("failed to compute certificate fingerprint"));
    }
    std::ostringstream oss;
    oss << "sha256:";
    for (unsigned int idx = 0; idx < length; ++idx) {
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<int>(digest[idx]);
    }
    return oss.str();
}

[[nodiscard]] auto subject_from_cert(X509* cert) -> std::string {
    if (!cert) {
        return {};
    }
    char buffer[512];
    auto name = X509_get_subject_name(cert);
    if (!name) {
        return {};
    }
    if (X509_NAME_oneline(name, buffer, static_cast<int>(sizeof(buffer))) == nullptr) {
        return {};
    }
    return std::string(buffer);
}

[[nodiscard]] auto configure_client_context(RemoteMountTlsClientConfig const& config)
    -> Expected<std::shared_ptr<asio::ssl::context>> {
    try {
        auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
        context->set_options(asio::ssl::context::default_workarounds
                             | asio::ssl::context::no_sslv2
                             | asio::ssl::context::single_dh_use);
        if (config.verify_server_certificate) {
            if (config.ca_cert_path.empty()) {
                return std::unexpected(make_transport_error("ca_cert_path required for TLS"));
            }
            context->load_verify_file(config.ca_cert_path);
            context->set_verify_mode(asio::ssl::verify_peer);
        } else {
            context->set_verify_mode(asio::ssl::verify_none);
        }
        if (!config.client_cert_path.empty()) {
            context->use_certificate_chain_file(config.client_cert_path);
        }
        if (!config.client_key_path.empty()) {
            context->use_private_key_file(config.client_key_path, asio::ssl::context::pem);
        }
        return context;
    } catch (std::system_error const& err) {
        return std::unexpected(make_transport_error(err.what()));
    }
}

[[nodiscard]] auto configure_server_context(RemoteMountTlsServerConfig const& config)
    -> Expected<std::shared_ptr<asio::ssl::context>> {
    try {
        auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tls_server);
        context->set_options(asio::ssl::context::default_workarounds
                             | asio::ssl::context::no_sslv2
                             | asio::ssl::context::single_dh_use);
        context->use_certificate_chain_file(config.certificate_path);
        context->use_private_key_file(config.private_key_path, asio::ssl::context::pem);
        if (config.require_client_certificate) {
            if (config.ca_cert_path.empty()) {
                return std::unexpected(make_transport_error("ca_cert_path required for mTLS"));
            }
            context->load_verify_file(config.ca_cert_path);
            context->set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
        } else {
            context->set_verify_mode(asio::ssl::verify_none);
        }
        return context;
    } catch (std::system_error const& err) {
        return std::unexpected(make_transport_error(err.what()));
    }
}

struct ClientConnection {
    explicit ClientConnection(std::shared_ptr<asio::ssl::context> ctx)
        : stream(io, *ctx) {}

    asio::io_context io;
    TlsStream        stream;
};

[[nodiscard]] auto write_frame(TlsStream& stream, RemoteFrame frame) -> Expected<void> {
    frame.sent_at = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch());
    auto payload = serializeFrame(frame);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    std::uint32_t size = static_cast<std::uint32_t>(payload->size());
    std::array<std::uint8_t, 4> header{{static_cast<std::uint8_t>((size >> 24) & 0xFF),
                                        static_cast<std::uint8_t>((size >> 16) & 0xFF),
                                        static_cast<std::uint8_t>((size >> 8) & 0xFF),
                                        static_cast<std::uint8_t>(size & 0xFF)}};
    std::array<asio::const_buffer, 2> buffers{asio::buffer(header),
                                              asio::buffer(payload->data(), payload->size())};
    std::error_code ec;
    asio::write(stream, buffers, ec);
    if (ec) {
        return std::unexpected(make_transport_error(ec.message()));
    }
    return {};
}

[[nodiscard]] auto read_frame(TlsStream& stream) -> Expected<RemoteFrame> {
    std::array<std::uint8_t, 4> header{};
    std::error_code             ec;
    asio::read(stream, asio::buffer(header), ec);
    if (ec) {
        return std::unexpected(make_transport_error(ec.message()));
    }
    auto size = (static_cast<std::uint32_t>(header[0]) << 24)
              | (static_cast<std::uint32_t>(header[1]) << 16)
              | (static_cast<std::uint32_t>(header[2]) << 8)
              | static_cast<std::uint32_t>(header[3]);
    if (size == 0) {
        return std::unexpected(make_transport_error("frame payload empty"));
    }
    std::string payload(size, '\0');
    asio::read(stream, asio::buffer(payload.data(), payload.size()), ec);
    if (ec) {
        return std::unexpected(make_transport_error(ec.message()));
    }
    return deserializeFrame(payload);
}

} // namespace

class RemoteMountTlsSession final : public RemoteMountSession {
public:
    RemoteMountTlsSession(RemoteMountClientOptions options,
                          RemoteMountTlsClientConfig config,
                          std::shared_ptr<asio::ssl::context> context,
                          std::string fingerprint,
                          std::string subject)
        : options_(std::move(options))
        , config_(std::move(config))
        , context_(std::move(context))
        , client_fingerprint_(std::move(fingerprint))
        , client_subject_(std::move(subject)) {}

    auto open(MountOpenRequest const& request) -> Expected<MountOpenResponse> override {
        auto adjusted = request;
        if (!client_fingerprint_.empty()) {
            adjusted.auth.fingerprint = client_fingerprint_;
            if (adjusted.auth.proof.empty()) {
                adjusted.auth.proof = client_fingerprint_;
            }
        }
        if (!client_subject_.empty() && adjusted.auth.subject.empty()) {
            adjusted.auth.subject = client_subject_;
        }
        adjusted.auth.kind = AuthKind::MutualTls;
        return invoke<MountOpenRequest, MountOpenResponse>(FrameKind::MountOpenRequest,
                                                           adjusted,
                                                           FrameKind::MountOpenResponse);
    }

    auto read(ReadRequest const& request) -> Expected<ReadResponse> override {
        return invoke<ReadRequest, ReadResponse>(FrameKind::ReadRequest,
                                                 request,
                                                 FrameKind::ReadResponse);
    }

    auto insert(InsertRequest const& request) -> Expected<InsertResponse> override {
        return invoke<InsertRequest, InsertResponse>(FrameKind::InsertRequest,
                                                     request,
                                                     FrameKind::InsertResponse);
    }

    auto take(TakeRequest const& request) -> Expected<TakeResponse> override {
        return invoke<TakeRequest, TakeResponse>(FrameKind::TakeRequest,
                                                 request,
                                                 FrameKind::TakeResponse);
    }

    auto waitSubscribe(WaitSubscriptionRequest const& request)
        -> Expected<WaitSubscriptionAck> override {
        return invoke<WaitSubscriptionRequest, WaitSubscriptionAck>(FrameKind::WaitSubscribeRequest,
                                                                    request,
                                                                    FrameKind::WaitSubscribeAck);
    }

    auto nextNotification(std::string const&, std::chrono::milliseconds)
        -> Expected<std::optional<Notification>> override {
        // Not used in the new streaming path; fall back to streaming with batch=1.
        NotificationStreamRequest request;
        request.request_id = "notif-" + std::to_string(++request_counter_);
        if (!session_id_.empty()) {
            request.session_id = session_id_;
        }
        request.timeout   = kDefaultNotificationTimeout;
        request.max_batch = 1;
        auto response     = streamNotifications(request.session_id.empty() ? std::string{}
                                                                          : request.session_id,
                                                request.timeout,
                                                request.max_batch);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (!response->empty()) {
            return std::optional<Notification>{response->front()};
        }
        return std::optional<Notification>{std::nullopt};
    }

    auto streamNotifications(std::string const& session_id,
                             std::chrono::milliseconds timeout,
                             std::size_t max_batch)
        -> Expected<std::vector<Notification>> override {
        NotificationStreamRequest request;
        request.request_id = "notif-" + std::to_string(++request_counter_);
        request.session_id = session_id;
        request.timeout    = timeout.count() > 0 ? timeout : kDefaultNotificationTimeout;
        request.max_batch  = max_batch == 0 ? 1 : max_batch;
        auto response = invoke<NotificationStreamRequest, NotificationStreamResponse>(
            FrameKind::NotificationStreamRequest, request, FrameKind::NotificationStreamResponse);
        if (!response) {
            return std::unexpected(response.error());
        }
        if (response->error) {
            return std::unexpected(parse_error_payload(*response->error));
        }
        return response->notifications;
    }

    auto heartbeat(Heartbeat const& heartbeat) -> Expected<void> override {
        auto response = invoke<Heartbeat, Heartbeat>(FrameKind::Heartbeat,
                                                     heartbeat,
                                                     FrameKind::Heartbeat);
        if (!response) {
            return std::unexpected(response.error());
        }
        return {};
    }

    void setSessionId(std::string session_id) {
        session_id_ = std::move(session_id);
    }

private:
    template <typename Request, typename Response>
    [[nodiscard]] Expected<Response> invoke(FrameKind request_kind,
                                            Request const& request,
                                            FrameKind response_kind) {
        auto connection = connect();
        if (!connection) {
            return std::unexpected(connection.error());
        }
        auto& conn = **connection;
        auto& io    = conn.io;
        (void)io;
        auto& stream = conn.stream;
        RemoteFrame frame;
        frame.kind    = request_kind;
        frame.payload = request;
        auto write    = write_frame(stream, frame);
        if (!write) {
            return std::unexpected(write.error());
        }
        auto response_frame = read_frame(stream);
        if (!response_frame) {
            return std::unexpected(response_frame.error());
        }
        if (response_frame->kind == FrameKind::Error) {
            auto payload = std::get<ErrorPayload>(response_frame->payload);
            return std::unexpected(parse_error_payload(payload));
        }
        if (response_frame->kind != response_kind) {
            return std::unexpected(
                make_transport_error("unexpected response frame kind"));
        }
        return std::get<Response>(response_frame->payload);
    }

    [[nodiscard]] Expected<std::unique_ptr<ClientConnection>> connect() {
        auto connection = std::make_unique<ClientConnection>(context_);
        std::error_code ec;
        asio::ip::tcp::resolver resolver(connection->io);
        auto                    endpoints = resolver.resolve(options_.host,
                                                              std::to_string(options_.port),
                                                              ec);
        if (ec) {
            return std::unexpected(make_transport_error(ec.message()));
        }
        asio::connect(connection->stream.lowest_layer(), endpoints, ec);
        if (ec) {
            return std::unexpected(make_transport_error(ec.message()));
        }
        if (!config_.sni_host.empty()) {
            SSL_set_tlsext_host_name(connection->stream.native_handle(), config_.sni_host.c_str());
        }
        connection->stream.handshake(asio::ssl::stream_base::client, ec);
        if (ec) {
            return std::unexpected(make_transport_error(ec.message()));
        }
        return connection;
    }

    RemoteMountClientOptions                options_;
    RemoteMountTlsClientConfig              config_;
    std::shared_ptr<asio::ssl::context>     context_;
    std::string                            client_fingerprint_;
    std::string                            client_subject_;
    std::string                            session_id_;
    std::atomic<std::uint64_t>             request_counter_{1};
};

class RemoteMountTlsSessionFactory final : public RemoteMountSessionFactory {
public:
    explicit RemoteMountTlsSessionFactory(std::optional<RemoteMountTlsClientConfig> default_config)
        : default_config_(std::move(default_config)) {}

    auto create(RemoteMountClientOptions const& options)
        -> Expected<std::shared_ptr<RemoteMountSession>> override {
        auto tls_config = options.tls ? *options.tls : RemoteMountTlsClientConfig{};
        if (!options.tls && default_config_) {
            tls_config = *default_config_;
        }
        if (tls_config.client_cert_path.empty() || tls_config.client_key_path.empty()) {
            return std::unexpected(make_transport_error("client certificate and key required"));
        }
        auto context = configure_client_context(tls_config);
        if (!context) {
            return std::unexpected(context.error());
        }
        auto cert = load_certificate(tls_config.client_cert_path);
        if (!cert) {
            return std::unexpected(cert.error());
        }
        auto fingerprint = fingerprint_from_cert(cert->get());
        if (!fingerprint) {
            return std::unexpected(fingerprint.error());
        }
        auto subject = subject_from_cert(cert->get());
        auto session
            = std::make_shared<RemoteMountTlsSession>(options, tls_config, *context, *fingerprint,
                                                      subject);
        return session;
    }

private:
    std::optional<RemoteMountTlsClientConfig> default_config_;
};

class RemoteMountTlsServer::Impl {
public:
    Impl(RemoteMountTlsServerConfig config,
         std::shared_ptr<RemoteMountServer> server)
        : config_(std::move(config))
        , server_(std::move(server)) {}

    auto start() -> bool {
        if (running_.exchange(true)) {
            return true;
        }
        auto context = configure_server_context(config_);
        if (!context) {
            running_ = false;
            sp_log("RemoteMountTlsServer failed to configure TLS context", "RemoteMountTlsServer");
            return false;
        }
        context_ = *context;
        std::error_code ec;
        auto address = asio::ip::make_address(config_.bind_address, ec);
        if (ec) {
            running_ = false;
            sp_log("RemoteMountTlsServer invalid bind address", "RemoteMountTlsServer");
            return false;
        }
        asio::ip::tcp::endpoint endpoint(address, config_.port);
        acceptor_ = std::make_unique<asio::ip::tcp::acceptor>(io_context_);
        acceptor_->open(endpoint.protocol(), ec);
        if (ec) {
            running_ = false;
            sp_log("RemoteMountTlsServer failed to open acceptor", "RemoteMountTlsServer");
            return false;
        }
        acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_->bind(endpoint, ec);
        if (ec) {
            running_ = false;
            sp_log("RemoteMountTlsServer failed to bind port", "RemoteMountTlsServer");
            return false;
        }
        acceptor_->listen(asio::socket_base::max_listen_connections, ec);
        if (ec) {
            running_ = false;
            sp_log("RemoteMountTlsServer failed to listen", "RemoteMountTlsServer");
            return false;
        }
        actual_port_ = acceptor_->local_endpoint().port();
        accept_thread_ = std::thread([this]() { acceptLoop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        std::error_code ec;
        if (acceptor_) {
            acceptor_->close(ec);
        }
        io_context_.stop();
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
    }

    [[nodiscard]] bool running() const {
        return running_.load();
    }

    [[nodiscard]] std::uint16_t port() const {
        return actual_port_;
    }

private:
    void acceptLoop() {
        while (running()) {
            asio::ip::tcp::socket socket(io_context_);
            std::error_code        ec;
            acceptor_->accept(socket, ec);
            if (ec) {
                if (!running()) {
                    break;
                }
                continue;
            }
            std::thread(&Impl::handleConnection, this, std::move(socket)).detach();
        }
    }

    void handleConnection(asio::ip::tcp::socket socket) {
        ScopedSocketCloser closer(socket);
        if (!context_) {
            return;
        }
        TlsStream stream(std::move(socket), *context_);
        std::error_code ec;
        stream.handshake(asio::ssl::stream_base::server, ec);
        if (ec) {
            return;
        }
        UniqueX509 peer_cert(SSL_get_peer_certificate(stream.native_handle()));
        std::string fingerprint;
        if (peer_cert) {
            auto fp = fingerprint_from_cert(peer_cert.get());
            if (fp) {
                fingerprint = *fp;
            }
        }
        auto subject = subject_from_cert(peer_cert.get());
        auto frame       = read_frame(stream);
        if (!frame) {
            return;
        }
        auto response = dispatch(*frame, fingerprint, subject);
        if (!response) {
            RemoteFrame error_frame;
            error_frame.kind    = FrameKind::Error;
            error_frame.payload = make_error_payload(response.error());
            auto _ = write_frame(stream, error_frame);
            (void)_;
            return;
        }
        auto _ = write_frame(stream, *response);
        (void)_;
    }

    [[nodiscard]] Expected<RemoteFrame> dispatch(RemoteFrame const& frame,
                                                  std::string const& fingerprint,
                                                  std::string const& subject) {
        switch (frame.kind) {
        case FrameKind::MountOpenRequest: {
            auto request = std::get<MountOpenRequest>(frame.payload);
            request.auth.kind        = AuthKind::MutualTls;
            request.auth.fingerprint = fingerprint;
            if (request.auth.proof.empty()) {
                request.auth.proof = fingerprint;
            }
            if (request.auth.subject.empty()) {
                request.auth.subject = subject;
            }
            auto response = server_->handleMountOpen(request);
            return to_frame(response, FrameKind::MountOpenResponse);
        }
        case FrameKind::ReadRequest: {
            auto response = server_->handleRead(std::get<ReadRequest>(frame.payload));
            return to_frame(response, FrameKind::ReadResponse);
        }
        case FrameKind::InsertRequest: {
            auto response = server_->handleInsert(std::get<InsertRequest>(frame.payload));
            return to_frame(response, FrameKind::InsertResponse);
        }
        case FrameKind::TakeRequest: {
            auto response = server_->handleTake(std::get<TakeRequest>(frame.payload));
            return to_frame(response, FrameKind::TakeResponse);
        }
        case FrameKind::WaitSubscribeRequest: {
            auto response = server_->handleWaitSubscribe(std::get<WaitSubscriptionRequest>(frame.payload));
            return to_frame(response, FrameKind::WaitSubscribeAck);
        }
        case FrameKind::NotificationStreamRequest: {
            auto request = std::get<NotificationStreamRequest>(frame.payload);
            auto response = server_->handleNotificationStream(request.session_id,
                                                              request.timeout,
                                                              request.max_batch);
            if (!response) {
                return std::unexpected(response.error());
            }
            NotificationStreamResponse resp;
            resp.request_id   = request.request_id;
            resp.session_id   = request.session_id;
            resp.notifications = std::move(*response);
            RemoteFrame frame_resp;
            frame_resp.kind    = FrameKind::NotificationStreamResponse;
            frame_resp.payload = std::move(resp);
            return frame_resp;
        }
        case FrameKind::Heartbeat: {
            auto response = server_->handleHeartbeat(std::get<Heartbeat>(frame.payload));
            if (!response) {
                return std::unexpected(response.error());
            }
            RemoteFrame resp;
            resp.kind    = FrameKind::Heartbeat;
            resp.payload = std::get<Heartbeat>(frame.payload);
            return resp;
        }
        default:
            return std::unexpected(make_transport_error("unsupported frame kind"));
        }
    }

    template <typename Response>
    [[nodiscard]] Expected<RemoteFrame> to_frame(Expected<Response> const& result, FrameKind kind) {
        if (!result) {
            return std::unexpected(result.error());
        }
        RemoteFrame frame;
        frame.kind    = kind;
        frame.payload = *result;
        return frame;
    }

    RemoteMountTlsServerConfig               config_;
    std::shared_ptr<RemoteMountServer>       server_;
    std::shared_ptr<asio::ssl::context>      context_;
    asio::io_context                         io_context_;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor_;
    std::thread                              accept_thread_;
    std::atomic<bool>                        running_{false};
    std::uint16_t                            actual_port_{0};
};

RemoteMountTlsServer::RemoteMountTlsServer(RemoteMountTlsServerConfig config,
                                           std::shared_ptr<RemoteMountServer> server)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(server))) {}

RemoteMountTlsServer::~RemoteMountTlsServer() {
    stop();
}

bool RemoteMountTlsServer::start() {
    return impl_->start();
}

void RemoteMountTlsServer::stop() {
    impl_->stop();
}

bool RemoteMountTlsServer::running() const {
    return impl_->running();
}

std::uint16_t RemoteMountTlsServer::port() const {
    return impl_->port();
}

std::shared_ptr<RemoteMountSessionFactory>
makeTlsSessionFactory(std::optional<RemoteMountTlsClientConfig> default_config) {
    return std::make_shared<RemoteMountTlsSessionFactory>(std::move(default_config));
}

} // namespace SP::Distributed
