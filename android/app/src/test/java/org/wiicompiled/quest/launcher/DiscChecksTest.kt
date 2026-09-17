package org.wiicompiled.quest.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Test

/** The Quest launcher must accept and reject disc images exactly as the PC installer does. */
class DiscChecksTest {

    private val dol = "clean dol".toByteArray()
    private val rel = "clean rel".toByteArray()
    private val checks = DiscChecks("RMCP01", DiscChecks.sha256(dol), DiscChecks.sha256(rel))

    @Test
    fun acceptsTheFormatsNodReads() {
        for (name in listOf("mkw.iso", "MKW.ISO", "game.rvz", "game.wbfs", "a.b.wia", "x.ciso", "x.gcz", "x.gcm")) {
            assertNull(name, checks.extensionError(name))
        }
        for (name in listOf("mkw.zip", "DATA", "mkw.iso.part", null)) {
            assertNotNull(name, checks.extensionError(name))
        }
    }

    @Test
    fun parsesTheNativeHeader() {
        val header = DiscChecks.parseHeader("RMCP01\nMARIO KART Wii\n2891234567")
        assertEquals(DiscChecks.Header("RMCP01", "MARIO KART Wii", 2_891_234_567L), header)
        assertEquals(0L, DiscChecks.parseHeader("RMCP01").extractedBytes)
    }

    @Test
    fun rejectsOtherRegionsWithTheInstallerMessage() {
        assertNull(checks.compatibilityError(DiscChecks.Header("rmcp01", "MARIO KART Wii", 1)))
        assertEquals(
            "This build supports Mario Kart Wii PAL (RMCP01). The selected image is RMCE01 (MARIO KART Wii, NTSC-U).",
            checks.compatibilityError(DiscChecks.Header("RMCE01", "MARIO KART Wii", 1)),
        )
    }

    @Test
    fun requiresTheCleanRevision() {
        assertNull(checks.revisionError(dol, rel))
        assertNotNull(checks.revisionError("patched".toByteArray(), rel))
        assertNotNull(checks.revisionError(dol, "patched".toByteArray()))
    }

    @Test
    fun hashesAsLowercaseHex() {
        assertEquals(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            DiscChecks.sha256("abc".toByteArray()),
        )
    }
}
