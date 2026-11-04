package com.sidedroid.console

import java.io.File
import java.io.RandomAccessFile
import java.nio.channels.FileLock

/**
 * One console per user. The operating system releases the lock when the process
 * ends, however it ends, so a crash cannot stop the console from opening again.
 */
class SingleInstance(name: String) {
    private val dir = File(System.getenv("LOCALAPPDATA") ?: System.getProperty("java.io.tmpdir"), "SideDroid").apply { mkdirs() }
    private val lockFile = File(dir, "console-$name.lock")
    private val signalFile = File(dir, "console-$name.show")
    private var lock: FileLock? = null

    fun acquire(): Boolean {
        lock = runCatching { RandomAccessFile(lockFile, "rw").channel.tryLock() }.getOrNull()
        // A signal left from an earlier run must not pull a new window forward.
        if (lock != null) signalFile.delete()
        return lock != null
    }

    fun signalExisting() {
        runCatching { signalFile.createNewFile() }
    }

    fun takeSignal(): Boolean = signalFile.exists() && signalFile.delete()
}
