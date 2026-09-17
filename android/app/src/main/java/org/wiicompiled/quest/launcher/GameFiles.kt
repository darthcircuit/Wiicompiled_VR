package org.wiicompiled.quest.launcher

import android.util.Log
import java.io.File
import org.wiicompiled.quest.BuildConfig

/** Checks and swaps shared by every [GameSetup] task. */
object GameFiles {

    private const val TAG = "WiiCompiledLauncher"

    val checks = DiscChecks(BuildConfig.DISC_GAME_ID, BuildConfig.DISC_DOL_SHA256, BuildConfig.DISC_REL_SHA256)

    /**
     * InstallerEngine.ValidateExtractedGame (the PC installer): the files the runtime needs, with
     * the pinned hashes. Null when [root] is a usable DATA folder.
     */
    fun validateData(root: File): String? {
        val dol = File(root, DiscChecks.DOL_PATH)
        val rel = File(root, DiscChecks.REL_PATH)
        if (!dol.isFile || !rel.isFile || !File(root, DiscChecks.FST_PATH).isFile) {
            return "The game files are missing required Mario Kart Wii files."
        }
        return checks.revisionError(dol.readBytes(), rel.readBytes())
    }

    /**
     * Moves [staging] to [destination], keeping the old destination until the new one is in
     * place. Null on success, otherwise the message to show.
     */
    fun replace(destination: File, staging: File): String? {
        val replaced = File(destination.parentFile, "${destination.name}.replaced")
        replaced.deleteRecursively()
        if (destination.exists() && !destination.renameTo(replaced)) {
            return "The existing ${destination.name} folder could not be replaced. Remove it, for example with " +
                "adb shell rm -r ${destination.absolutePath}, and try again."
        }
        if (!staging.renameTo(destination)) {
            replaced.renameTo(destination)
            return "The new files could not be moved into ${destination.absolutePath}."
        }
        // The new copy is in place; an old one that will not delete only costs space.
        if (replaced.exists() && !replaced.deleteRecursively()) {
            Log.w(TAG, "Could not remove ${replaced.absolutePath}")
        }
        return null
    }

    fun gigabytes(bytes: Long) = "%.1f GB".format(bytes / 1_000_000_000.0)
}
