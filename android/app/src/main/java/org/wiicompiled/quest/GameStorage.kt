package org.wiicompiled.quest

import android.content.Context
import java.io.File

/**
 * Where the game's user files live on the headset. The launcher and the game
 * activity run in different processes and both resolve every path here, so
 * they cannot disagree about which Config.toml or DATA directory is meant.
 *
 * Everything sits under Android/data/<package>/files/WiiCompiledOpenXRVR, the
 * one directory a player can reach over adb: the extracted disc (DATA),
 * Config.toml, NAND saves and per-run logs.
 */
object GameStorage {

    // Must match kApplicationDirectoryName in runtime/include/runtime_config.h.
    const val APP_DIRECTORY = "WiiCompiledOpenXRVR"
    const val DISC_DIRECTORY = "DATA"

    enum class DiscStatus { Missing, Incomplete, Ready }

    fun dataRoot(context: Context): File = context.getExternalFilesDir(null) ?: context.filesDir

    fun gameRoot(context: Context): File = File(dataRoot(context), APP_DIRECTORY)

    fun discDirectory(context: Context): File = File(gameRoot(context), DISC_DIRECTORY)

    fun configFile(context: Context): File = File(gameRoot(context), "Config.toml")

    fun logsDirectory(context: Context): File = File(gameRoot(context), "Logs")

    /** Game packages dropped here (adb push, Build-QuestGame.ps1 -Install) are imported when the launcher opens. */
    fun importDirectory(context: Context): File = File(gameRoot(context), "Import")

    /**
     * Whether DATA holds what the runtime's DVD layer accepts (IsDvdDataRoot in
     * runtime/src/hle/storage/dvd.cpp): the whole extracted partition, not just
     * its files/ folder. Incomplete also covers a tree adb left unreadable to
     * the app, which looks the same from here.
     */
    fun discStatus(context: Context): DiscStatus {
        val disc = discDirectory(context)
        if (!disc.isDirectory) {
            return DiscStatus.Missing
        }
        val usable = File(disc, "files").isDirectory && File(disc, "sys/fst.bin").isFile
        return if (usable) DiscStatus.Ready else DiscStatus.Incomplete
    }

    /**
     * Creates the game directory and a first Config.toml with VR on and the
     * disc path filled in. An existing file is left alone: the launcher, the
     * in-headset settings panel and the player all edit it in place.
     */
    fun prepare(context: Context): File {
        val gameRoot = gameRoot(context)
        gameRoot.mkdirs()
        // Created by the app so it owns it and can remove imported packages; adb only adds files.
        importDirectory(context).mkdirs()
        val config = configFile(context)
        if (!config.isFile) {
            val disc = discDirectory(context).absolutePath
            config.writeText(
                """
                # WiiCompiled Quest configuration. Edit with the launcher, the in-game panel or adb pull/push.
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
        return gameRoot
    }
}
