package com.localremote

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.os.Build
import android.view.Surface
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.InetSocketAddress
import java.net.URL
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.security.SecureRandom
import java.security.cert.CertificateException
import java.security.cert.X509Certificate
import java.util.ArrayDeque
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import javax.net.ssl.SSLContext
import javax.net.ssl.SSLSocket
import javax.net.ssl.TrustManager
import javax.net.ssl.X509TrustManager
import kotlin.concurrent.thread
import kotlin.math.max

private const val PROTOCOL_VERSION = 2
private const val TYPE_VIDEO = 0x01
private const val TYPE_AUDIO = 0x02
private const val TYPE_INPUT = 0x03
private const val TYPE_STATUS = 0x04
private const val TYPE_HEARTBEAT = 0x05
private const val TYPE_VIDEO_CONFIG = 0x06
private const val TYPE_AUTH_PAIR = 0x07
private const val TYPE_AUTH_SESSION = 0x08
private const val TYPE_AUTH_RESULT = 0x09
private const val TYPE_AUDIO_CONFIG = 0x0A
private const val TYPE_REQUEST_KEYFRAME = 0x0B
private const val MAX_VIDEO_PAYLOAD = 8 * 1024 * 1024
private const val MAX_AUDIO_PAYLOAD = 256 * 1024
private const val MAX_CONFIG_PAYLOAD = 64 * 1024
private const val TOKEN_LENGTH = 64

private data class PairingInfo(
    val tlsPort: Int,
    val fingerprint: String,
    val protocolVersion: Int
)

private data class VideoConfig(
    val width: Int,
    val height: Int,
    val sps: ByteArray,
    val pps: ByteArray
)

private data class AudioSpec(val sampleRate: Int, val channels: Int)
private data class OutgoingPacket(val type: Int, val payload: ByteArray)
private data class Packet(val type: Int, val payload: ByteArray)

