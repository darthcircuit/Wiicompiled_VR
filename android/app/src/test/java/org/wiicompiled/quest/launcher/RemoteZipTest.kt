package org.wiicompiled.quest.launcher

import java.io.ByteArrayOutputStream
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/** The NDK download reads only the wanted entries of a zip, through range requests. */
class RemoteZipTest {

    private val contents = linkedMapOf(
        "ndk/sysroot/usr/include/stdio.h" to "int printf(const char *, ...);\n".repeat(200).toByteArray(),
        "ndk/sysroot/usr/lib/aarch64-linux-android/29/libc.so" to ByteArray(70_000) { (it * 31).toByte() },
        "ndk/prebuilt/bin/clang" to ByteArray(300_000) { (it * 7).toByte() },
        "ndk/lib/clang/21/lib/linux/libclang_rt.builtins-aarch64-android.a" to "!<arch>\n".toByteArray(),
    )

    private val zip: ByteArray = ByteArrayOutputStream().also { bytes ->
        ZipOutputStream(bytes).use { out ->
            for ((name, data) in contents) {
                val entry = ZipEntry(name)
                if (name.endsWith(".a")) {
                    // A stored entry: sizes and CRC must be known up front.
                    entry.method = ZipEntry.STORED
                    entry.size = data.size.toLong()
                    entry.crc = CRC32().apply { update(data) }.value
                }
                out.putNextEntry(entry)
                out.write(data)
                out.closeEntry()
            }
            out.putNextEntry(ZipEntry("ndk/empty-dir/"))
            out.closeEntry()
        }
    }.toByteArray()

    @Test
    fun readsWantedEntriesWithoutTheRest() {
        val requests = mutableListOf<LongRange>()
        val remote = RemoteZip(zip.size.toLong()) { start, end ->
            requests += start..end
            zip.copyOfRange(start.toInt(), end.toInt() + 1)
        }
        val entries = remote.entries()
        assertEquals(contents.keys + "ndk/empty-dir/", entries.map { it.name }.toSet())

        val wanted = entries.filter { it.name.contains("sysroot") || it.name.endsWith(".a") }
        val read = LinkedHashMap<String, ByteArray>()
        requests.clear()
        remote.read(wanted, maxGap = 64) { entry, bytes -> read[entry.name] = bytes }

        assertEquals(wanted.map { it.name }.toSet(), read.keys)
        for ((name, bytes) in read) assertArrayEquals(name, contents.getValue(name), bytes)
        // The two sysroot entries sit next to each other and share a request; clang, 300 KB, is skipped.
        assertEquals(2, requests.size)
        assertTrue(requests.sumOf { it.last - it.first + 1 } < 150_000)
    }

    @Test
    fun keepsRequestsWithinTheSizeLimit() {
        val requests = mutableListOf<LongRange>()
        val remote = RemoteZip(zip.size.toLong()) { start, end ->
            requests += start..end
            zip.copyOfRange(start.toInt(), end.toInt() + 1)
        }
        val files = remote.entries().filter { !it.name.endsWith("/") }
        requests.clear()
        var count = 0
        remote.read(files, maxGap = 1L shl 20, maxRequest = 1) { _, _ -> count++ }
        assertEquals(files.size, count)
        assertEquals(files.size, requests.size)
    }
}
