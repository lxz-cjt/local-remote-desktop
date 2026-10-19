#include "network_server.h"

#include "audio_capture.h"
#include "video_encoder.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {

constexpr DWORD kReceiveTimeoutMs = 15000;
constexpr DWORD kSendTimeoutMs = 1000;
constexpr size_t kMaxHttpRequest = 2048;
constexpr unsigned int kMaxPinFailures = 5;
constexpr auto kPinLockout = std::chrono::seconds(60);

bool WriteAllSsl(SSL* ssl, const uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const int chunk = static_cast<int>(std::min<size_t>(size - offset, 1024 * 1024));
        const int written = SSL_write(ssl, data + offset, chunk);
        if (written <= 0) {
            return false;
        }
        offset += static_cast<size_t>(written);
    }
    return true;
}

bool ReadAllSsl(SSL* ssl, uint8_t* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        const int chunk = static_cast<int>(std::min<size_t>(size - offset, 1024 * 1024));
        const int received = SSL_read(ssl, data + offset, chunk);
        if (received <= 0) {
            return false;
        }
        offset += static_cast<size_t>(received);
    }
    return true;
}

bool ValidClientPacket(uint8_t type, uint32_t length, bool authenticated) {
    using protocol::MsgType;
    if (!authenticated) {
        if (type == static_cast<uint8_t>(MsgType::AUTH_PAIR)) {
            return length == sizeof(protocol::AuthPair);
        }
        if (type == static_cast<uint8_t>(MsgType::AUTH_SESSION)) {
            return length == sizeof(protocol::AuthSession);
        }
        return false;
    }
    if (type == static_cast<uint8_t>(MsgType::INPUT_EVENT)) {
        return length == sizeof(protocol::InputEvent);
    }
    return (type == static_cast<uint8_t>(MsgType::HEARTBEAT) ||
            type == static_cast<uint8_t>(MsgType::REQUEST_KEYFRAME)) && length == 0;
}

bool ReadClientPacket(SSL* ssl, protocol::PacketHeader& header,
                      std::vector<uint8_t>& payload, bool authenticated) {
    if (!ReadAllSsl(ssl, reinterpret_cast<uint8_t*>(&header), sizeof(header)) ||
        !ValidClientPacket(header.msg_type, header.payload_len, authenticated)) {
        return false;
    }
    payload.resize(header.payload_len);
    return payload.empty() || ReadAllSsl(ssl, payload.data(), payload.size());
}

void SetSocketTimeouts(SOCKET socket, DWORD receive_ms, DWORD send_ms) {
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&receive_ms), sizeof(receive_ms));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&send_ms), sizeof(send_ms));
}

bool SendAll(SOCKET socket, const std::string& data) {
    size_t offset = 0;
    while (offset < data.size()) {
        const int sent = send(socket, data.data() + offset,
                              static_cast<int>(data.size() - offset), 0);
        if (sent <= 0) {
            return false;
        }
        offset += static_cast<size_t>(sent);
    }
    return true;
}

std::string HexFingerprint(const unsigned char* bytes, unsigned int length) {
    std::ostringstream output;
    for (unsigned int i = 0; i < length; ++i) {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(bytes[i]);
    }
    return output.str();
}

} // namespace

NetworkServer::NetworkServer(uint16_t tls_port, uint16_t http_port, std::string pin)
    : tls_port_(tls_port), http_port_(http_port), pin_(std::move(pin)) {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        throw std::runtime_error("WSAStartup failed");
    }
}

NetworkServer::~NetworkServer() {
    Stop();
    WSACleanup();
}

bool NetworkServer::InitializeSslContext(const std::string& cert, const std::string& key) {
    ssl_ctx_ = SSL_CTX_new(TLS_server_method());
    if (!ssl_ctx_) {
        return false;
    }
    SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(ssl_ctx_, 0);
    SSL_CTX_set_options(ssl_ctx_, SSL_OP_NO_COMPRESSION);

    if (SSL_CTX_use_certificate_file(ssl_ctx_, cert.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ssl_ctx_, key.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ssl_ctx_) != 1) {
        std::cerr << "[TLS] Could not load the runtime certificate and key\n";
        return false;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, cert.c_str(), "rb") != 0 || !file) {
        return false;
    }
    X509* x509 = PEM_read_X509(file, nullptr, nullptr, nullptr);
    fclose(file);
    if (!x509) {
        return false;
    }
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_length = 0;
    const int result = X509_digest(x509, EVP_sha256(), digest, &digest_length);
    X509_free(x509);
    if (result != 1) {
        return false;
    }
    cert_fingerprint_ = HexFingerprint(digest, digest_length);
    return true;
}

std::string NetworkServer::GetPairingUrl() const {
    return "http://" + local_ip_ + ":" + std::to_string(http_port_) + "/pair";
}

