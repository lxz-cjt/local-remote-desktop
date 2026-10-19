#include "audio_capture.h"

#include <ksmedia.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>

namespace {

int16_t FloatToPcm16(float value) {
    value = std::max(-1.0f, std::min(1.0f, value));
    return static_cast<int16_t>(std::lround(value * 32767.0f));
}

} // namespace

AudioCapture::AudioCapture() {
    InitializeCriticalSection(&lock_);
}

AudioCapture::~AudioCapture() {
    Stop();
    DeleteCriticalSection(&lock_);
    if (wave_format_) CoTaskMemFree(wave_format_);
    if (capture_client_) capture_client_->Release();
    if (audio_client_) audio_client_->Release();
}

bool AudioCapture::Initialize() {
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) return false;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return false;

    IMMDevice* device = nullptr;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    enumerator->Release();
    if (FAILED(hr)) return false;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&audio_client_));
    device->Release();
    if (FAILED(hr)) return false;

    hr = audio_client_->GetMixFormat(&wave_format_);
    if (FAILED(hr) || !wave_format_) return false;

    sample_rate_ = wave_format_->nSamplesPerSec;
    source_channels_ = wave_format_->nChannels;
    WORD format_tag = wave_format_->wFormatTag;
    if (format_tag == WAVE_FORMAT_EXTENSIBLE &&
        wave_format_->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wave_format_);
        format_tag = static_cast<WORD>(extensible->SubFormat.Data1);
    }
    float32_source_ = format_tag == WAVE_FORMAT_IEEE_FLOAT && wave_format_->wBitsPerSample == 32;
    pcm16_source_ = format_tag == WAVE_FORMAT_PCM && wave_format_->wBitsPerSample == 16;
    if (sample_rate_ == 0 || source_channels_ == 0 || (!float32_source_ && !pcm16_source_)) {
        std::cerr << "[Audio] Only float32 and PCM16 WASAPI mix formats are supported\n";
        return false;
    }

    const REFERENCE_TIME buffer_duration = 0;
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                   buffer_duration, 0, wave_format_, nullptr);
    if (FAILED(hr)) return false;

    hr = audio_client_->GetService(__uuidof(IAudioCaptureClient),
                                   reinterpret_cast<void**>(&capture_client_));
    return SUCCEEDED(hr);
}

bool AudioCapture::Start() {
    if (!audio_client_ || !capture_client_ || running_.exchange(true)) return false;
    if (FAILED(audio_client_->Start())) {
        running_ = false;
        return false;
    }
    thread_ = std::thread(&AudioCapture::CaptureLoop, this);
    return true;
}

void AudioCapture::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (audio_client_) audio_client_->Stop();
    EnterCriticalSection(&lock_);
    buffer_.clear();
    LeaveCriticalSection(&lock_);
}

void AudioCapture::GetFormat(uint32_t& sample_rate, uint16_t& channels) const {
    sample_rate = sample_rate_;
    channels = 2;
}

void AudioCapture::AppendSamples(const BYTE* data, UINT32 frames, bool silent) {
    if (frames == 0) return;
    EnterCriticalSection(&lock_);
    const size_t old_size = buffer_.size();
    buffer_.resize(old_size + static_cast<size_t>(frames) * 2);
    int16_t* output = buffer_.data() + old_size;

    for (UINT32 frame = 0; frame < frames; ++frame) {
        if (silent || !data) {
            output[frame * 2] = 0;
            output[frame * 2 + 1] = 0;
            continue;
        }
        if (float32_source_) {
            const auto* input = reinterpret_cast<const float*>(data);
            const float left = input[frame * source_channels_];
            const float right = source_channels_ == 1 ? left : input[frame * source_channels_ + 1];
            output[frame * 2] = FloatToPcm16(left);
            output[frame * 2 + 1] = FloatToPcm16(right);
        } else {
            const auto* input = reinterpret_cast<const int16_t*>(data);
            const int16_t left = input[frame * source_channels_];
            const int16_t right = source_channels_ == 1 ? left : input[frame * source_channels_ + 1];
            output[frame * 2] = left;
            output[frame * 2 + 1] = right;
        }
    }

    const size_t max_buffered_samples = static_cast<size_t>(sample_rate_) * 2 / 2;
    if (buffer_.size() > max_buffered_samples) {
        buffer_.erase(buffer_.begin(), buffer_.end() - max_buffered_samples);
    }
    LeaveCriticalSection(&lock_);
}

void AudioCapture::CaptureLoop() {
    while (running_) {
        UINT32 packet_length = 0;
        HRESULT hr = capture_client_->GetNextPacketSize(&packet_length);
        if (FAILED(hr)) break;

        while (packet_length != 0 && running_) {
            BYTE* data = nullptr;
            DWORD flags = 0;
            UINT32 frames_available = 0;
            hr = capture_client_->GetBuffer(&data, &frames_available, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            AppendSamples(data, frames_available, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            capture_client_->ReleaseBuffer(frames_available);
            hr = capture_client_->GetNextPacketSize(&packet_length);
            if (FAILED(hr)) break;
        }
        Sleep(2);
    }
}

bool AudioCapture::GetFrame(PCMFrame& frame) {
    EnterCriticalSection(&lock_);
    const size_t samples_per_frame = static_cast<size_t>(sample_rate_ / 100) * 2;
    if (samples_per_frame == 0 || buffer_.size() < samples_per_frame) {
        LeaveCriticalSection(&lock_);
        return false;
    }
    frame.samples.assign(buffer_.begin(), buffer_.begin() + samples_per_frame);
    buffer_.erase(buffer_.begin(), buffer_.begin() + samples_per_frame);
    LeaveCriticalSection(&lock_);

    frame.timestamp_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    return true;
}
