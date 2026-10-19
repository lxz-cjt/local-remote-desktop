# Architecture

## Processes

The public project contains two implementations that are designed to work together:

- `server/`: a Windows C++17 executable using DXGI Desktop Duplication, FFmpeg/libx264, WASAPI and OpenSSL.
- `android/`: an Android 8.0+ Kotlin client using `SSLSocket`, `MediaCodec`, `AudioTrack` and a `SurfaceView`.

`python-server/` is a local legacy prototype. It is ignored and is not part of the supported protocol or public source set.

## Server lifecycle

`NetworkServer` owns one TLS listener and one HTTP metadata listener. The TLS accept loop handles one socket at a time. Before authentication, its read loop accepts only `AUTH_PAIR` or `AUTH_SESSION`. A successful result enables the authenticated flag; all media sends check that flag.

The main thread observes that flag and lazily creates the desktop capture and encoder. It sends video configuration before encoded media and destroys those objects on disconnect. `AudioManager` initializes and runs WASAPI on a separate worker so an unavailable or unresponsive audio driver does not block video. Normal audio stops on disconnect; an unresponsive driver is abandoned without repeated retries, with shared ownership preserving objects until the worker exits or the process terminates.

There is no video frame queue between capture and the network. Socket send timeouts and synchronous writes bound slow-client behavior; new desktop frames are skipped while a send is in progress rather than accumulating in an application queue. Audio uses a bounded sample buffer and the Android side has a separate bounded playback queue.

A separate HTTP thread serves only `GET /pair`. It returns the TLS port, protocol version and public certificate fingerprint. It does not issue tokens and has no CORS grant.

OpenSSL generates a per-start PIN and session tokens. The certificate and private key are generated in `runtime/` beside the executable and reused while valid. The certificate fingerprint is printed for out-of-band comparison.

## Android threading

- The IO thread fetches public metadata, establishes pinned TLS, performs authentication and writes queued packets/heartbeats.
- The read thread validates packet boundaries and routes media/configuration messages.
- Video decode uses `MediaCodec`; no fixed display resolution is assumed.
- The audio thread owns `AudioTrack` and consumes a bounded PCM queue. If full, the oldest audio chunk is discarded.
- Touch callbacks only create bounded input events and enqueue them. They never perform socket writes on the UI thread.

Closing the socket on disconnect unblocks network reads and writes. The video decoder is recreated when the rendering surface or codec configuration changes, and a keyframe is requested for a recreated surface.

## Trust and storage

The first pairing requires the user to compare and accept the certificate's SHA-256 fingerprint. Android stores that fingerprint and the current session token in private, non-backed-up app preferences keyed by endpoint. A changed fingerprint requires another explicit decision and clears the old token. Server tokens last only for that server process; a later connection can pair again with the current PIN.

The design is intentionally limited to trusted LANs. The public HTTP metadata request can be intercepted, so skipping the console fingerprint comparison defeats the intended first-use identity check.
