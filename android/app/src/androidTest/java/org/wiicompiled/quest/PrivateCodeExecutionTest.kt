package org.wiicompiled.quest

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import java.io.File
import java.util.concurrent.TimeUnit
import java.util.zip.ZipFile
import org.junit.Test
import org.junit.runner.RunWith

/**
 * What an app targeting API 29+ may run from its own private storage, which is
 * where an on-device build puts the compiler and the compiled game.
 *
 * Run with `adb shell am instrument`, never `gradlew connectedAndroidTest`: that
 * task uninstalls the app afterwards, and with it the player's extracted DATA.
 */
@RunWith(AndroidJUnit4::class)
class PrivateCodeExecutionTest {

    private val context = InstrumentationRegistry.getInstrumentation().targetContext
    private val probe = File(context.filesDir, "probe").apply { mkdirs() }

    @Test
    fun loadsALibraryFromPrivateStorage() {
        val library = File(probe, "libnod_jni_copy.so")
        ZipFile(context.applicationInfo.sourceDir).use { apk ->
            apk.getInputStream(apk.getEntry("lib/arm64-v8a/libnod_jni.so")).use { input ->
                library.outputStream().use { input.copyTo(it) }
            }
        }
        report("System.load from filesDir") { System.load(library.absolutePath); "loaded" }
    }

    @Test
    fun runsABinaryFromPrivateStorage() {
        val binary = File(probe, "toybox")
        File("/system/bin/toybox").inputStream().use { input -> binary.outputStream().use { input.copyTo(it) } }
        binary.setExecutable(true)
        report("exec from filesDir") { run(binary.absolutePath, "echo", "direct") }
        report("exec through /system/bin/linker64") { run("/system/bin/linker64", binary.absolutePath, "echo", "linker") }
    }

    private fun run(vararg command: String): String {
        val process = ProcessBuilder(*command).redirectErrorStream(true).start()
        val output = process.inputStream.bufferedReader().readText().trim()
        process.waitFor(30, TimeUnit.SECONDS)
        return "exit ${process.exitValue()}: $output"
    }

    private fun report(what: String, probe: () -> String) {
        val outcome = try {
            probe()
        } catch (t: Throwable) {
            "FAILED ${t.javaClass.simpleName}: ${t.message}"
        }
        Log.i(TAG, "$what -> $outcome")
        InstrumentationRegistry.getInstrumentation().sendStatus(0, android.os.Bundle().apply { putString("stream", "$what -> $outcome\n") })
    }

    private companion object {
        const val TAG = "WiiCompiledProbe"
    }
}
