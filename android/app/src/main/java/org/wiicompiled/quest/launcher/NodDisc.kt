package org.wiicompiled.quest.launcher

import java.io.IOException
import java.io.InterruptedIOException

/**
 * nod, the disc image library the PC installer runs as nodtool, through
 * android/nod-jni. Every call takes the file descriptor of an open disc image
 * (ISO, WBFS, RVZ, WIA, CISO, GCZ) and reads the Wii data partition from it.
 * All three block, so callers run them off the main thread.
 */
object NodDisc {

    init {
        System.loadLibrary("nod_jni")
    }

    fun interface Listener {
        /** Bytes written so far out of [total]; return false to cancel. */
        fun update(done: Long, total: Long): Boolean
    }

    /** "<game id>\n<title>\n<bytes an extraction writes>", see [DiscChecks.parseHeader]. */
    @Throws(IOException::class)
    external fun header(fd: Int): String

    /** `sys/main.dol` or a path below `files/` of the data partition. */
    @Throws(IOException::class)
    external fun readFile(fd: Int, path: String): ByteArray

    /**
     * Writes the data partition to [outDir] in nodtool extract's layout.
     * Throws [InterruptedIOException] when [listener] cancels.
     */
    @Throws(IOException::class)
    external fun extract(fd: Int, outDir: String, listener: Listener)
}
