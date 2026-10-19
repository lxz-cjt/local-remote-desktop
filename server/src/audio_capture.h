#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

struct PCMFrame {
    std::vector<int16_t> samples;
    uint64_t timestamp_us = 0;
};

class AudioCapture {
public:
    AudioCapture();
    ~AudioCapture();

    bool Initialize();
    bool Start();
    void Stop();
    bool GetFrame(PCMFrame& frame);
    void GetFormat(uint32_t& sample_rate, uint16_t& channels) const;

private:
    void CaptureLoop();
    void AppendSamples(const BYTE* data, UINT32 frames, bool silent);

    IAudioClient* audio_client_ = nullptr;
    IAudioCaptureClient* capture_client_ = nullptr;
    WAVEFORMATEX* wave_format_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::vector<int16_t> buffer_;
    CRITICAL_SECTION lock_;
    uint32_t sample_rate_ = 0;
    uint16_t source_channels_ = 0;
    bool float32_source_ = false;
    bool pcm16_source_ = false;
};
