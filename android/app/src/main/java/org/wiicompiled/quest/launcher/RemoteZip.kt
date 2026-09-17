package org.wiicompiled.quest.launcher

import java.io.ByteArrayOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.Inflater

/**
 * Reads a few entries of a large zip on a web server without downloading the rest: the central
 * directory from the end of the file, then only the wanted entries, neighbouring ones in one
 * request. [fetch] returns the bytes from start to endInclusive (an HTTP range request).
 *
 * Only what the Android NDK zip needs: no ZIP64, stored or deflated entries.
 */
class RemoteZip(private val size: Long, private val fetch: (start: Long, endInclusive: Long) -> ByteArray) {

    /** [end] is where the next entry (or the central directory) starts, which bounds this one's bytes. */
    data class Entry(val name: String, val method: Int, val compressedSize: Long, val size: Long, val offset: Long, val end: Long)

    fun entries(): List<Entry> {
        val tailStart = maxOf(0L, size - EOCD_SEARCH)
        val tail = fetch(tailStart, size - 1)
        var eocd = -1
        for (i in tail.size - EOCD_SIZE downTo 0) {
            if (u32(tail, i) == EOCD_SIGNATURE) { eocd = i; break }
        }
        if (eocd < 0) throw IOException("No end of central directory record")
        val count = u16(tail, eocd + 10)
        val directorySize = u32(tail, eocd + 12)
        val directoryOffset = u32(tail, eocd + 16)
        if (count == 0xFFFF || directoryOffset == 0xFFFFFFFFL) throw IOException("ZIP64 archives are not supported")
        val directory = fetch(directoryOffset, directoryOffset + directorySize - 1)

        data class Raw(val name: String, val method: Int, val compressedSize: Long, val size: Long, val offset: Long)
        val raw = ArrayList<Raw>(count)
        var at = 0
        repeat(count) {
            if (u32(directory, at) != CENTRAL_SIGNATURE) throw IOException("Corrupt central directory")
            val nameLength = u16(directory, at + 28)
            val extraLength = u16(directory, at + 30)
            val commentLength = u16(directory, at + 32)
            raw += Raw(
                name = String(directory, at + 46, nameLength, Charsets.UTF_8),
                method = u16(directory, at + 10),
                compressedSize = u32(directory, at + 20),
                size = u32(directory, at + 24),
                offset = u32(directory, at + 42),
            )
            at += 46 + nameLength + extraLength + commentLength
        }
        val sorted = raw.sortedBy { it.offset }
        return sorted.mapIndexed { i, entry ->
            val end = if (i + 1 < sorted.size) sorted[i + 1].offset else directoryOffset
            Entry(entry.name, entry.method, entry.compressedSize, entry.size, entry.offset, end)
        }
    }

    /**
     * Calls [onEntry] with each of [wanted]'s contents, in offset order. Entries less than
     * [maxGap] apart are fetched in one request of at most [maxRequest] bytes.
     */
    fun read(wanted: List<Entry>, maxGap: Long = 1L shl 20, maxRequest: Long = 16L shl 20, onEntry: (Entry, ByteArray) -> Unit) {
        val entries = wanted.sortedBy { it.offset }
        var first = 0
        while (first < entries.size) {
            var last = first
            while (last + 1 < entries.size &&
                entries[last + 1].offset - entries[last].end < maxGap &&
                entries[last + 1].end - entries[first].offset <= maxRequest
            ) {
                last++
            }
            val start = entries[first].offset
            val chunk = fetch(start, entries[last].end - 1)
            for (i in first..last) {
                val entry = entries[i]
                val local = (entry.offset - start).toInt()
                if (u32(chunk, local) != LOCAL_SIGNATURE) throw IOException("Corrupt entry ${entry.name}")
                val dataStart = local + 30 + u16(chunk, local + 26) + u16(chunk, local + 28)
                onEntry(entry, content(entry, chunk, dataStart))
            }
            first = last + 1
        }
    }

    private fun content(entry: Entry, chunk: ByteArray, dataStart: Int): ByteArray {
        if (dataStart + entry.compressedSize > chunk.size) throw IOException("Truncated entry ${entry.name}")
        val bytes = when (entry.method) {
            0 -> chunk.copyOfRange(dataStart, dataStart + entry.compressedSize.toInt())
            8 -> {
                val inflater = Inflater(true)
                try {
                    inflater.setInput(chunk, dataStart, entry.compressedSize.toInt())
                    val out = ByteArrayOutputStream(entry.size.toInt())
                    val buffer = ByteArray(64 * 1024)
                    while (!inflater.finished()) {
                        val n = inflater.inflate(buffer)
                        if (n == 0 && (inflater.needsInput() || inflater.needsDictionary())) break
                        out.write(buffer, 0, n)
                    }
                    out.toByteArray()
                } finally {
                    inflater.end()
                }
            }
            else -> throw IOException("Entry ${entry.name} uses compression method ${entry.method}")
        }
        if (bytes.size.toLong() != entry.size) throw IOException("Entry ${entry.name} has the wrong size")
        return bytes
    }

    private fun u16(bytes: ByteArray, at: Int): Int =
        ByteBuffer.wrap(bytes, at, 2).order(ByteOrder.LITTLE_ENDIAN).short.toInt() and 0xFFFF

    private fun u32(bytes: ByteArray, at: Int): Long =
        ByteBuffer.wrap(bytes, at, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xFFFFFFFFL

    private companion object {
        const val EOCD_SIGNATURE = 0x06054b50L
        const val CENTRAL_SIGNATURE = 0x02014b50L
        const val LOCAL_SIGNATURE = 0x04034b50L
        const val EOCD_SIZE = 22
        const val EOCD_SEARCH = EOCD_SIZE + 0xFFFFL
    }
}
