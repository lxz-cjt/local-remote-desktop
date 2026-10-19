# Third-party notices

The project source is MIT-licensed. Dependencies and generated or distributed binaries can have additional obligations. This file is a summary, not a replacement for the upstream license texts.

| Component | Use | License / notice |
| --- | --- | --- |
| FFmpeg (`avcodec`, `avutil`, `swscale`) | H.264 encoding and pixel conversion | LGPL-2.1-or-later in a default configuration; GPL components change the combined terms. See https://ffmpeg.org/legal.html and the exact build configuration. |
| x264 / libx264 | H.264 encoder selected by the vcpkg manifest | GPL-2.0-or-later. See https://www.videolan.org/developers/x264.html. |
| OpenSSL | TLS, random values, certificates and fingerprints | Apache-2.0 (OpenSSL 3.x and later). See https://www.openssl.org/source/license.html. |
| AndroidX, Material Components and ConstraintLayout | Android UI and app compatibility | Apache-2.0. See each artifact's POM and source notices. |
| Kotlin Android plugin and standard library | Android source compilation and runtime | Apache-2.0. See https://github.com/JetBrains/kotlin/blob/main/license/LICENSE.txt. |
| Android Gradle Plugin | Android build | Apache-2.0 and accompanying Google notices. |
| Gradle and Gradle Wrapper | Build bootstrap and execution | Apache-2.0. See https://github.com/gradle/gradle/blob/master/LICENSE. |

## Important binary-distribution note

`server/vcpkg.json` intentionally enables x264 so the server has a functional H.264 encoder. A server binary linked with that configuration, and the corresponding FFmpeg build, must be distributed in compliance with all applicable GPL and FFmpeg license obligations. The MIT license on this repository's own source does not remove those obligations. Review the exact vcpkg feature and linkage configuration before publishing binaries.

CI-produced APKs also include dependencies subject to their own notices. Debug APKs are build artifacts for evaluation, not signed release distributions.
