package org.wiicompiled.quest.launcher

import java.io.File
import java.io.InterruptedIOException
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread

/**
 * Runs a build tool from the app's private storage. Android refuses to exec files there, but lets
 * the system linker load them (the same permission that lets the game load libmain.so), so every
 * tool starts as `/system/bin/linker64 <absolute path> <arguments>`.
 *
 * Output goes to [log] and to the caller line by line. [cancelled] is polled while the tool runs;
 * when it turns true the tool is stopped and [run] throws [InterruptedIOException].
 */
class ToolProcess(
    private val environment: Map<String, String>,
    private val log: BuildLog,
    private val cancelled: () -> Boolean,
) {

    class Result(val exitCode: Int, val tail: List<String>) {
        /** Android's low memory killer ends a process with SIGKILL, which reads as 128 + 9. */
        val killed: Boolean get() = exitCode == 137
    }

    fun run(workingDirectory: File, executable: File, arguments: List<String>, onLine: (String) -> Unit = {}): Result {
        val command = listOf(LINKER, executable.absolutePath) + arguments
        val process = ProcessBuilder(command)
            .directory(workingDirectory)
            .redirectErrorStream(true)
            .apply { environment().putAll(this@ToolProcess.environment) }
            .start()
        val tail = ArrayDeque<String>()
        val reader = thread(name = "BuildOutput") {
            process.inputStream.bufferedReader().useLines { lines ->
                for (line in lines) {
                    log.line(line)
                    synchronized(tail) {
                        tail.addLast(line)
                        if (tail.size > TAIL_LINES) tail.removeFirst()
                    }
                    onLine(line)
                }
            }
        }
        try {
            while (!process.waitFor(POLL_MS, TimeUnit.MILLISECONDS)) {
                if (cancelled()) {
                    stop(process)
                    throw InterruptedIOException("Build cancelled")
                }
            }
        } catch (e: InterruptedException) {
            stop(process)
            throw InterruptedIOException("Build interrupted")
        }
        reader.join(READER_JOIN_MS)
        return Result(process.exitValue(), synchronized(tail) { tail.toList() })
    }

    private fun stop(process: Process) {
        process.destroy()
        if (!process.waitFor(STOP_GRACE_MS, TimeUnit.MILLISECONDS)) process.destroyForcibly()
    }

    private companion object {
        const val LINKER = "/system/bin/linker64"
        const val POLL_MS = 250L
        const val STOP_GRACE_MS = 2000L
        const val READER_JOIN_MS = 5000L
        const val TAIL_LINES = 40
    }
}

/** The build's log file, which several tools write to at once. */
class BuildLog(val file: File) {
    init {
        file.parentFile?.mkdirs()
    }

    private val writer = file.bufferedWriter()

    @Synchronized
    fun line(text: String) {
        writer.write(text)
        writer.newLine()
        writer.flush()
    }

    @Synchronized
    fun close() = writer.close()
}
