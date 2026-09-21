package org.wiicompiled.quest.launcher

import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** The Patches page keeps mods as WheelWizard does and fills the Patches folder as it does at launch. */
class ModLibraryTest {

    private fun temp(): File = Files.createTempDirectory("mods").toFile()

    private fun source(name: String, text: String) = ModLibrary.Source(name) { text.byteInputStream() }

    private fun zip(name: String, vararg entries: Pair<String, String>): ModLibrary.Source {
        val bytes = ByteArrayOutputStream().also { out ->
            ZipOutputStream(out).use { zip ->
                for ((path, text) in entries) {
                    zip.putNextEntry(ZipEntry(path))
                    zip.write(text.toByteArray())
                    zip.closeEntry()
                }
            }
        }.toByteArray()
        return ModLibrary.Source(name) { bytes.inputStream() }
    }

    @Test
    fun readsTheIniThePcWrites() {
        // IniParser's output: CRLF, spaces around '=', .NET's True/False, sometimes a BOM.
        val text = "﻿[Mod]\r\nName = Fast Karts\r\nAuthor = someone\r\nModID = 4242\r\nIsEnabled = False\r\nPriority = 3\r\n"
        assertEquals(ModLibrary.Mod("Fast Karts", enabled = false, priority = 3, author = "someone", modId = 4242), ModLibrary.parseIni(text))
        // Absent values take the PC's defaults; a nameless file is no mod.
        assertEquals(ModLibrary.Mod("Solo", enabled = true, priority = 0), ModLibrary.parseIni("[Mod]\nName=Solo\n"))
        assertNull(ModLibrary.parseIni("[Mod]\nPriority = 1\n"))
        val mod = ModLibrary.Mod("Round Trip", enabled = true, priority = 7, author = "a", modId = 9)
        assertEquals(mod, ModLibrary.parseIni(ModLibrary.iniText(mod)))
        assertTrue(ModLibrary.iniText(mod).contains("IsEnabled = True"))
    }

    @Test
    fun namesFollowThePcRules() {
        val mods = listOf(ModLibrary.Mod("Taken", true, 1))
        assertEquals(ModLibrary.NameProblem.Empty, ModLibrary.validateName("  ", mods))
        assertEquals(ModLibrary.NameProblem.Exists, ModLibrary.validateName("taken", mods))
        assertEquals(ModLibrary.NameProblem.IllegalCharacters, ModLibrary.validateName("v1.2", mods))
        assertEquals(ModLibrary.NameProblem.IllegalCharacters, ModLibrary.validateName("a:b", mods))
        assertNull(ModLibrary.validateName(" New Mod ", mods))
        assertEquals("Fire Kart", ModLibrary.suggestName("Fire Kart.tag.szs", mods))
        assertEquals("", ModLibrary.suggestName("Taken.zip", mods))
    }

    @Test
    fun importsLooseFilesAndZipsIntoOneMod() {
        val modsDir = temp()
        val first = ModLibrary.import(modsDir, " Karts ", listOf(source("Common.szs", "c"), zip("pack.zip", "Patches/Menu.szs" to "m")), emptyList())
        assertEquals(ModLibrary.Mod("Karts", enabled = true, priority = 1), first)
        assertEquals("c", File(modsDir, "Karts/Common.szs").readText())
        assertEquals("m", File(modsDir, "Karts/Patches/Menu.szs").readText())
        assertTrue(File(modsDir, "Karts/Karts.ini").isFile)
        // A new mod goes below every existing one, and nothing is left of the staging folder.
        val second = ModLibrary.import(modsDir, "Music", listOf(source("a.brstm", "a")), ModLibrary.load(modsDir))
        assertEquals(2, second.priority)
        assertEquals(listOf("Karts", "Music"), ModLibrary.load(modsDir).map { it.title })
        assertEquals(setOf("Karts", "Music"), modsDir.list()!!.toSet())
    }

    @Test
    fun refusesWhatItCannotImportAndLeavesNothing() {
        val modsDir = temp()
        for (sources in listOf(listOf(zip("evil.zip", "../escape.szs" to "x")), listOf(source("mod.rar", "r")), listOf(zip("empty.zip", "folder/" to "")))) {
            try {
                ModLibrary.import(modsDir, "Bad", sources, emptyList())
                fail("imported ${sources.map { it.name }}")
            } catch (expected: IOException) {
            }
        }
        assertFalse(File(modsDir.parentFile, "escape.szs").exists())
        assertEquals(0, modsDir.list()!!.size)
    }

