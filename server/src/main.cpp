#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <openssl/applink.c>
#endif
#include <openssl/rand.h>

#include "audio_manager.h"
#include "certificate.h"
#include "desktop_capture.h"
#include "input_injector.h"
#include "network_server.h"
#include "video_encoder.h"

namespace {

std::atomic<bool> g_running{true};

BOOL WINAPI HandleConsoleEvent(DWORD event_type) {
    if (event_type == CTRL_C_EVENT || event_type == CTRL_BREAK_EVENT ||
        event_type == CTRL_CLOSE_EVENT || event_type == CTRL_SHUTDOWN_EVENT) {
        g_running = false;
        return TRUE;
    }
    return FALSE;
}

std::string GeneratePin() {
    unsigned char random_bytes[4];
    if (RAND_bytes(random_bytes, sizeof(random_bytes)) != 1) {
        throw std::runtime_error("OpenSSL could not generate a pairing PIN");
    }
    const uint32_t value = (static_cast<uint32_t>(random_bytes[0]) << 24) |
        (static_cast<uint32_t>(random_bytes[1]) << 16) |
        (static_cast<uint32_t>(random_bytes[2]) << 8) |
        random_bytes[3];
    std::ostringstream pin;
    pin << std::setw(6) << std::setfill('0') << value % 1000000;
    return pin.str();
}

uint16_t ParsePort(int argc, char** argv) {
    if (argc < 2) return 48200;
    char* end = nullptr;
    const unsigned long port = std::strtoul(argv[1], &end, 10);
    if (!end || *end != '\0' || port < 1024 || port >= 65535) {
        throw std::runtime_error("Port must be between 1024 and 65534");
    }
    return static_cast<uint16_t>(port);
}

} // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    SetConsoleCtrlHandler(nullptr, FALSE);
    if (!SetConsoleCtrlHandler(HandleConsoleEvent, TRUE)) {
        std::cerr << "[Fatal] Could not register the console stop handler\n";
        return 1;
    }

    try {
        const uint16_t tls_port = ParsePort(argc, argv);
        const uint16_t http_port = static_cast<uint16_t>(tls_port + 1);
        const std::string pin = GeneratePin();

        std::cout << "========================================\n"
                  << "  Local Remote Desktop Server v" LRDC_VERSION "\n"
                  << "  Trusted-LAN security MVP\n"
                  << "========================================\n";

        std::string cert_file;
        std::string key_file;
        std::string certificate_error;
        if (!EnsureCertificate(cert_file, key_file, certificate_error)) {
            std::cerr << "[TLS] " << certificate_error << "\n";
            return 1;
        }

        InputInjector injector;
        std::atomic<bool> keyframe_requested{false};
        auto server = std::make_shared<NetworkServer>(tls_port, http_port, pin);
        AudioManager audio_manager(server);
        server->SetInputCallback([&injector](const protocol::InputEvent& event) {
            injector.InjectInput(event);
        });
        server->SetKeyFrameCallback([&keyframe_requested]() {
            keyframe_requested = true;
        });

        if (!server->Start(cert_file, key_file)) {
            std::cerr << "[Network] Failed to start the server\n";
            return 1;
        }

        std::cout << "\n=== Pairing Information ===\n"
                  << "  PIN (valid for this server run): " << pin << "\n"
                  << "  Compare the certificate fingerprint above with the Android prompt.\n"
                  << "  Pairing metadata URL: " << server->GetPairingUrl() << "\n"
                  << "============================\n\n"
                  << "[Server] Press Ctrl+C to stop. Media capture is idle until authentication.\n";

        std::unique_ptr<DesktopCapture> capture;
        std::unique_ptr<H264Encoder> encoder;
        bool media_session = false;
        uint32_t frames_sent = 0;
        auto last_status = std::chrono::steady_clock::now();
        auto next_media_attempt = std::chrono::steady_clock::time_point{};

        while (g_running) {
            const bool connected = server->IsClientConnected();
            if (connected && !media_session && std::chrono::steady_clock::now() >= next_media_attempt) {
                capture = std::make_unique<DesktopCapture>();
                encoder = std::make_unique<H264Encoder>();

                uint32_t width = 0;
                uint32_t height = 0;
                H264CodecConfig config;
                if (!capture->Initialize()) {
                    std::cerr << "[Capture] Desktop Duplication initialization failed\n";
                } else {
                    capture->GetScreenSize(width, height);
                    if (!encoder->Initialize(width, height, 30, 4000) ||
                        !encoder->GetCodecConfig(config)) {
                        std::cerr << "[Video] Encoder initialization failed\n";
                    } else {
                        server->SendVideoConfig(config.width, config.height, config.sps, config.pps);
                        media_session = true;
                        keyframe_requested = true;
                        audio_manager.Start();
                        std::cout << "[Capture] Authenticated session started at " << width << "x" << height << "\n";
                    }
                }

                if (!media_session) {
                    capture.reset();
                    encoder.reset();
                    next_media_attempt = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                }
            }

            if (!connected && media_session) {
                audio_manager.Stop();
                encoder.reset();
                capture.reset();
                media_session = false;
                std::cout << "[Capture] Media capture stopped\n";
            }

            if (connected && media_session && capture && encoder) {
                if (keyframe_requested.exchange(false)) encoder->ForceKeyFrame();

                CapturedFrame frame;
                if (capture->CaptureFrame(frame)) {
                    EncodedPacket packet;
                    if (encoder->EncodeFrame(frame.rgba_data.data(), frame.width, frame.height, packet)) {
                        packet.timestamp_us = frame.timestamp_us;
                        server->SendVideoFrame(packet);
                        ++frames_sent;
                    }
                }

                const auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_status).count() >= 5) {
                    std::cout << "[Status] " << frames_sent << " encoded frames in the last 5 seconds\n";
                    frames_sent = 0;
                    last_status = now;
                }
                Sleep(20);
            } else {
                Sleep(50);
            }
        }

        std::cout << "\n[Server] Shutting down...\n";
        audio_manager.Stop();
        server->Stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[Fatal] " << error.what() << "\n";
        return 1;
    }
}
