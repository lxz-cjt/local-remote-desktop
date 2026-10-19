# Changelog

All notable changes are documented here. The format follows Keep a Changelog principles and versions follow Semantic Versioning where practical.

## [0.2.0] - 2026-10-19

### Added

- Per-start six-digit PIN pairing, per-address failure lockout, and in-memory session tokens.
- Explicit certificate-fingerprint confirmation and SHA-256 pinning on Android.
- Authentication-gated media capture and remote input.
- Protocol version 2 with bounded packet lengths, media configuration messages, and keyframe requests.
- Real desktop dimensions, H.264 SPS/PPS configuration, and negotiated WASAPI sample-rate reporting.
- Right-click and vertical-scroll controls on Android.
- CMake presets, a vcpkg manifest, official Gradle Wrapper files, and Windows/Android GitHub Actions.
- Security, architecture, protocol, third-party-license, and changelog documentation.

### Changed

- TLS now accepts TLS 1.2 or newer instead of requiring TLS 1.3 on every device.
- Certificates are generated through OpenSSL APIs in an ignored runtime directory.
- Float32 and PCM16 WASAPI loopback input is converted to PCM16 stereo without forcing an unsupported mix format.
- Android network writes and audio playback run on dedicated threads with bounded queues.
- Android application version is now 0.2.0 (versionCode 2).

### Fixed

- Ctrl+C and Ctrl+Break now stop the Windows server and release media/network resources.
- Partial network reads/writes, socket timeouts, and concurrent SSL writes are handled explicitly.
- Video decoding no longer assumes 1920x1080.
- Disconnect closes the TLS socket to unblock Android I/O threads.
- Touch coordinates are bounded and coordinate logging was removed.

### Security

- Removed the Android TrustAll certificate manager.
- Unauthenticated TLS connections cannot receive media or inject input.
- PINs and session tokens are sent only inside a fingerprint-pinned TLS connection; HTTP exposes pairing metadata only.

[0.2.0]: https://github.com/lxz-cjt/local-remote-desktop/releases/tag/v0.2.0
