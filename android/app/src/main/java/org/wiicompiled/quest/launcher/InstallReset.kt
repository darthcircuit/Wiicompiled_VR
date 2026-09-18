package org.wiicompiled.quest.launcher

import android.content.Context
import android.util.Log
import java.io.File
import java.io.InterruptedIOException
import org.wiicompiled.quest.GameStorage

/**
 * Removes what the app put on the headset so the game can be set up again: the game files (DATA)
 * with the leftovers of an interrupted extraction or import, the built games with the on-device
 * build workspace, and the Retro Rewind pack with its own leftovers. Config.toml, the saves (NAND)
 * and the logs are never touched, so a reset costs no progress.
 */
object InstallReset {

    /** What to remove; at least one must be set for a reset to do anything. */
    data class Options(val gameFiles: Boolean, val games: Boolean, val modPack: Boolean) {
        val anything: Boolean get() = gameFiles || games || modPack
    }

    private const val TAG = "WiiCompiledLauncher"
    private const val REPORT_EVERY = 64L

    /** Null on success, otherwise the message to show. */
    fun run(context: Context, options: Options, progress: GameSetup.Progress, finishing: () -> Unit): String? {
        val roots = targets(
            GameStorage.gameRoot(context),
            File(context.filesDir, "game"),
            File(context.filesDir, "build"),
            options,
        )
        val total = roots.sumOf { countEntries(it) }.toLong()
        if (!progress.update(0, total)) throw InterruptedIOException("Reset cancelled")
        var done = 0L
        val kept = mutableListOf<File>()
        for (root in roots) {
            Log.i(TAG, "Removing ${root.absolutePath}")
            deleteTree(root, kept) {
                done++
                if (done % REPORT_EVERY == 0L && !progress.update(done, total)) {
                    throw InterruptedIOException("Reset cancelled")
                }
            }
        }
        finishing()
        if (kept.isNotEmpty()) {
            Log.w(TAG, "${kept.size} entries could not be removed, first ${kept.first().absolutePath}")
            return "${kept.size} files could not be removed, for example ${kept.first().absolutePath}. " +
                "Remove them over adb (adb shell rm -r), then try again."
        }
        Log.i(TAG, "Reset removed ${roots.size} trees, $done entries")
        return null
    }

    /**
     * The trees [options] select, as they lie on disk: DATA and any `DATA.*` staging leftover
     * beside it, every built game plus the build workspace, the pack and its own leftovers.
     */
    fun targets(gameRoot: File, gamesDir: File, buildDir: File, options: Options): List<File> {
        val roots = mutableListOf<File>()
        if (options.gameFiles) roots += siblings(gameRoot, GameStorage.DISC_DIRECTORY)
        if (options.games) roots += listOf(gamesDir, buildDir)
        if (options.modPack) roots += siblings(gameRoot, GameStorage.MOD_DIRECTORY)
        return roots.filter { it.exists() }
    }

    private fun siblings(parent: File, name: String): List<File> =
        parent.listFiles { file -> file.name == name || file.name.startsWith("$name.") }?.sortedBy { it.name } ?: emptyList()

    fun countEntries(root: File): Int = root.walkBottomUp().count()

    /** Deletes [root] bottom-up, calling [onEntry] per entry; whatever refuses to go joins [kept]. */
    fun deleteTree(root: File, kept: MutableList<File>, onEntry: () -> Unit) {
        for (entry in root.walkBottomUp()) {
            onEntry()
            if (!entry.delete() && entry.exists()) kept += entry
        }
    }
}