    @Test
    fun movingSwapsPriorityWithTheNeighbour() {
        val mods = listOf(ModLibrary.Mod("A", true, 1), ModLibrary.Mod("B", true, 2), ModLibrary.Mod("C", true, 5))
        assertEquals(listOf(ModLibrary.Mod("B", true, 1), ModLibrary.Mod("A", true, 2)), ModLibrary.move(mods, mods[1], up = true))
        assertEquals(listOf(ModLibrary.Mod("B", true, 5), ModLibrary.Mod("C", true, 2)), ModLibrary.move(mods, mods[1], up = false))
        assertTrue(ModLibrary.move(mods, mods[0], up = true).isEmpty())
    }

    @Test
    fun renameMovesTheFolderAndItsIni() {
        val modsDir = temp()
        val mod = ModLibrary.import(modsDir, "Old", listOf(source("x.szs", "x")), emptyList())
        val renamed = ModLibrary.rename(modsDir, mod, "New", ModLibrary.load(modsDir))
        assertEquals(listOf(renamed), ModLibrary.load(modsDir))
        assertTrue(File(modsDir, "New/x.szs").isFile)
        assertFalse(File(modsDir, "New/Old.ini").exists())
    }

    @Test
    fun modArchivesTakeTheirModsPriority() {
        assertEquals("3.Kart.tag.szs", ModLibrary.launchName(3, "Kart.tag.szs"))
        assertEquals("3.Kart.tag.szs", ModLibrary.launchName(3, "12.Kart.tag.szs"))
        assertEquals("Common.szs", ModLibrary.launchName(3, "Common.szs"))
        assertEquals("track.brstm", ModLibrary.launchName(3, "track.brstm"))
    }

    @Test
    fun theTopOfTheListWinsAndDisabledModsAreLeftOut() {
        val modsDir = temp()
        val top = ModLibrary.import(modsDir, "Top", listOf(source("Common.szs", "top"), source("Top.brstm", "t")), emptyList())
        val bottom = ModLibrary.import(modsDir, "Bottom", listOf(source("COMMON.szs", "bottom"), source("Bottom.brstm", "b")), listOf(top))
        val off = ModLibrary.Mod("Off", enabled = false, priority = 3)
        File(modsDir, "Off").mkdirs()
        File(modsDir, "Off/Off.brstm").writeText("o")
        ModLibrary.save(modsDir, off)

        val plan = ModLibrary.plan(modsDir, ModLibrary.load(modsDir))
        assertEquals(setOf("common.szs", "top.brstm", "bottom.brstm"), plan.keys.map { it.lowercase() }.toSet())
        assertEquals("top", plan.entries.single { it.key.equals("common.szs", ignoreCase = true) }.value.readText())
        assertEquals(listOf(top, bottom, off), ModLibrary.load(modsDir))
    }

    @Test
    fun syncLeavesExactlyThePlanInPatches() {
        val modsDir = temp()
        val mod = ModLibrary.import(modsDir, "Mod", listOf(source("New.szs", "new")), emptyList())
        val patches = File(temp(), "Patches").apply { mkdirs() }
        File(patches, "Stale.szs").writeText("stale")
        File(patches, "keep-folder").mkdirs()

        ModLibrary.sync(patches, ModLibrary.plan(modsDir, listOf(mod)))
        assertEquals(setOf("New.szs", "keep-folder"), patches.list()!!.toSet())
        assertEquals("new", File(patches, "New.szs").readText())

        // Nothing enabled: the folder is only cleared when the player agrees.
        val disabled = listOf(mod.copy(enabled = false))
        assertTrue(ModLibrary.shouldAskToClear(disabled, patches))
        assertNull(ModLibrary.prepareForLaunch(modsDir, patches, disabled, clear = false))
        assertTrue(File(patches, "New.szs").isFile)
        assertNull(ModLibrary.prepareForLaunch(modsDir, patches, disabled, clear = true))
        assertFalse(patches.exists())
        assertFalse(ModLibrary.shouldAskToClear(disabled, patches))
    }
}