class MediaClient(
    private val serverIp: String,
    private val tlsPort: Int,
    private val pin: String,
    private val pairingStore: PairingStore,
    surface: Surface?,
    private val onStatus: (String) -> Unit,
    private val onError: (String) -> Unit,
    private val onFingerprintRequired: (String, Boolean) -> Unit
) {
    @Volatile
    var isConnected = false
        private set

    @Volatile
    private var renderSurface: Surface? = surface

    @Volatile
    private var socket: SSLSocket? = null

    @Volatile
    private var outputStream: DataOutputStream? = null

    @Volatile
    private var videoConfig: VideoConfig? = null

    @Volatile
    private var audioSpec: AudioSpec? = null

    private val running = AtomicBoolean(false)
    private var ioThread: Thread? = null
    private var readThread: Thread? = null
    private var audioThread: Thread? = null

    private val decoderLock = Object()
    private val sendLock = Object()
    private val pendingLock = Object()
    private val pendingPackets = ArrayDeque<OutgoingPacket>()
    private val audioQueue = LinkedBlockingQueue<ByteArray>(8)

    private val fingerprintLock = Object()
    private var fingerprintDecision: Boolean? = null
    private var videoDecoder: MediaCodec? = null

    private val endpoint = "$serverIp:$tlsPort"

    fun setRenderSurface(surface: Surface?) {
        synchronized(decoderLock) {
            renderSurface = surface
            releaseDecoderLocked()
            if (surface != null && isConnected) {
                videoConfig?.let { initVideoDecoderLocked(it, surface) }
                enqueuePacket(TYPE_REQUEST_KEYFRAME, ByteArray(0))
            }
        }
    }

    fun start() {
        if (!running.compareAndSet(false, true)) return
        ioThread = thread(name = "MediaClient-IO") {
            try {
                runClient()
            } catch (error: Exception) {
                if (running.get()) onError(error.message ?: "Connection failed")
                isConnected = false
                closeSocket()
            }
        }
    }

    fun stop() {
        running.set(false)
        isConnected = false
        resolveFingerprint(false)
        closeSocket()
        synchronized(pendingLock) {
            pendingPackets.clear()
            pendingLock.notifyAll()
        }
        joinThread(ioThread)
        joinThread(readThread)
        joinThread(audioThread)
        synchronized(decoderLock) {
            releaseDecoderLocked()
        }
    }

    fun resolveFingerprint(accepted: Boolean) {
        synchronized(fingerprintLock) {
            fingerprintDecision = accepted
            fingerprintLock.notifyAll()
        }
    }

    fun sendInput(data: ByteArray) {
        enqueuePacket(TYPE_INPUT, data)
    }

    private fun runClient() {
        onStatus("Reading pairing metadata...")
        val pairing = fetchPairingInfo()
        require(pairing.protocolVersion == PROTOCOL_VERSION) { "Protocol version is not supported" }

        val stored = pairingStore.load(endpoint)
        var token = stored?.token
        if (stored?.fingerprint != pairing.fingerprint) {
            fingerprintDecision = null
            onFingerprintRequired(pairing.fingerprint, stored != null)
            if (!awaitFingerprintDecision()) throw IOException("Certificate fingerprint was not accepted")
            pairingStore.saveFingerprint(endpoint, pairing.fingerprint)
            token = null
        }

        onStatus("Connecting with pinned TLS...")
        connectTls(pairing.tlsPort, pairing.fingerprint)
        val input = DataInputStream(socket!!.getInputStream())
        val authenticatedToken = authenticate(input, token)
        pairingStore.saveToken(endpoint, authenticatedToken)

        isConnected = true
        onStatus("Connected (${socket!!.session.protocol}); waiting for media configuration")
        synchronized(pendingLock) {
            pendingPackets.clear()
        }
        audioQueue.clear()

        readThread = thread(name = "MediaClient-Read") {
            readLoop(input)
        }
        audioThread = thread(name = "MediaClient-Audio") {
            audioLoop()
        }
        writerLoop()
    }

    private fun fetchPairingInfo(): PairingInfo {
        val connection = URL("http://$serverIp:${tlsPort + 1}/pair").openConnection() as HttpURLConnection
        connection.connectTimeout = 5000
        connection.readTimeout = 5000
        connection.useCaches = false
        return try {
            if (connection.responseCode != HttpURLConnection.HTTP_OK) {
                throw IOException("Pairing metadata request failed (${connection.responseCode})")
            }
            val body = String(readLimited(connection.inputStream, 4096), Charsets.UTF_8)
            val json = JSONObject(body)
            val fingerprint = json.getString("fingerprint").lowercase()
            require(Regex("[0-9a-f]{64}").matches(fingerprint)) { "Invalid certificate fingerprint" }
            PairingInfo(
                json.getInt("tls_port"),
                fingerprint,
                json.getInt("protocol_version")
            )
        } finally {
            connection.disconnect()
        }
    }

    private fun awaitFingerprintDecision(): Boolean {
        val deadline = System.currentTimeMillis() + 120_000
        synchronized(fingerprintLock) {
            while (running.get() && fingerprintDecision == null) {
                val remaining = deadline - System.currentTimeMillis()
                if (remaining <= 0) return false
                fingerprintLock.wait(remaining)
            }
            return running.get() && fingerprintDecision == true
        }
    }

    private fun connectTls(port: Int, fingerprint: String) {
        val context = SSLContext.getInstance("TLS")
        context.init(null, arrayOf<TrustManager>(PinnedTrustManager(fingerprint)), SecureRandom())
        val newSocket = context.socketFactory.createSocket() as SSLSocket
        socket = newSocket
        val protocols = newSocket.supportedProtocols.filter {
            it == "TLSv1.3" || it == "TLSv1.2"
        }.toTypedArray()
        if (protocols.isEmpty()) throw IOException("TLS 1.2 or newer is unavailable")
        newSocket.enabledProtocols = protocols
        newSocket.connect(InetSocketAddress(serverIp, port), 5000)
        newSocket.soTimeout = 15_000
        newSocket.startHandshake()
        outputStream = DataOutputStream(newSocket.getOutputStream())
    }

    private fun authenticate(input: DataInputStream, savedToken: String?): String {
        if (!savedToken.isNullOrBlank() && Regex("[0-9a-f]{64}").matches(savedToken)) {
            val payload = ByteBuffer.allocate(4 + TOKEN_LENGTH).order(ByteOrder.LITTLE_ENDIAN)
                .putInt(PROTOCOL_VERSION)
                .put(savedToken.toByteArray(Charsets.US_ASCII))
                .array()
            sendPacketNow(TYPE_AUTH_SESSION, payload)
            val result = readPacket(input, true)
            when (result.payload[0].toInt()) {
                0 -> return tokenFromResult(result.payload)
                3 -> pairingStore.clearToken(endpoint)
                else -> throw IOException(authError(result.payload[0].toInt()))
            }
        }

        require(Regex("[0-9]{6}").matches(pin)) { "Enter the current six-digit PIN" }
        val pinBytes = pin.toByteArray(Charsets.US_ASCII)
        val payload = ByteBuffer.allocate(12).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(PROTOCOL_VERSION)
            .put(pinBytes)
            .put(ByteArray(8 - pinBytes.size))
            .array()
        sendPacketNow(TYPE_AUTH_PAIR, payload)
        val result = readPacket(input, true)
        if (result.payload[0].toInt() != 0) {
            throw IOException(authError(result.payload[0].toInt()))
        }
        return tokenFromResult(result.payload)
    }

    private fun tokenFromResult(payload: ByteArray): String {
        val token = String(payload, 1, TOKEN_LENGTH, Charsets.US_ASCII)
        if (!Regex("[0-9a-f]{64}").matches(token)) throw IOException("Invalid session token response")
        return token
    }

    private fun writerLoop() {
        var nextHeartbeat = 0L
        while (running.get() && isConnected) {
            val packet = synchronized(pendingLock) {
                if (pendingPackets.isEmpty()) pendingLock.wait(250)
                if (pendingPackets.isEmpty()) null else pendingPackets.removeFirst()
            }
            if (packet != null) sendPacketNow(packet.type, packet.payload)
            val now = System.currentTimeMillis()
            if (now >= nextHeartbeat) {
                sendPacketNow(TYPE_HEARTBEAT, ByteArray(0))
                nextHeartbeat = now + 2000
            }
        }
    }

    private fun enqueuePacket(type: Int, payload: ByteArray) {
        if (!running.get() || !isConnected) return
        synchronized(pendingLock) {
            if (pendingPackets.size >= 128) pendingPackets.removeFirst()
            pendingPackets.addLast(OutgoingPacket(type, payload))
            pendingLock.notifyAll()
        }
    }

    private fun readLoop(input: DataInputStream) {
        try {
            while (running.get() && isConnected) {
                val packet = readPacket(input, false)
                when (packet.type) {
                    TYPE_VIDEO -> handleVideo(packet.payload)
                    TYPE_AUDIO -> handleAudio(packet.payload)
                    TYPE_STATUS -> handleStatus(packet.payload)
                    TYPE_VIDEO_CONFIG -> handleVideoConfig(packet.payload)
                    TYPE_AUDIO_CONFIG -> handleAudioConfig(packet.payload)
                    TYPE_HEARTBEAT -> Unit
                }
            }
        } catch (error: Exception) {
            if (running.get() && isConnected) onError("Connection lost: ${error.message ?: "read error"}")
            isConnected = false
            closeSocket()
        }
    }

    private fun readPacket(input: DataInputStream, authenticating: Boolean): Packet {
        val header = ByteArray(5)
        input.readFully(header)
        val length = ByteBuffer.wrap(header, 0, 4).order(ByteOrder.LITTLE_ENDIAN).int
        val type = header[4].toInt() and 0xFF
        if (!validServerPacket(type, length, authenticating)) {
            throw IOException("Invalid protocol packet (type=$type, length=$length)")
        }
        val payload = ByteArray(length)
        input.readFully(payload)
        return Packet(type, payload)
    }

    private fun validServerPacket(type: Int, length: Int, authenticating: Boolean): Boolean {
        if (authenticating) return type == TYPE_AUTH_RESULT && length == 1 + TOKEN_LENGTH
        return when (type) {
            TYPE_VIDEO -> length in 1..MAX_VIDEO_PAYLOAD
            TYPE_AUDIO -> length >= 0 && length <= MAX_AUDIO_PAYLOAD && length % 2 == 0
            TYPE_STATUS -> length == 9
            TYPE_HEARTBEAT -> length == 0
            TYPE_VIDEO_CONFIG -> length in 12..MAX_CONFIG_PAYLOAD
            TYPE_AUDIO_CONFIG -> length == 8
            else -> false
        }
    }

    private fun handleVideoConfig(payload: ByteArray) {
        val buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val width = buffer.int
        val height = buffer.int
        val spsLength = buffer.short.toInt() and 0xFFFF
        val ppsLength = buffer.short.toInt() and 0xFFFF
        require(width in 2..8192 && height in 2..8192) { "Invalid video dimensions" }
        require(spsLength > 0 && ppsLength > 0 && 12 + spsLength + ppsLength == payload.size) {
            "Invalid H.264 configuration"
        }
        val sps = payload.copyOfRange(12, 12 + spsLength)
        val pps = payload.copyOfRange(12 + spsLength, payload.size)
        val config = VideoConfig(width, height, sps, pps)
        videoConfig = config
        synchronized(decoderLock) {
            renderSurface?.let { surface ->
                releaseDecoderLocked()
                initVideoDecoderLocked(config, surface)
            }
        }
        onStatus("Video ${width}x$height; authenticated session active")
    }

    private fun initVideoDecoderLocked(config: VideoConfig, surface: Surface) {
        val format = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, config.width, config.height)
        format.setByteBuffer("csd-0", codecNal(config.sps))
        format.setByteBuffer("csd-1", codecNal(config.pps))
        format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, MAX_VIDEO_PAYLOAD)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            format.setInteger(MediaFormat.KEY_PRIORITY, 0)
        }
        videoDecoder = MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_VIDEO_AVC).also {
            it.configure(format, surface, null, 0)
            it.start()
        }
    }

    private fun codecNal(nal: ByteArray): ByteBuffer {
        val buffer = ByteBuffer.allocate(nal.size + 4)
        buffer.put(byteArrayOf(0, 0, 0, 1))
        buffer.put(nal)
        buffer.flip()
        return buffer
    }

    private fun handleVideo(payload: ByteArray) {
        synchronized(decoderLock) {
            val decoder = videoDecoder ?: return
            val inputId = decoder.dequeueInputBuffer(2000)
            if (inputId >= 0) {
                val buffer = decoder.getInputBuffer(inputId) ?: return
                buffer.clear()
                if (payload.size > buffer.remaining()) {
                    enqueuePacket(TYPE_REQUEST_KEYFRAME, ByteArray(0))
                    return
                }
                buffer.put(payload)
                decoder.queueInputBuffer(inputId, 0, payload.size, 0, 0)
            }
            val info = MediaCodec.BufferInfo()
            while (true) {
                val outputId = decoder.dequeueOutputBuffer(info, 0)
                if (outputId < 0) break
                decoder.releaseOutputBuffer(outputId, true)
            }
        }
    }

    private fun handleAudioConfig(payload: ByteArray) {
        val buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val sampleRate = buffer.int
        val channels = buffer.short.toInt() and 0xFFFF
        val bits = buffer.short.toInt() and 0xFFFF
        require(sampleRate in 8000..192000 && channels == 2 && bits == 16) {
            "Unsupported audio configuration"
        }
        audioSpec = AudioSpec(sampleRate, channels)
        audioQueue.clear()
    }

    private fun handleAudio(payload: ByteArray) {
        if (payload.isEmpty()) return
        if (!audioQueue.offer(payload)) {
            audioQueue.poll()
            audioQueue.offer(payload)
        }
    }

    private fun audioLoop() {
        var track: AudioTrack? = null
        var activeSpec: AudioSpec? = null
        try {
            while (running.get() && isConnected) {
                val spec = audioSpec
                if (spec == null) {
                    Thread.sleep(25)
                    continue
                }
                if (track == null || activeSpec != spec) {
                    track?.let {
                        runCatching { it.stop() }
                        it.release()
                    }
                    activeSpec = spec
                    track = createAudioTrack(spec).also { it.play() }
                }
                val payload = audioQueue.poll(100, TimeUnit.MILLISECONDS) ?: continue
                track?.write(payload, 0, payload.size)
            }
        } catch (error: Exception) {
            if (running.get() && isConnected) onError("Audio playback stopped: ${error.message ?: "audio error"}")
        } finally {
            track?.let {
                runCatching { it.stop() }
                it.release()
            }
        }
    }

    private fun createAudioTrack(spec: AudioSpec): AudioTrack {
        val minimum = AudioTrack.getMinBufferSize(
            spec.sampleRate,
            AudioFormat.CHANNEL_OUT_STEREO,
            AudioFormat.ENCODING_PCM_16BIT
        )
        require(minimum > 0) { "AudioTrack does not support ${spec.sampleRate} Hz" }
        return AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_UNKNOWN)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(spec.sampleRate)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .build()
            )
            .setBufferSizeInBytes(max(minimum * 4, spec.sampleRate * 4 / 5))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
    }

    private fun handleStatus(payload: ByteArray) {
        val buffer = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN)
        val width = buffer.int
        val height = buffer.int
        val paused = buffer.get().toInt() != 0
        onStatus("Desktop ${width}x$height${if (paused) " (paused)" else ""}")
    }

    private fun sendPacketNow(type: Int, payload: ByteArray) {
        synchronized(sendLock) {
            val output = outputStream ?: throw IOException("TLS output is unavailable")
            val header = ByteBuffer.allocate(5).order(ByteOrder.LITTLE_ENDIAN)
            header.putInt(payload.size)
            header.put(type.toByte())
            output.write(header.array())
            output.write(payload)
            output.flush()
        }
    }

    private fun closeSocket() {
        val current = socket
        socket = null
        outputStream = null
        try {
            current?.close()
        } catch (_: IOException) {
        }
    }

    private fun releaseDecoderLocked() {
        videoDecoder?.let {
            runCatching { it.stop() }
            it.release()
        }
        videoDecoder = null
    }

    private fun joinThread(thread: Thread?) {
        if (thread != null && thread !== Thread.currentThread()) {
            runCatching { thread.join(1500) }
        }
    }

    private fun readLimited(input: java.io.InputStream, limit: Int): ByteArray {
        val output = ByteArrayOutputStream()
        val buffer = ByteArray(1024)
        while (true) {
            val count = input.read(buffer)
            if (count < 0) break
            if (output.size() + count > limit) throw IOException("Pairing response is too large")
            output.write(buffer, 0, count)
        }
        return output.toByteArray()
    }

    private fun authError(status: Int): String = when (status) {
        1 -> "Incorrect PIN"
        2 -> "Too many PIN failures; retry after the server lockout"
        3 -> "Saved session expired; enter the current PIN"
        4 -> "Protocol version is not supported"
        else -> "Authentication failed"
    }

    private class PinnedTrustManager(private val fingerprint: String) : X509TrustManager {
        override fun checkServerTrusted(chain: Array<X509Certificate>?, authType: String?) {
            if (chain.isNullOrEmpty()) throw CertificateException("Server certificate is missing")
            val certificate = chain[0]
            certificate.checkValidity()
            val actual = MessageDigest.getInstance("SHA-256")
                .digest(certificate.encoded)
                .joinToString("") { "%02x".format(it) }
            if (!MessageDigest.isEqual(actual.toByteArray(Charsets.US_ASCII), fingerprint.toByteArray(Charsets.US_ASCII))) {
                throw CertificateException("Pinned certificate fingerprint does not match")
            }
        }

        override fun checkClientTrusted(chain: Array<X509Certificate>?, authType: String?) {
            throw CertificateException("Client certificates are not supported")
        }

        override fun getAcceptedIssuers(): Array<X509Certificate> = emptyArray()
    }
}
