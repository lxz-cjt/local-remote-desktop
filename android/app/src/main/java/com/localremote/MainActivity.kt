package com.localremote

import android.annotation.SuppressLint
import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity

class MainActivity : AppCompatActivity() {
    private lateinit var remoteView: RemoteView
    private lateinit var etIp: EditText
    private lateinit var etPort: EditText
    private lateinit var etPin: EditText
    private lateinit var btnConnect: Button
    private lateinit var tvStatus: TextView
    private lateinit var controlPanel: View
    private lateinit var pairingStore: PairingStore

    private var mediaClient: MediaClient? = null

    @SuppressLint("ClickableViewAccessibility")
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        pairingStore = PairingStore(this)

        remoteView = findViewById(R.id.remoteView)
        etIp = findViewById(R.id.etIp)
        etPort = findViewById(R.id.etPort)
        etPin = findViewById(R.id.etPin)
        btnConnect = findViewById(R.id.btnConnect)
        tvStatus = findViewById(R.id.tvStatus)
        controlPanel = findViewById(R.id.controlPanel)

        remoteView.setOnClickListener {
            controlPanel.visibility = if (controlPanel.visibility == View.VISIBLE) View.GONE else View.VISIBLE
        }
        btnConnect.setOnClickListener {
            val ip = etIp.text.toString().trim()
            val port = etPort.text.toString().toIntOrNull()
            when {
                ip.isEmpty() -> tvStatus.text = getString(R.string.error_ip_required)
                port == null || port !in 1024..65534 -> tvStatus.text = getString(R.string.error_port)
                else -> connect(ip, port)
            }
        }
        findViewById<Button>(R.id.btnDisconnect).setOnClickListener { disconnect() }
        findViewById<Button>(R.id.btnRightClick).setOnClickListener { remoteView.sendRightClick() }
        findViewById<Button>(R.id.btnScrollUp).setOnClickListener { remoteView.sendScroll(1) }
        findViewById<Button>(R.id.btnScrollDown).setOnClickListener { remoteView.sendScroll(-1) }
    }

    private fun connect(ip: String, port: Int) {
        disconnect()
        tvStatus.text = getString(R.string.status_connecting)

        var client: MediaClient? = null
        client = MediaClient(
            ip,
            port,
            etPin.text.toString().trim(),
            pairingStore,
            remoteView.holder.surface,
            onStatus = { status ->
                runOnUiThread {
                    if (mediaClient === client) {
                        tvStatus.text = status
                        if (status.startsWith("Video ")) controlPanel.visibility = View.GONE
                    }
                }
            },
            onError = { error ->
                runOnUiThread {
                    if (mediaClient === client) {
                        tvStatus.text = getString(R.string.error_prefix, error)
                        controlPanel.visibility = View.VISIBLE
                    }
                }
            },
            onFingerprintRequired = { fingerprint, changed ->
                runOnUiThread {
                    if (mediaClient === client) {
                        showFingerprintPrompt(fingerprint, changed) { accepted ->
                            client?.resolveFingerprint(accepted)
                        }
                    }
                }
            }
        )
        mediaClient = client
        remoteView.setMediaClient(client)
        client?.start()
    }

    private fun showFingerprintPrompt(
        fingerprint: String,
        changed: Boolean,
        onDecision: (Boolean) -> Unit
    ) {
        val warning = if (changed) getString(R.string.fingerprint_changed) else ""
        val formatted = fingerprint.chunked(2).joinToString(":") { it.uppercase() }
        AlertDialog.Builder(this)
            .setTitle(R.string.fingerprint_title)
            .setMessage(getString(R.string.fingerprint_message, warning, formatted))
            .setPositiveButton(R.string.fingerprint_trust) { _, _ -> onDecision(true) }
            .setNegativeButton(R.string.fingerprint_reject) { _, _ -> onDecision(false) }
            .setOnCancelListener { onDecision(false) }
            .show()
    }

    private fun disconnect() {
        val client = mediaClient
        mediaClient = null
        remoteView.setMediaClient(null)
        client?.stop()
        tvStatus.text = getString(R.string.status_disconnected)
        controlPanel.visibility = View.VISIBLE
    }

    override fun onDestroy() {
        disconnect()
        super.onDestroy()
    }
}
