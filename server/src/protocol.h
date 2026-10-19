#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace protocol {

constexpr uint32_t kVersion = 2;
constexpr uint32_t kMaxVideoPayload = 8 * 1024 * 1024;
constexpr uint32_t kMaxAudioPayload = 256 * 1024;
constexpr uint32_t kMaxConfigPayload = 64 * 1024;
constexpr size_t kSessionTokenLength = 64;

enum class MsgType : uint8_t {
    VIDEO_H264 = 0x01,
    AUDIO_PCM = 0x02,
    INPUT_EVENT = 0x03,
    SERVER_STATUS = 0x04,
    HEARTBEAT = 0x05,
    VIDEO_CONFIG = 0x06,
    AUTH_PAIR = 0x07,
    AUTH_SESSION = 0x08,
    AUTH_RESULT = 0x09,
    AUDIO_CONFIG = 0x0A,
    REQUEST_KEYFRAME = 0x0B
};

enum class AuthStatus : uint8_t {
    OK = 0,
    INVALID_PIN = 1,
    RATE_LIMITED = 2,
    INVALID_SESSION = 3,
    VERSION_MISMATCH = 4
};

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t payload_len;
    uint8_t msg_type;
};

struct InputEvent {
    uint64_t timestamp_us;
    uint32_t seq_num;
    uint8_t event_type;
    float norm_x;
    float norm_y;
    uint32_t client_width;
    uint32_t client_height;
    uint8_t mouse_action;
    uint8_t mouse_button;
    int16_t scroll_dx;
    int16_t scroll_dy;
    uint32_t key_code;
    uint8_t key_pressed;
};

struct ServerStatus {
    uint32_t desktop_width;
    uint32_t desktop_height;
    uint8_t capture_paused;
};

struct AuthPair {
    uint32_t protocol_version;
    char pin[8];
};

struct AuthSession {
    uint32_t protocol_version;
    char token[kSessionTokenLength];
};

struct AuthResult {
    uint8_t status;
    char token[kSessionTokenLength];
};

struct VideoConfig {
    uint32_t width;
    uint32_t height;
    uint16_t sps_length;
    uint16_t pps_length;
};

struct AudioConfig {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
};
#pragma pack(pop)

static_assert(sizeof(PacketHeader) == 5, "Unexpected packet header layout");
static_assert(sizeof(InputEvent) == 40, "Unexpected input event layout");
static_assert(sizeof(AuthPair) == 12, "Unexpected pair request layout");
static_assert(sizeof(AuthSession) == 68, "Unexpected session request layout");
static_assert(sizeof(AuthResult) == 65, "Unexpected auth result layout");
static_assert(sizeof(VideoConfig) == 12, "Unexpected video config layout");
static_assert(sizeof(AudioConfig) == 8, "Unexpected audio config layout");

inline std::vector<uint8_t> BuildPacket(MsgType type, const void* data, size_t len) {
    std::vector<uint8_t> packet(sizeof(PacketHeader) + len);
    auto* header = reinterpret_cast<PacketHeader*>(packet.data());
    header->payload_len = static_cast<uint32_t>(len);
    header->msg_type = static_cast<uint8_t>(type);
    if (len > 0 && data != nullptr) {
        std::memcpy(packet.data() + sizeof(PacketHeader), data, len);
    }
    return packet;
}

} // namespace protocol
