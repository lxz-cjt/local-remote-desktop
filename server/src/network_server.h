#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "protocol.h"

struct EncodedPacket;
struct PCMFrame;

class NetworkServer {
public:
    using InputCallback = std::function<void(const protocol::InputEvent&)>;
    using KeyFrameCallback = std::function<void()>;

    NetworkServer(uint16_t tls_port, uint16_t http_port, std::string pin);
    ~NetworkServer();

    bool Start(const std::string& cert_pem, const std::string& key_pem);
    void Stop();

    void SetInputCallback(InputCallback callback) { input_cb_ = std::move(callback); }
    void SetKeyFrameCallback(KeyFrameCallback callback) { keyframe_cb_ = std::move(callback); }

    void SendVideoConfig(uint32_t width, uint32_t height,
                         const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps);
    void SendAudioConfig(uint32_t sample_rate, uint16_t channels);
    void SendVideoFrame(const EncodedPacket& packet);
    void SendAudioFrame(const PCMFrame& packet);

    bool IsClientConnected() const { return client_authenticated_.load(); }
    std::string GetPairingUrl() const;

private:
    struct PairAttempts {
        unsigned int failures = 0;
        std::chrono::steady_clock::time_point locked_until{};
    };

    void TlsAcceptLoop();
    void HttpServerLoop();
    void ClientReadLoop(SSL* ssl, SOCKET socket, const std::string& peer_ip);
    bool AuthenticatePair(SSL* ssl, SOCKET socket, const protocol::AuthPair& request,
                          const std::string& peer_ip);
    bool AuthenticateSession(SSL* ssl, SOCKET socket, const protocol::AuthSession& request);
    bool SendAuthResult(SSL* ssl, protocol::AuthStatus status, const std::string& token = {});
    bool SendPacket(protocol::MsgType type, const void* data, uint32_t len);
    bool InitializeSslContext(const std::string& cert, const std::string& key);
    void ShutdownClient();
    void DisconnectAuthenticatedClient();

    uint16_t tls_port_;
    uint16_t http_port_;
    const std::string pin_;

    SOCKET listen_socket_ = INVALID_SOCKET;
    SOCKET http_socket_ = INVALID_SOCKET;
    SOCKET client_socket_ = INVALID_SOCKET;
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
    std::thread accept_thread_;
    std::thread http_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> client_authenticated_{false};

    std::string local_ip_;
    std::string cert_fingerprint_;
    std::string session_token_;

    InputCallback input_cb_;
    KeyFrameCallback keyframe_cb_;
    std::mutex send_mutex_;
    std::mutex client_mutex_;
    std::mutex auth_mutex_;
    std::unordered_map<std::string, PairAttempts> pair_attempts_;
};
