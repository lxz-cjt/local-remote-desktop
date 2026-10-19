package com.localremote

import android.content.Context
import java.security.MessageDigest

data class StoredPairing(val fingerprint: String, val token: String?)

class PairingStore(context: Context) {
    private val preferences = context.applicationContext.getSharedPreferences(
        "pairing_sessions",
        Context.MODE_PRIVATE
    )

    fun load(endpoint: String): StoredPairing? {
        val prefix = prefix(endpoint)
        val fingerprint = preferences.getString("${prefix}fingerprint", null) ?: return null
        return StoredPairing(fingerprint, preferences.getString("${prefix}token", null))
    }

    fun saveFingerprint(endpoint: String, fingerprint: String) {
        val prefix = prefix(endpoint)
        preferences.edit()
            .putString("${prefix}fingerprint", fingerprint)
            .remove("${prefix}token")
            .apply()
    }

    fun saveToken(endpoint: String, token: String) {
        preferences.edit().putString("${prefix(endpoint)}token", token).apply()
    }

    fun clearToken(endpoint: String) {
        preferences.edit().remove("${prefix(endpoint)}token").apply()
    }

    private fun prefix(endpoint: String): String {
        val digest = MessageDigest.getInstance("SHA-256").digest(endpoint.toByteArray(Charsets.UTF_8))
        return digest.joinToString("") { "%02x".format(it) } + ":"
    }
}