bool NetworkServer::Start(const std::string& cert_pem, const std::string& key_pem) {
    if (running_ || !InitializeSslContext(cert_pem, key_pem)) {
        return false;
    }

    char hostname[256] = {};
    if (gethostname(hostname, sizeof(hostname) - 1) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        if (getaddrinfo(hostname, nullptr, &hints, &addresses) == 0 && addresses) {
            const auto* address = reinterpret_cast<sockaddr_in*>(addresses->ai_addr);
            const char* ip = inet_ntoa(address->sin_addr);
            local_ip_ = ip ? ip : "127.0.0.1";
            freeaddrinfo(addresses);
        }
    }
    if (local_ip_.empty()) {
        local_ip_ = "127.0.0.1";
    }

    listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    http_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket_ == INVALID_SOCKET || http_socket_ == INVALID_SOCKET) {
        return false;
    }

    int reuse = 1;
    setsockopt(listen_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    setsockopt(http_socket_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in tls_address{};
    tls_address.sin_family = AF_INET;
    tls_address.sin_addr.s_addr = INADDR_ANY;
    tls_address.sin_port = htons(tls_port_);
    sockaddr_in http_address = tls_address;
    http_address.sin_port = htons(http_port_);

    if (bind(listen_socket_, reinterpret_cast<sockaddr*>(&tls_address), sizeof(tls_address)) == SOCKET_ERROR ||
        listen(listen_socket_, 1) == SOCKET_ERROR ||
        bind(http_socket_, reinterpret_cast<sockaddr*>(&http_address), sizeof(http_address)) == SOCKET_ERROR ||
        listen(http_socket_, 4) == SOCKET_ERROR) {
        std::cerr << "[Network] Could not bind the requested ports\n";
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&NetworkServer::TlsAcceptLoop, this);
    http_thread_ = std::thread(&NetworkServer::HttpServerLoop, this);

    std::cout << "[Network] TLS server listening on port " << tls_port_ << " (TLS 1.2+)\n";
    std::cout << "[Network] Pairing metadata: " << GetPairingUrl() << "\n";
    std::cout << "[Network] Certificate SHA-256: " << cert_fingerprint_ << "\n";
    return true;
}

void NetworkServer::Stop() {
    running_ = false;
    client_authenticated_ = false;

    if (listen_socket_ != INVALID_SOCKET) {
        shutdown(listen_socket_, SD_BOTH);
        closesocket(listen_socket_);
        listen_socket_ = INVALID_SOCKET;
    }
    if (http_socket_ != INVALID_SOCKET) {
        shutdown(http_socket_, SD_BOTH);
        closesocket(http_socket_);
        http_socket_ = INVALID_SOCKET;
    }
    ShutdownClient();

    if (accept_thread_.joinable()) accept_thread_.join();
    if (http_thread_.joinable()) http_thread_.join();
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
}

void NetworkServer::ShutdownClient() {
    std::lock_guard<std::mutex> lock(client_mutex_);
    if (client_socket_ != INVALID_SOCKET) {
        shutdown(client_socket_, SD_BOTH);
    }
}

void NetworkServer::DisconnectAuthenticatedClient() {
    client_authenticated_ = false;
    ShutdownClient();
}

void NetworkServer::HttpServerLoop() {
    while (running_) {
        sockaddr_in peer{};
        int peer_length = sizeof(peer);
        const SOCKET client = accept(http_socket_, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (client == INVALID_SOCKET) {
            continue;
        }
        SetSocketTimeouts(client, 3000, 3000);

        std::string request;
        char buffer[512];
        while (request.size() < kMaxHttpRequest && request.find("\r\n\r\n") == std::string::npos) {
            const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
            if (received <= 0) break;
            request.append(buffer, static_cast<size_t>(received));
        }

        std::string response;
        if (request.rfind("GET /pair HTTP/1.", 0) == 0) {
            const std::string json = "{\"ip\":\"" + local_ip_ + "\",\"tls_port\":" +
                std::to_string(tls_port_) + ",\"fingerprint\":\"" + cert_fingerprint_ +
                "\",\"protocol_version\":" + std::to_string(protocol::kVersion) +
                ",\"version\":\"" LRDC_VERSION "\"}";
            response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\n"
                       "X-Content-Type-Options: nosniff\r\nContent-Length: " +
                       std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
        } else {
            const std::string body = "Not found";
            response = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        }
        SendAll(client, response);
        closesocket(client);
    }
}

void NetworkServer::TlsAcceptLoop() {
    while (running_) {
        sockaddr_in peer{};
        int peer_length = sizeof(peer);
        const SOCKET client = accept(listen_socket_, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (client == INVALID_SOCKET) {
            continue;
        }
        if (client_authenticated_) {
            closesocket(client);
            continue;
        }
        SetSocketTimeouts(client, kReceiveTimeoutMs, kSendTimeoutMs);
        const char* peer_text = inet_ntoa(peer.sin_addr);
        const std::string peer_ip = peer_text ? peer_text : "unknown";

        SSL* ssl = SSL_new(ssl_ctx_);
        if (!ssl) {
            closesocket(client);
            continue;
        }
        SSL_set_fd(ssl, static_cast<int>(client));
        {
            std::lock_guard<std::mutex> lock(client_mutex_);
            ssl_ = ssl;
            client_socket_ = client;
        }

        if (SSL_accept(ssl) == 1 && running_) {
            std::cout << "[Network] TLS peer connected from " << peer_ip << " using "
                      << SSL_get_version(ssl) << "\n";
            ClientReadLoop(ssl, client, peer_ip);
        }

        client_authenticated_ = false;
        {
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            std::lock_guard<std::mutex> client_lock(client_mutex_);
            if (ssl_ == ssl) ssl_ = nullptr;
            if (client_socket_ == client) client_socket_ = INVALID_SOCKET;
            SSL_free(ssl);
            closesocket(client);
        }
        std::cout << "[Network] Client disconnected\n";
    }
}

bool NetworkServer::SendAuthResult(SSL* ssl, protocol::AuthStatus status, const std::string& token) {
    protocol::AuthResult result{};
    result.status = static_cast<uint8_t>(status);
    if (status == protocol::AuthStatus::OK && token.size() == protocol::kSessionTokenLength) {
        std::memcpy(result.token, token.data(), token.size());
    }
    const auto packet = protocol::BuildPacket(protocol::MsgType::AUTH_RESULT, &result, sizeof(result));
    return WriteAllSsl(ssl, packet.data(), packet.size());
}

bool NetworkServer::AuthenticatePair(SSL* ssl, SOCKET socket, const protocol::AuthPair& request,
                                     const std::string& peer_ip) {
    (void)socket;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        auto& attempts = pair_attempts_[peer_ip];
        if (attempts.locked_until > now) {
            SendAuthResult(ssl, protocol::AuthStatus::RATE_LIMITED);
            return false;
        }
    }

    size_t pin_length = 0;
    while (pin_length < sizeof(request.pin) && request.pin[pin_length] != '\0') ++pin_length;
    const bool valid_format = pin_length == 6 &&
        std::all_of(request.pin, request.pin + pin_length,
                    [](char value) { return value >= '0' && value <= '9'; });
    const bool pin_matches = valid_format && pin_.size() == 6 &&
        CRYPTO_memcmp(request.pin, pin_.data(), 6) == 0;

    if (!pin_matches) {
        bool limited = false;
        {
            std::lock_guard<std::mutex> lock(auth_mutex_);
            auto& attempts = pair_attempts_[peer_ip];
            ++attempts.failures;
            if (attempts.failures >= kMaxPinFailures) {
                attempts.locked_until = now + kPinLockout;
                attempts.failures = 0;
                limited = true;
            }
            if (pair_attempts_.size() > 256) {
                pair_attempts_.clear();
            }
        }
        SendAuthResult(ssl, limited ? protocol::AuthStatus::RATE_LIMITED
                                    : protocol::AuthStatus::INVALID_PIN);
        return false;
    }

    unsigned char random_bytes[32];
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1) {
        return false;
    }
    const std::string token = HexFingerprint(random_bytes, sizeof(random_bytes));
    {
        std::lock_guard<std::mutex> lock(auth_mutex_);
        session_token_ = token;
        pair_attempts_.erase(peer_ip);
    }
    if (!SendAuthResult(ssl, protocol::AuthStatus::OK, token)) {
        return false;
    }
    client_authenticated_ = true;
    std::cout << "[Session] PIN pairing succeeded; remote input is enabled\n";
    return true;
}

bool NetworkServer::AuthenticateSession(SSL* ssl, SOCKET socket,
                                        const protocol::AuthSession& request) {
    (void)socket;
    std::lock_guard<std::mutex> lock(auth_mutex_);
    const bool valid = session_token_.size() == protocol::kSessionTokenLength &&
        CRYPTO_memcmp(request.token, session_token_.data(), protocol::kSessionTokenLength) == 0;
    if (!valid) {
        SendAuthResult(ssl, protocol::AuthStatus::INVALID_SESSION);
        return false;
    }
    if (!SendAuthResult(ssl, protocol::AuthStatus::OK, session_token_)) {
        return false;
    }
    client_authenticated_ = true;
    std::cout << "[Session] Saved session accepted; remote input is enabled\n";
    return true;
}

void NetworkServer::ClientReadLoop(SSL* ssl, SOCKET socket, const std::string& peer_ip) {
    (void)socket;
    protocol::PacketHeader header{};
    std::vector<uint8_t> payload;
    bool pin_attempted = false;

    for (int message_count = 0;
         running_ && !client_authenticated_ && message_count < 2;
         ++message_count) {
        if (!ReadClientPacket(ssl, header, payload, false)) return;
        const auto type = static_cast<protocol::MsgType>(header.msg_type);
        if (type != protocol::MsgType::AUTH_PAIR && type != protocol::MsgType::AUTH_SESSION) return;

        uint32_t version = 0;
        std::memcpy(&version, payload.data(), sizeof(version));
        if (version != protocol::kVersion) {
            SendAuthResult(ssl, protocol::AuthStatus::VERSION_MISMATCH);
            return;
        }

        if (type == protocol::MsgType::AUTH_PAIR) {
            pin_attempted = true;
            protocol::AuthPair request{};
            std::memcpy(&request, payload.data(), sizeof(request));
            if (!AuthenticatePair(ssl, socket, request, peer_ip)) return;
        } else {
            protocol::AuthSession request{};
            std::memcpy(&request, payload.data(), sizeof(request));
            AuthenticateSession(ssl, socket, request);
        }
        if (pin_attempted && !client_authenticated_) return;
    }
    if (!client_authenticated_) return;

    while (running_ && client_authenticated_) {
        if (!ReadClientPacket(ssl, header, payload, true)) break;
        const auto type = static_cast<protocol::MsgType>(header.msg_type);
        if (type == protocol::MsgType::INPUT_EVENT) {
            protocol::InputEvent event{};
            std::memcpy(&event, payload.data(), sizeof(event));
            if (input_cb_) input_cb_(event);
        } else if (type == protocol::MsgType::HEARTBEAT) {
            if (!SendPacket(protocol::MsgType::HEARTBEAT, nullptr, 0)) break;
        } else if (type == protocol::MsgType::REQUEST_KEYFRAME) {
            if (keyframe_cb_) keyframe_cb_();
        }
    }
    client_authenticated_ = false;
}

bool NetworkServer::SendPacket(protocol::MsgType type, const void* data, uint32_t len) {
    if (!client_authenticated_ || len > protocol::kMaxVideoPayload) {
        return false;
    }
    const auto packet = protocol::BuildPacket(type, data, len);
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    std::lock_guard<std::mutex> client_lock(client_mutex_);
    if (!client_authenticated_ || !ssl_ || client_socket_ == INVALID_SOCKET) {
        return false;
    }
    if (!WriteAllSsl(ssl_, packet.data(), packet.size())) {
        client_authenticated_ = false;
        shutdown(client_socket_, SD_BOTH);
        return false;
    }
    return true;
}

void NetworkServer::SendVideoConfig(uint32_t width, uint32_t height,
                                    const std::vector<uint8_t>& sps,
                                    const std::vector<uint8_t>& pps) {
    if (sps.empty() || pps.empty() || sps.size() > UINT16_MAX || pps.size() > UINT16_MAX) {
        return;
    }
    protocol::VideoConfig config{};
    config.width = width;
    config.height = height;
    config.sps_length = static_cast<uint16_t>(sps.size());
    config.pps_length = static_cast<uint16_t>(pps.size());
    std::vector<uint8_t> payload(sizeof(config) + sps.size() + pps.size());
    std::memcpy(payload.data(), &config, sizeof(config));
    std::memcpy(payload.data() + sizeof(config), sps.data(), sps.size());
    std::memcpy(payload.data() + sizeof(config) + sps.size(), pps.data(), pps.size());
    SendPacket(protocol::MsgType::VIDEO_CONFIG, payload.data(), static_cast<uint32_t>(payload.size()));
}

void NetworkServer::SendAudioConfig(uint32_t sample_rate, uint16_t channels) {
    protocol::AudioConfig config{};
    config.sample_rate = sample_rate;
    config.channels = channels;
    config.bits_per_sample = 16;
    SendPacket(protocol::MsgType::AUDIO_CONFIG, &config, sizeof(config));
}

void NetworkServer::SendVideoFrame(const EncodedPacket& packet) {
    if (packet.data.size() <= protocol::kMaxVideoPayload) {
        SendPacket(protocol::MsgType::VIDEO_H264, packet.data.data(),
                   static_cast<uint32_t>(packet.data.size()));
    }
}

void NetworkServer::SendAudioFrame(const PCMFrame& packet) {
    const size_t bytes = packet.samples.size() * sizeof(int16_t);
    if (bytes <= protocol::kMaxAudioPayload) {
        SendPacket(protocol::MsgType::AUDIO_PCM, packet.samples.data(), static_cast<uint32_t>(bytes));
    }
}
