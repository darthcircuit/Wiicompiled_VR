package org.wiicompiled.quest.launcher

import android.util.Log
import java.io.File
import java.io.IOException

/**
 * Config.toml on disk. Every edit re-reads the file first, so a value the
 * in-headset panel wrote meanwhile is never overwritten with a stale copy.
 */
class ConfigStore(val file: File) {

    /** The current file, empty when it does not exist yet, or null when it cannot be read. */
    fun load(): TomlConfig? {
        if (!file.isFile) {
            return TomlConfig.parse("")
        }
        return try {
            TomlConfig.parse(file.readText())
        } catch (e: IOException) {
            Log.w(TAG, "Cannot read ${file.absolutePath}", e)
            null
        }
    }

    /** Applies [edit] to a fresh read and writes the result back. */
    fun update(edit: (TomlConfig) -> Unit): Boolean {
        // A file that exists but cannot be read must not be replaced by one
        // holding only this edit.
        val config = load() ?: return false
        edit(config)
        val text = config.text()
        return try {
            file.parentFile?.mkdirs()
            val temporary = File(file.parentFile, "${file.name}.tmp")
            temporary.writeText(text)
            if (!temporary.renameTo(file)) {
                temporary.delete()
                file.writeText(text)
            }
            true
        } catch (e: IOException) {
            Log.w(TAG, "Cannot write ${file.absolutePath}", e)
            false
        }
    }

    private companion object {
        const val TAG = "WiiCompiledLauncher"
    }
}
