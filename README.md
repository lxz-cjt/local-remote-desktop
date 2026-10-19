# Local Remote Desktop

[![Windows Server](https://github.com/lxz-cjt/local-remote-desktop/actions/workflows/windows.yml/badge.svg)](https://github.com/lxz-cjt/local-remote-desktop/actions/workflows/windows.yml)
[![Android](https://github.com/lxz-cjt/local-remote-desktop/actions/workflows/android.yml/badge.svg)](https://github.com/lxz-cjt/local-remote-desktop/actions/workflows/android.yml)

A trusted-LAN remote desktop security MVP: a C++17 Windows server and a Kotlin Android client. Version **0.2.0** focuses on authenticated pairing, certificate pinning, bounded protocol I/O, and reproducible builds.

## What works in this version

- On-demand DXGI desktop capture and libx264 H.264 encoding after an authenticated client connects.
- Actual capture dimensions and H.264 SPS/PPS sent to Android for `MediaCodec` configuration.
- WASAPI loopback using the system mix format, converted to PCM16 stereo at the original sample rate.
- Six-digit PIN pairing with failure lockout and in-memory session tokens.
- Explicit first-use fingerprint confirmation followed by SHA-256 certificate pinning; no TrustAll manager.
- TLS 1.2 or newer for authentication, media, heartbeats, and input.
- Mouse movement, left/right click, and vertical scroll from Android.
- Graceful Windows shutdown with Ctrl+C or Ctrl+Break.

No benchmarked latency or frame-rate claim is made. Performance depends on the desktop, encoder, decoder, and network.

## Security model and limits

This project is for a **trusted IPv4 LAN only**. Do not expose the server to the internet or configure port forwarding.

- `GET /pair` uses cleartext HTTP and returns only public metadata. Always compare the displayed SHA-256 fingerprint with the Windows console before tapping **Trust and connect**.
- PINs and session tokens are sent only inside the fingerprint-pinned TLS connection.
- The PIN changes on every server start. Five failures from one address cause a 60-second lockout. This is rate limiting, not enterprise-grade authentication.
- The self-signed RSA-2048/SHA-256 certificate is generated through OpenSSL in `runtime/` beside the executable and is valid for 30 days. The unencrypted private key must never be published.
- Session tokens are stored in Android's private app preferences and in server memory. They are invalidated by a server restart.
- Android 8.0 devices without TLS 1.3 use TLS 1.2; TLS 1.3 is not promised on every supported API level.

Read [SECURITY.md](SECURITY.md) and [docs/PROTOCOL.md](docs/PROTOCOL.md) before deploying or changing the trust model.

## Repository layout

```text
server/                 Windows server, CMake presets and vcpkg manifest
  src/                  Capture, encoding, audio, TLS protocol and input injection
android/                Android application and official Gradle Wrapper
.github/workflows/      Windows Release build and Android lint/APK CI
docs/                   Architecture and wire protocol
```

The legacy `python-server/` prototype may exist in a developer's working copy, but it is ignored and is not supported or included in the public source set.

## Build the Windows server

### Requirements

- Windows 10/11 x64 with a DirectX 11 capable primary display.
- CMake 3.21 or newer and Visual Studio 2022 with the C++ desktop workload for the provided presets.
- vcpkg, or separate FFmpeg and OpenSSL installations. FFmpeg must include `avcodec`, `avutil`, `swscale`, and the **libx264** encoder.

### Option A: vcpkg manifest

Set `VCPKG_ROOT` to a vcpkg checkout whose `vcpkg.exe` has been bootstrapped, then run:

```powershell
cd server
cmake --preset windows-vcpkg
cmake --build --preset vcpkg-release
```

The executable is written to `server/.build/vcpkg/Release/LocalRemoteServer.exe`. The manifest build downloads and compiles dependencies on first use. The CI workflow pins the vcpkg checkout used for its reproducible build.

### Option B: existing local FFmpeg and OpenSSL

Set environment variables to dependency prefixes; no personal absolute path is hard-coded in CMake:

```powershell
$env:FFMPEG_ROOT = "C:\tools\ffmpeg"
$env:OPENSSL_ROOT_DIR = "C:\tools\OpenSSL-Win64"
cd server
cmake --preset windows-local
cmake --build --preset local-release
```

Each prefix must contain `include` and `lib`; a `bin` directory is used to copy runtime DLLs. With a newer Visual Studio installation, configure with its matching CMake generator instead of the VS2022 preset.

### Run

```powershell
.\LocalRemoteServer.exe
# Optional: TLS port 49000 and metadata port 49001
.\LocalRemoteServer.exe 49000
```

Leave the console open. It shows the current PIN, LAN metadata URL, and certificate fingerprint. Runtime certificates are created beside the executable, not in the source tree.

## Build the Android client

Requirements: JDK 17 and Android SDK with API 34. Android Studio is optional.

```bash
cd android
bash ./gradlew lint assembleDebug
```

On Windows, `gradlew.bat lint assembleDebug` is equivalent. The debug APK is generated at `android/app/build/outputs/apk/debug/app-debug.apk`. The Wrapper pins Gradle 8.5 and verifies the distribution SHA-256.

GitHub Actions performs the same lint and `assembleDebug` tasks and uploads the debug APK as an artifact. A debug artifact is not a signed release build.

## Pair and use

1. Connect the computer and Android device to the same trusted LAN and allow the selected ports through the host firewall for that private network.
2. Start the Windows server and note its IPv4 address, current six-digit PIN, and fingerprint.
3. In the app, enter the IPv4 address, TLS port (default `48200`), and PIN. Tap **Connect**.
4. On first use, compare the complete fingerprint with the server console, then tap **Trust and connect**. Reject it if anything differs.
5. Later connections can reuse the saved session token while that server process remains running. After a restart, enter the new PIN.
6. Use touch for mouse movement/left click and the control buttons for right click and scrolling. Tap **Disconnect** to close the socket and stop server capture.

A changed certificate produces a warning and requires a new explicit trust decision. Do not accept a changed fingerprint unless you intentionally regenerated the server identity.

## Known limitations and roadmap

- One client connection at a time and the primary DXGI output only.
- No Android keyboard UI, multi-monitor selection, discovery service, account system, NAT traversal, or internet-safe deployment mode.
- Raw PCM audio has no jitter buffer or compression; multichannel sources use their first two channels. An unresponsive audio driver disables audio without blocking video, but that driver path still requires investigation on the affected machine.
- H.264 YUV420 encoding requires even desktop dimensions.
- No hardware video encoder, adaptive bitrate, or advanced gesture system.
- MediaCodec behavior and audio latency vary by device. Compilation and lint do not replace physical-device testing.

Planned work includes an Android keyboard UI, multi-output selection, compressed audio with jitter buffering, hardware encoding, and broader device validation. These are not presented as completed features.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Protocol version 2](docs/PROTOCOL.md)
- [Security policy](SECURITY.md)
- [Changelog](CHANGELOG.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## License

The project's own source is available under the [MIT License](LICENSE). Dependencies have their own licenses. In particular, the provided vcpkg configuration enables x264; combined server binaries must also comply with applicable GPL and FFmpeg obligations. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) before redistributing binaries.
