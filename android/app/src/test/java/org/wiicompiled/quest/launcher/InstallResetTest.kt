package org.wiicompiled.quest.launcher

import java.io.File
import java.nio.file.Files
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** A reset removes exactly the trees chosen, with their leftovers, and nothing a player keeps. */
class InstallResetTest {

    private fun tree(root: File, vararg paths: String): File {
        for (path in paths) File(root, path).apply { parentFile?.mkdirs(); writeText(path) }
        return root
    }

    private fun fixture(): Triple<File, File, File> {
        val base = Files.createTempDirectory("reset").toFile()
        val gameRoot = tree(
            File(base, "WiiCompiledOpenXRVR"),
            "DATA/sys/main.dol", "DATA/files/rel/StaticR.rel", "DATA.extracting/sys/main.dol", "DATA.replaced/x",
            "RetroRewind6/Binaries/Code.pul", "RetroRewind6.downloading/version.txt",
            "Config.toml", "NAND/shared2/save.bin", "Logs/run/console.log", "Import/game.wcgame",
        )
        val games = tree(File(base, "files/game"), "base/libmain.so", "base/game.json", "retro_rewind/libmain.so")
        val build = tree(File(base, "files/build"), "toolchain/llvm/bin/clang-21")
        return Triple(gameRoot, games, build)
    }

    @Test
    fun gameFilesTakeTheirLeftoversAndNothingElse() {
        val (gameRoot, games, build) = fixture()
        val options = InstallReset.Options(gameFiles = true, games = false, modPack = false)
        val targets = InstallReset.targets(gameRoot, games, build, options)
        assertEquals(listOf("DATA", "DATA.extracting", "DATA.replaced"), targets.map { it.name })
        for (target in targets) InstallReset.deleteTree(target, mutableListOf()) {}
        assertFalse(File(gameRoot, "DATA").exists())
        assertFalse(File(gameRoot, "DATA.extracting").exists())
        assertTrue(File(gameRoot, "Config.toml").isFile)
        assertTrue(File(gameRoot, "NAND/shared2/save.bin").isFile)
        assertTrue(File(gameRoot, "RetroRewind6/Binaries/Code.pul").isFile)
        assertTrue(File(games, "base/libmain.so").isFile)
    }

    @Test
    fun gamesAndPackAreSeparateChoices() {
        val (gameRoot, games, build) = fixture()
        val both = InstallReset.Options(gameFiles = false, games = true, modPack = true)
        assertEquals(
            listOf("game", "build", "RetroRewind6", "RetroRewind6.downloading"),
            InstallReset.targets(gameRoot, games, build, both).map { it.name },
        )
        val nothing = InstallReset.Options(gameFiles = false, games = false, modPack = false)
        assertFalse(nothing.anything)
        assertEquals(0, InstallReset.targets(gameRoot, games, build, nothing).size)
    }

    @Test
    fun countsEveryEntryItWillVisit() {
        val (gameRoot, _, _) = fixture()
        val data = File(gameRoot, "DATA")
        val expected = InstallReset.countEntries(data)
        var visited = 0
        InstallReset.deleteTree(data, mutableListOf()) { visited++ }
        assertEquals(expected, visited)
        assertFalse(data.exists())
    }
}
