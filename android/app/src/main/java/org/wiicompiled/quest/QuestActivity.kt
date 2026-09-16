package org.wiicompiled.quest

import android.app.AlertDialog
import android.content.Context
import android.os.Bundle
import android.system.Os
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import org.libsdl.app.SDLActivity
import org.libsdl.app.SDLSurface

/**
 * The one activity of the standalone Quest build.
 *
 * SDLActivity owns the surface, the Java-side event pump and the JNI plumbing
 * the OpenXR loader needs (the runtime fetches the JavaVM and this activity
 * through SDL_GetAndroidJNIEnv/SDL_GetAndroidActivity). What this subclass
 * adds is everything the native runtime cannot discover on its own:
 *
 *  - the data directory the player fills with the extracted disc
 *    (Android/data/<package>/files/WiiCompiled/DATA) and where Config.toml,
 *    saves and logs live;
 *  - the bundled read-only runtime resources, unpacked from the APK once;
 *  - a Config.toml with VR enabled and the disc path filled in.
 *
 * The paths are handed over through the environment before SDLActivity's own
 * onCreate loads the native libraries, because the runtime resolves them from
 * static initialisers where SDL's JNI helpers are not yet usable.
 */
class QuestActivity : SDLActivity() {

    override fun getLibraries(): Array<String> = arrayOf("SDL3", BuildConfig.MAIN_LIBRARY)

    override fun createSDLSurface(context: Context): SDLSurface = QuestSurface(context)

    override fun onCreate(savedInstanceState: Bundle?) {
        val dataRoot = (getExternalFilesDir(null) ?: filesDir)
        val gameRoot = File(dataRoot, APP_DIRECTORY)
        gameRoot.mkdirs()
        val resources = unpackRuntimeResources()

        Os.setenv("MKW_ANDROID_DATA_DIR", dataRoot.absolutePath, true)
        Os.setenv("MKW_ANDROID_RESOURCES_DIR", resources.absolutePath, true)
        ensureConfig(gameRoot)

        super.onCreate(savedInstanceState)

        if (!File(gameRoot, DISC_DIRECTORY).isDirectory && !mBrokenLibraries) {
            Log.w(TAG, "No extracted disc found under ${gameRoot.absolutePath}")
            AlertDialog.Builder(this)
                .setTitle(getString(R.string.missing_game_data_title))
                .setMessage(getString(R.string.missing_game_data_message, File(gameRoot, DISC_DIRECTORY).absolutePath))
                .setCancelable(false)
                .setPositiveButton(android.R.string.ok) { _, _ -> finish() }
                .show()
        }
    }

    /**
     * Copies runtime_resources/ from the APK assets into private storage the
     * first time this build runs. A stamp file keyed on the version code keeps
     * every later launch to one existence check.
     */
    private fun unpackRuntimeResources(): File {
        val target = File(filesDir, "runtime_resources")
        val stamp = File(target, ".version")
        val expected = "${BuildConfig.VERSION_CODE}:${BuildConfig.VERSION_NAME}"
        if (stamp.isFile && stamp.readText() == expected) {
            return target
        }
        target.deleteRecursively()
        target.mkdirs()
        copyAssetTree("runtime_resources", target)
        stamp.writeText(expected)
        Log.i(TAG, "Unpacked runtime resources into ${target.absolutePath}")
        return target
    }

    private fun copyAssetTree(assetPath: String, destination: File) {
        val entries = assets.list(assetPath) ?: emptyArray()
        if (entries.isEmpty()) {
            // A leaf: copy the file.
            assets.open(assetPath).use { input ->
                FileOutputStream(destination).use { output -> input.copyTo(output) }
            }
            return
        }
        destination.mkdirs()
        for (entry in entries) {
            copyAssetTree("$assetPath/$entry", File(destination, entry))
        }
    }

    /**
     * Writes a first Config.toml pointing at the disc directory with VR on.
     * An existing file is left alone: the settings overlay edits it in place.
     */
    private fun ensureConfig(gameRoot: File) {
        val config = File(gameRoot, "Config.toml")
        if (config.isFile) {
            return
        }
        val disc = File(gameRoot, DISC_DIRECTORY).absolutePath
        config.writeText(
            """
            # WiiCompiled Quest configuration. Edit with adb pull/push or the in-game overlay.
            [paths]
            dvd_root = "$disc"

            [video]
            widescreen = true
            resolution_multiplier = 1.0

            [vr]
            enabled = true
            render_scale = 1.0
            """.trimIndent() + "\n",
        )
    }

    companion object {
        private const val TAG = "WiiCompiledQuest"
        // Must match kApplicationDirectoryName in runtime/include/runtime_config.h.
        private const val APP_DIRECTORY = "WiiCompiledOpenXRVR"
        private const val DISC_DIRECTORY = "DATA"
    }
}
