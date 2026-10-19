#include "video_encoder.h"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace {

size_t StartCodeSize(const uint8_t* data, size_t size) {
    if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) return 4;
    if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) return 3;
    return 0;
}

void AppendAnnexBNal(std::vector<uint8_t>& output, const uint8_t* data, size_t size) {
    const uint8_t start_code[] = {0, 0, 0, 1};
    output.insert(output.end(), std::begin(start_code), std::end(start_code));
    output.insert(output.end(), data, data + size);
}

bool ExtractAvcCConfig(const uint8_t* data, size_t size, H264CodecConfig& config) {
    if (size < 7 || data[0] != 1) return false;
    size_t offset = 6;
    const size_t sps_count = data[5] & 0x1F;
    for (size_t i = 0; i < sps_count; ++i) {
        if (offset + 2 > size) return false;
        const size_t length = (static_cast<size_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        if (length == 0 || offset + length > size) return false;
        if (i == 0) config.sps.assign(data + offset, data + offset + length);
        offset += length;
    }
    if (offset >= size) return false;
    const size_t pps_count = data[offset++];
    for (size_t i = 0; i < pps_count; ++i) {
        if (offset + 2 > size) return false;
        const size_t length = (static_cast<size_t>(data[offset]) << 8) | data[offset + 1];
        offset += 2;
        if (length == 0 || offset + length > size) return false;
        if (i == 0) config.pps.assign(data + offset, data + offset + length);
        offset += length;
    }
    return !config.sps.empty() && !config.pps.empty();
}

bool ExtractAnnexBConfig(const uint8_t* data, size_t size, H264CodecConfig& config) {
    size_t offset = 0;
    while (offset < size) {
        const size_t start = StartCodeSize(data + offset, size - offset);
        if (start == 0) {
            ++offset;
            continue;
        }
        const size_t nal_start = offset + start;
        size_t next = nal_start;
        while (next < size && StartCodeSize(data + next, size - next) == 0) ++next;
        if (nal_start < next) {
            const uint8_t nal_type = data[nal_start] & 0x1F;
            if (nal_type == 7 && config.sps.empty()) {
                config.sps.assign(data + nal_start, data + next);
            } else if (nal_type == 8 && config.pps.empty()) {
                config.pps.assign(data + nal_start, data + next);
            }
        }
        offset = next;
    }
    return !config.sps.empty() && !config.pps.empty();
}

bool ConvertAvcCToAnnexB(const AVPacket* packet, std::vector<uint8_t>& output) {
    size_t offset = 0;
    output.clear();
    while (offset < static_cast<size_t>(packet->size)) {
        if (offset + 4 > static_cast<size_t>(packet->size)) return false;
        const uint32_t length = (static_cast<uint32_t>(packet->data[offset]) << 24) |
            (static_cast<uint32_t>(packet->data[offset + 1]) << 16) |
            (static_cast<uint32_t>(packet->data[offset + 2]) << 8) |
            packet->data[offset + 3];
        offset += 4;
        if (length == 0 || offset + length > static_cast<size_t>(packet->size)) return false;
        AppendAnnexBNal(output, packet->data + offset, length);
        offset += length;
    }
    return !output.empty();
}

} // namespace

H264Encoder::H264Encoder() = default;

H264Encoder::~H264Encoder() {
    if (sws_ctx_) sws_freeContext(sws_ctx_);
    if (frame_) av_frame_free(&frame_);
    if (codec_ctx_) avcodec_free_context(&codec_ctx_);
}

bool H264Encoder::Initialize(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrate_kbps) {
    if (width == 0 || height == 0 || width % 2 != 0 || height % 2 != 0 || fps == 0) {
        std::cerr << "[Video] H.264 YUV420 requires non-zero even dimensions\n";
        return false;
    }
    encoder_width_ = width;
    encoder_height_ = height;

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) {
        std::cerr << "[Video] FFmpeg was built without the libx264 encoder\n";
        return false;
    }
    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) return false;

    codec_ctx_->width = static_cast<int>(width);
    codec_ctx_->height = static_cast<int>(height);
    codec_ctx_->time_base = AVRational{1, static_cast<int>(fps)};
    codec_ctx_->framerate = AVRational{static_cast<int>(fps), 1};
    codec_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    codec_ctx_->bit_rate = static_cast<int64_t>(bitrate_kbps) * 1000;
    codec_ctx_->gop_size = static_cast<int>(fps);
    codec_ctx_->max_b_frames = 0;
    codec_ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (av_opt_set(codec_ctx_->priv_data, "preset", "ultrafast", 0) < 0 ||
        av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0) < 0 ||
        av_opt_set(codec_ctx_->priv_data, "profile", "baseline", 0) < 0 ||
        avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        std::cerr << "[Video] Could not initialize libx264\n";
        return false;
    }

    frame_ = av_frame_alloc();
    if (!frame_) return false;
    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width = static_cast<int>(width);
    frame_->height = static_cast<int>(height);
    if (av_frame_get_buffer(frame_, 0) < 0) return false;

    sws_ctx_ = sws_getContext(
        static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_BGRA,
        static_cast<int>(width), static_cast<int>(height), AV_PIX_FMT_YUV420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    return sws_ctx_ != nullptr;
}

bool H264Encoder::GetCodecConfig(H264CodecConfig& config) const {
    if (!codec_ctx_ || !codec_ctx_->extradata || codec_ctx_->extradata_size <= 0) return false;
    config.width = encoder_width_;
    config.height = encoder_height_;
    const auto* data = codec_ctx_->extradata;
    const size_t size = static_cast<size_t>(codec_ctx_->extradata_size);
    return ExtractAvcCConfig(data, size, config) || ExtractAnnexBConfig(data, size, config);
}

bool H264Encoder::EncodeFrame(const uint8_t* bgra_data, uint32_t width, uint32_t height,
                              EncodedPacket& packet) {
    if (!codec_ctx_ || !frame_ || !sws_ctx_ || !bgra_data ||
        width != encoder_width_ || height != encoder_height_) {
        return false;
    }
    if (av_frame_make_writable(frame_) < 0) return false;

    const uint8_t* source_slices[] = {bgra_data};
    const int source_strides[] = {static_cast<int>(width * 4)};
    if (sws_scale(sws_ctx_, source_slices, source_strides, 0, static_cast<int>(height),
                  frame_->data, frame_->linesize) <= 0) {
        return false;
    }

    frame_->pts = pts_++;
    frame_->pict_type = (force_keyframe_ || frame_->pts % codec_ctx_->gop_size == 0)
        ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    force_keyframe_ = false;

    if (avcodec_send_frame(codec_ctx_, frame_) < 0) return false;
    AVPacket* encoded = av_packet_alloc();
    if (!encoded) return false;
    const int result = avcodec_receive_packet(codec_ctx_, encoded);
    if (result < 0) {
        av_packet_free(&encoded);
        return false;
    }

    packet.is_keyframe = (encoded->flags & AV_PKT_FLAG_KEY) != 0;
    bool converted = true;
    if (StartCodeSize(encoded->data, static_cast<size_t>(encoded->size)) == 0) {
        converted = ConvertAvcCToAnnexB(encoded, packet.data);
    } else {
        packet.data.assign(encoded->data, encoded->data + encoded->size);
    }
    av_packet_free(&encoded);
    return converted && !packet.data.empty();
}
