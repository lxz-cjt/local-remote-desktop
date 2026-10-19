#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cstdint>
#include <vector>

struct EncodedPacket {
    std::vector<uint8_t> data;
    uint64_t timestamp_us = 0;
    bool is_keyframe = false;
};

struct H264CodecConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
};

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    bool Initialize(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps);
    bool GetCodecConfig(H264CodecConfig& config) const;
    bool EncodeFrame(const uint8_t* bgra_data, uint32_t width, uint32_t height, EncodedPacket& packet);
    void ForceKeyFrame() { force_keyframe_ = true; }

private:
    AVCodecContext* codec_ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    int64_t pts_ = 0;
    uint32_t encoder_width_ = 0;
    uint32_t encoder_height_ = 0;
    bool force_keyframe_ = true;
};
