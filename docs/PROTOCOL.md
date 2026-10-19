# Protocol version 2

## Transport and framing

Media and control messages use TCP with TLS 1.2 or newer. Android authenticates the self-signed certificate by its SHA-256 fingerprint, not by a public CA or hostname. Multi-byte integers and IEEE-754 floats in the binary protocol are little-endian. Structures are packed without padding.

Every packet is:

```text
uint32 payload_length
uint8  message_type
uint8  payload[payload_length]
```

The five-byte header is not included in `payload_length`. Both peers read and write the complete header and payload, reject invalid lengths before allocation, and close on framing, timeout, or TLS errors.

## Message types

| Value | Name | Direction | Payload |
| ---: | --- | --- | --- |
| 0x01 | `VIDEO_H264` | Server to client | Annex-B H.264 access unit, 1 byte to 8 MiB |
| 0x02 | `AUDIO_PCM` | Server to client | Interleaved little-endian PCM16 stereo, up to 256 KiB |
| 0x03 | `INPUT_EVENT` | Client to server | Exactly 40-byte `InputEvent` |
| 0x04 | `SERVER_STATUS` | Server to client | Exactly 9 bytes; reserved for status updates |
| 0x05 | `HEARTBEAT` | Either | Empty; the server replies to authenticated client heartbeats |
| 0x06 | `VIDEO_CONFIG` | Server to client | 12-byte header plus SPS and PPS, up to 64 KiB |
| 0x07 | `AUTH_PAIR` | Client to server | Exactly 12 bytes |
| 0x08 | `AUTH_SESSION` | Client to server | Exactly 68 bytes |
| 0x09 | `AUTH_RESULT` | Server to client | Exactly 65 bytes |
| 0x0A | `AUDIO_CONFIG` | Server to client | Exactly 8 bytes |
| 0x0B | `REQUEST_KEYFRAME` | Client to server | Empty |

The server rejects every other client-to-server message. Before authentication it rejects media, input, heartbeat and keyframe requests. Android similarly rejects unexpected server messages.

## Authentication payloads

```text
AuthPair:
  uint32 protocol_version   # 2
  char   pin[8]             # six ASCII digits followed by NUL padding

AuthSession:
  uint32 protocol_version   # 2
  char   token[64]          # 32 random bytes encoded as 64 lowercase hex characters

AuthResult:
  uint8  status
  char   token[64]          # present only when status is OK; otherwise zero-filled
```

`AuthResult.status` values are `0=OK`, `1=INVALID_PIN`, `2=RATE_LIMITED`, `3=INVALID_SESSION`, and `4=VERSION_MISMATCH`.

A client with a saved token sends `AUTH_SESSION`. If the token is rejected, it can send one `AUTH_PAIR` on the same unauthenticated connection. A failed PIN closes that connection. Five failed PIN attempts from one address trigger a 60-second in-memory lockout. Socket read timeout is 15 seconds. The PIN changes each time the server starts; tokens are memory-only and also become invalid on restart.

Only after an OK result does the server enable media sending and input handling. Authentication results are sent before the authenticated flag is set, so media cannot overtake the result on the TLS stream.

## Video configuration

```text
uint32 width
uint32 height
uint16 sps_length
uint16 pps_length
uint8  sps[sps_length]      # one raw H.264 SPS NAL unit, no Annex-B start code
uint8  pps[pps_length]      # one raw H.264 PPS NAL unit, no Annex-B start code
```

Dimensions come from the active DXGI duplication output and the encoder. Android places SPS/PPS in `csd-0`/`csd-1` with Annex-B start codes and configures `MediaCodec` with these dimensions. The server requests an IDR for each new media session and honors authenticated `REQUEST_KEYFRAME` messages.

## Audio configuration

```text
uint32 sample_rate
uint16 channels             # 2 in v0.2.0
uint16 bits_per_sample      # 16 in v0.2.0
```

WASAPI uses the device's actual shared mix format. Float32 and PCM16 input are converted to interleaved PCM16 stereo at the original mix sample rate. Mono is duplicated; for multichannel input, the first two channels are used. No resampling or audio compression is performed.

## Input event

The 40-byte packed layout is:

```text
uint64 timestamp_us
uint32 seq_num
uint8  event_type           # 0=mouse, 1=keyboard, 2=scroll
float  norm_x               # normalized to [0,1]
float  norm_y               # normalized to [0,1]
uint32 client_width
uint32 client_height
uint8  mouse_action         # 0=down, 1=up, 2=move
uint8  mouse_button         # 0=left, 1=right, 2=middle
int16  scroll_dx
int16  scroll_dy
uint32 key_code
uint8  key_pressed
```

The Android v0.2.0 UI emits mouse and vertical-scroll events only. Both peers bound coordinates; the server ignores unknown event types and invalid mouse actions/buttons. Keyboard injection exists in the server protocol implementation but has no Android user interface in this release.

## Public pairing metadata

`GET http://HOST:TLS_PORT+1/pair` returns a small JSON object:

```json
{
  "ip": "192.0.2.10",
  "tls_port": 48200,
  "fingerprint": "64 lowercase hex characters",
  "protocol_version": 2,
  "version": "0.2.0"
}
```

The response contains no PIN or session token and is marked `Cache-Control: no-store`. HTTP is unauthenticated and cleartext. The user must compare the fingerprint with the server console before trusting it. PINs and tokens are never sent over this HTTP endpoint.
