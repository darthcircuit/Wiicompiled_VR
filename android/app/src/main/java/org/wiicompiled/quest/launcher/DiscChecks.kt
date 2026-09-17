package org.wiicompiled.quest.launcher

import java.security.MessageDigest
import java.util.Locale

/**
 * The PC installer's acceptance rules for a player's disc image, in one place:
 * Launcher/WiiCompiled.Setup.Windows/InputValidation.cs (formats, game ID) and
 * InstallerEngine.ValidateExtractedGame (the clean PAL main.dol and StaticR.rel).
 * The pins come from projects/mkwii/recomp.yml through BuildConfig.
 */
class DiscChecks(
    private val gameId: String,
    private val dolSha256: String,
    private val relSha256: String,
) {

    data class Header(val gameId: String, val title: String, val extractedBytes: Long)

    /** Null when [displayName] is a format nod reads, otherwise the message to show. */
    fun extensionError(displayName: String?): String? {
        val extension = displayName?.substringAfterLast('.', "")?.lowercase(Locale.ROOT).orEmpty()
        return if (extension in SUPPORTED_EXTENSIONS) null else FORMAT_MESSAGE
    }

    fun compatibilityError(header: Header): String? {
        if (header.gameId.equals(gameId, ignoreCase = true)) {
            return null
        }
        return "This build supports Mario Kart Wii PAL ($gameId). The selected image is " +
            "${header.gameId} (${header.title}, ${region(header.gameId)})."
    }

    fun revisionError(dol: ByteArray, rel: ByteArray): String? {
        if (sha256(dol).equals(dolSha256, ignoreCase = true) && sha256(rel).equals(relSha256, ignoreCase = true)) {
            return null
        }
        return "The disc is $gameId but does not match the supported clean PAL revision. " +
            "Patched or otherwise modified game code cannot be installed safely."
    }

    companion object {
        const val DOL_PATH = "sys/main.dol"
        const val REL_PATH = "files/rel/StaticR.rel"
        const val FST_PATH = "sys/fst.bin"

        private val SUPPORTED_EXTENSIONS = setOf("iso", "gcm", "gcz", "ciso", "wbfs", "wia", "rvz")
        private const val FORMAT_MESSAGE =
            "Select a complete Wii disc image in ISO, GCM, GCZ, CISO, WBFS, WIA, or RVZ format."

        /** NodDisc.header's "<game id>\n<title>\n<bytes>". */
        fun parseHeader(text: String): Header {
            val lines = text.split('\n')
            return Header(
                gameId = lines.getOrElse(0) { "" },
                title = lines.getOrElse(1) { "" },
                extractedBytes = lines.getOrNull(2)?.toLongOrNull() ?: 0L,
            )
        }

        fun region(gameId: String): String = when (gameId.getOrNull(3)) {
            'P' -> "PAL"
            'E' -> "NTSC-U"
            'J' -> "NTSC-J"
            'K' -> "Korea"
            'W' -> "Taiwan"
            null -> "Unknown"
            else -> gameId[3].toString()
        }

        fun sha256(bytes: ByteArray): String =
            MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }
    }
}
