package org.wiicompiled.quest

import android.content.Context
import java.io.File
import org.json.JSONObject

/**
 * The games this app can play, as the PC launcher offers them: the unmodded game and Retro Rewind.
 *
 * One APK carries a kit for each (`assets/game_kit`, see android/QuestGameKit.psm1) and the player
 * keeps a built game per profile in private storage, so switching between them is a toggle rather
 * than a second app. The choice is a file, not a preference, because the launcher and the game run
 * in different processes and the game reads it while starting.
 */
enum class GameProfile(val id: String, val title: Int, val modPack: Boolean) {
    Base("base", R.string.home_title_base, modPack = false),
    RetroRewind("retro_rewind", R.string.home_title_retro_rewind, modPack = true);

    companion object {
        private const val SELECTION_FILE = "selected-game"

        fun of(id: String?): GameProfile? = entries.firstOrNull { it.id == id }

        /** The profiles this APK's kit can build and load, in launcher order. */
        fun available(context: Context): List<GameProfile> {
            val products = runCatching {
                context.assets.open("game_kit/kit.json").use { input ->
                    JSONObject(input.reader().readText()).getJSONObject("products").keys().asSequence().toSet()
                }
            }.getOrDefault(setOf(Base.id))
            return entries.filter { it.id in products }.ifEmpty { listOf(Base) }
        }

        /** The profile the player last chose, or the first available one. */
        fun selected(context: Context): GameProfile {
            val available = available(context)
            val stored = runCatching { File(context.filesDir, SELECTION_FILE).readText().trim() }.getOrNull()
            return of(stored)?.takeIf { it in available } ?: available.first()
        }

        fun select(context: Context, profile: GameProfile) {
            File(context.filesDir, SELECTION_FILE).writeText(profile.id)
        }
    }
}
