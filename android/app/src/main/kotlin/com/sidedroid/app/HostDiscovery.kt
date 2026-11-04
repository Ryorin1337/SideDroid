package com.sidedroid.app

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import android.util.Log
import java.util.ArrayDeque

data class FoundHost(val name: String, val address: String, val port: Int)

/** Watches the local network for PCs announcing _sidedroid._tcp. */
class HostDiscovery(context: Context, private val onChange: (List<FoundHost>) -> Unit) {
    private val nsd = context.getSystemService(Context.NSD_SERVICE) as NsdManager
    private val wifi = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private var multicast: WifiManager.MulticastLock? = null
    private val found = LinkedHashMap<String, FoundHost>()
    private var listening = false

    // NsdManager resolves only one service at a time on older Android versions.
    private val pending = ArrayDeque<NsdServiceInfo>()
    private var resolving = false

    private val listener = object : NsdManager.DiscoveryListener {
        override fun onDiscoveryStarted(serviceType: String) {}
        override fun onDiscoveryStopped(serviceType: String) {}
        override fun onStartDiscoveryFailed(serviceType: String, errorCode: Int) {
            Log.w(TAG, "discovery failed: $errorCode")
            listening = false
        }
        override fun onStopDiscoveryFailed(serviceType: String, errorCode: Int) {}
        override fun onServiceFound(info: NsdServiceInfo) {
            synchronized(pending) { pending.add(info) }
            resolveNext()
        }
        override fun onServiceLost(info: NsdServiceInfo) {
            synchronized(found) { found.remove(info.serviceName) }
            publish()
        }
    }

    @Suppress("DEPRECATION")
    private fun resolveNext() {
        val next = synchronized(pending) {
            if (resolving) return
            pending.poll()?.also { resolving = true }
        } ?: return
        nsd.resolveService(next, object : NsdManager.ResolveListener {
            override fun onResolveFailed(info: NsdServiceInfo, errorCode: Int) {
                synchronized(pending) { resolving = false }
                resolveNext()
            }
            override fun onServiceResolved(info: NsdServiceInfo) {
                val addr = info.host?.hostAddress
                if (addr != null) {
                    synchronized(found) { found[info.serviceName] = FoundHost(info.serviceName, addr, info.port) }
                    publish()
                }
                synchronized(pending) { resolving = false }
                resolveNext()
            }
        })
    }

    private fun publish() = onChange(synchronized(found) { found.values.toList() })

    fun start() {
        if (listening) return
        listening = true
        multicast = wifi.createMulticastLock("sidedroid").apply { setReferenceCounted(false); acquire() }
        nsd.discoverServices("_sidedroid._tcp", NsdManager.PROTOCOL_DNS_SD, listener)
    }

    fun stop() {
        if (!listening) return
        listening = false
        runCatching { nsd.stopServiceDiscovery(listener) }
        multicast?.release()
        multicast = null
    }

    private companion object {
        const val TAG = "SideDroid"
    }
}
