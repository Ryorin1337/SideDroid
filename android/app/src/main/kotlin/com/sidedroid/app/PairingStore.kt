package com.sidedroid.app

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** This device's id and per-PC pairing secrets, encrypted with a key in the Android Keystore. */
class PairingStore(context: Context) {
    private val prefs = context.getSharedPreferences("pairing", Context.MODE_PRIVATE)

    val clientId: ByteArray
        get() {
            prefs.getString("client_id", null)?.let { return Base64.decode(it, Base64.NO_WRAP) }
            val id = ByteArray(16).also { SecureRandom().nextBytes(it) }
            prefs.edit().putString("client_id", Base64.encodeToString(id, Base64.NO_WRAP)).apply()
            return id
        }

    fun secretFor(host: String): ByteArray? {
        val stored = prefs.getString(key(host), null) ?: return null
        return runCatching {
            val all = Base64.decode(stored, Base64.NO_WRAP)
            val c = Cipher.getInstance(TRANSFORM)
            c.init(Cipher.DECRYPT_MODE, keystoreKey(), GCMParameterSpec(128, all, 0, 12))
            c.doFinal(all, 12, all.size - 12)
        }.getOrNull()
    }

    fun save(host: String, secret: ByteArray) {
        val c = Cipher.getInstance(TRANSFORM)
        c.init(Cipher.ENCRYPT_MODE, keystoreKey())
        val all = c.iv + c.doFinal(secret)
        prefs.edit().putString(key(host), Base64.encodeToString(all, Base64.NO_WRAP)).apply()
    }

    fun forget(host: String) {
        prefs.edit().remove(key(host)).apply()
    }

    private fun key(host: String) = "secret_$host"

    private fun keystoreKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        gen.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .build(),
        )
        return gen.generateKey()
    }

    private companion object {
        const val ALIAS = "sidedroid_pairing"
        const val TRANSFORM = "AES/GCM/NoPadding"
    }
}
