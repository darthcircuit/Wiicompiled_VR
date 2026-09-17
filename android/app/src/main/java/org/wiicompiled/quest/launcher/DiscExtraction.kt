package org.wiicompiled.quest.launcher

import android.content.Context
import android.os.StatFs
import android.util.Log
import java.io.File
import java.io.IOException
import org.wiicompiled.quest.GameStorage

/**
 * The player's disc image becomes DATA, the way the PC installer does it
 * (Launcher/WiiCompiled.Setup.Windows/InstallerEngine.cs): read the header and require RMCP01,
 * check main.dol and StaticR.rel against the clean PAL pins, extract the data partition with nod,
 * and check the extracted files again before they replace DATA.
 *
 * The image is checked before anything is written, so a wrong disc fails in seconds rather than
 * after the extraction.
 */
object DiscExtraction {

    private const val TAG = "WiiCompiledLauncher"
    private const val STAGING_DIRECTORY = "DATA.extracting"
    // Headroom beyond the files themselves: FUSE and directory overhead.
    private const val FREE_SPACE_MARGIN = 256L * 1024 * 1024

    /** Null on success, otherwise the message to show. */
    fun run(context: Context, fd: Int, name: String?, progress: GameSetup.Progress, finishing: () -> Unit): String? {
        val checks = GameFiles.checks
        checks.extensionError(name)?.let { return it }

        val header = try {
            DiscChecks.parseHeader(NodDisc.header(fd))
        } catch (e: IOException) {
            return "This disc image could not be read. ${e.message.orEmpty()}".trim()
        }
        checks.compatibilityError(header)?.let { return it }
        checks.revisionError(NodDisc.readFile(fd, DiscChecks.DOL_PATH), NodDisc.readFile(fd, DiscChecks.REL_PATH))
            ?.let { return it }

        val gameRoot = GameStorage.gameRoot(context)
        gameRoot.mkdirs()
        val staging = File(gameRoot, STAGING_DIRECTORY)
        staging.deleteRecursively()
        try {
            val needed = header.extractedBytes + FREE_SPACE_MARGIN
            val available = StatFs(gameRoot.absolutePath).availableBytes
            if (available < needed) {
                return "Not enough free space: the game files need ${GameFiles.gigabytes(needed)}, and ${GameFiles.gigabytes(available)} is free."
            }

            staging.mkdirs()
            progress.update(0, header.extractedBytes)
            NodDisc.extract(fd, staging.absolutePath) { done, total -> progress.update(done, total) }

            finishing()
            GameFiles.validateData(staging)?.let { return it }
            GameFiles.replace(GameStorage.discDirectory(context), staging)?.let { return it }
            Log.i(TAG, "Game data extracted into ${GameStorage.discDirectory(context).absolutePath}")
            return null
        } finally {
            // Gone already after a successful run, which moved it to DATA.
            staging.deleteRecursively()
        }
    }
}
