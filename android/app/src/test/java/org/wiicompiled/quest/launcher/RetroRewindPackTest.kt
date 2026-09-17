package org.wiicompiled.quest.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** The headset reads Retro Rewind's published feeds exactly as the PC launcher does. */
class RetroRewindPackTest {

    private val versions = """
        6.12.5 https://update.rwfc.net/RetroRewind/zip/6.12.5.zip /RetroRewind6 New tracks
        6.12.6 http://update.rwfc.net:8000/RetroRewind/zip/6.12.6.zip /RetroRewind6 Fixes
        6.12.7 https://update.rwfc.net/RetroRewind/zip/6.12.7.zip /RetroRewind6 More fixes
        broken line without enough fields
    """.trimIndent()

    @Test
    fun readsTheVersionFeedAndMovesOldDownloadsToTheCurrentHost() {
        val updates = RetroRewindPack.parseUpdates(versions)
        assertEquals(listOf("6.12.5", "6.12.6", "6.12.7"), updates.map { it.version })
        assertEquals("https://update.rwfc.net/RetroRewind/zip/6.12.6.zip", updates[1].url)
        assertEquals("More fixes", updates[2].description)
    }

    @Test
    fun appliesOnlyTheUpdatesNewerThanTheInstalledVersion() {
        val updates = RetroRewindPack.parseUpdates(versions)
        assertEquals(listOf("6.12.6", "6.12.7"), RetroRewindPack.updatesAfter("6.12.5", updates).map { it.version })
        assertEquals(emptyList<String>(), RetroRewindPack.updatesAfter("6.12.7", updates).map { it.version })
        // Nothing installed takes every published update, oldest first.
        assertEquals(listOf("6.12.5", "6.12.6", "6.12.7"), RetroRewindPack.updatesAfter(null, updates).map { it.version })
    }

    @Test
    fun comparesVersionsByNumberNotByText() {
        assertTrue(RetroRewindPack.compare("6.12.7", "6.9.9") > 0)
        assertTrue(RetroRewindPack.compare("6.2", "6.2.0") == 0)
        assertTrue(RetroRewindPack.compare("6.12.7", "6.12.7") == 0)
    }

    @Test
    fun deletesOnlyWhatTheUpdatesInRangeDropFromInsideThePack() {
        val deletions = RetroRewindPack.parseDeletions(
            """
            6.12.5 RetroRewind6/Race/Course/old.szs
            6.12.6 riivolution/RetroRewind6.xml
            6.12.6 RetroRewind6/Assets/gone.brres
            6.12.7 RetroRewindOld.zip
            6.12.8 RetroRewind6/Assets/future.brres
            """.trimIndent()
        )
        assertEquals(
            listOf("Race/Course/old.szs", "Assets/gone.brres"),
            RetroRewindPack.deletionsBetween(null, "6.12.7", deletions),
        )
        assertEquals(
            listOf("Assets/gone.brres"),
            RetroRewindPack.deletionsBetween("6.12.5", "6.12.7", deletions),
        )
    }

    @Test
    fun keepsOnlyPackPathsAndRefusesOnesThatClimbOut() {
        assertEquals("Binaries/Code.pul", RetroRewindPack.packRelative("RetroRewind6/Binaries/Code.pul"))
        assertEquals("Binaries/Code.pul", RetroRewindPack.packRelative("/RetroRewind6\\Binaries\\Code.pul"))
        assertNull(RetroRewindPack.packRelative("riivolution/RetroRewind6.xml"))
        assertNull(RetroRewindPack.packRelative("RetroRewind6/../escape.txt"))
        assertNull(RetroRewindPack.packRelative("RetroRewind6/"))
    }
}
