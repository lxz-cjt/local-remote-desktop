#include "audio_manager.h"

#include "audio_capture.h"
#include "network_server.h"

#include <atomic>
#include <chrono>
#include <iostream>

struct AudioManager::State {
    std::shared_ptr<AudioCapture> capture = std::make_shared<AudioCapture>();
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
};

AudioManager::AudioManager(std::shared_ptr<NetworkServer> server)
    : server_(std::move(server)) {
}

AudioManager::~AudioManager() {
    Stop();
}

void AudioManager::Start() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (state_ && !state_->done) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    state_ = std::make_shared<State>();
    const auto state = state_;
    const auto server = server_;
    worker_ = std::thread([server, state]() {
        if (state->capture->Initialize() && !state->stop && state->capture->Start() && !state->stop) {
            uint32_t sample_rate = 0;
            uint16_t channels = 0;
            state->capture->GetFormat(sample_rate, channels);
            server->SendAudioConfig(sample_rate, channels);
            std::cout << "[Audio] WASAPI " << sample_rate << " Hz mix format, PCM16 stereo output\n";

            while (!state->stop && server->IsClientConnected()) {
                PCMFrame frame;
                if (state->capture->GetFrame(frame)) {
                    server->SendAudioFrame(frame);
                } else {
                    Sleep(5);
                }
            }
        } else if (!state->stop) {
            std::cerr << "[Audio] WASAPI unavailable; continuing with video only\n";
        }
        state->capture->Stop();
        state->done = true;
    });
}

void AudioManager::Stop() {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!state_) {
        return;
    }
    state_->stop = true;
    if (!worker_.joinable()) {
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!state_->done && std::chrono::steady_clock::now() < deadline) {
        Sleep(10);
    }
    if (state_->done) {
        worker_.join();
    } else {
        worker_.detach();
        std::cerr << "[Audio] Audio driver did not stop; audio is disabled until the driver responds\n";
    }
}
