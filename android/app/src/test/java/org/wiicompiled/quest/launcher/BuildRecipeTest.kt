package org.wiicompiled.quest.launcher

import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Test

/** The headset must turn the game kit's recipe into the same commands android/QuestGameKit.psm1 runs. */
class BuildRecipeTest {

    @Test
    fun quotesResponseFileArgumentsForGnuTokenizers() {
        assertEquals("-DIMGUI_USER_CONFIG=\\\"aurora/imgui_config.h\\\"", BuildRecipe.responseFileArgument("-DIMGUI_USER_CONFIG=\"aurora/imgui_config.h\""))
        assertEquals("/data/with\\ space/a\\\\b", BuildRecipe.responseFileArgument("/data/with space/a\\b"))
        assertEquals("-O2\n-c\n", BuildRecipe.responseFile(listOf("-O2", "-c")))
    }

    @Test
    fun expandsPlaceholdersAndRejectsUnknownOnes() {
        val values = mapOf("kit" to "/k", "sysroot" to "/n/sysroot")
        assertEquals("-I/k/include", BuildRecipe.expand("-I{kit}/include", values))
        assertEquals("--sysroot=/n/sysroot", BuildRecipe.expand("--sysroot={sysroot}", values))
        assertThrows(IllegalArgumentException::class.java) { BuildRecipe.expand("-I{workspace}", values) }
    }

    @Test
    fun expandsTheLinkSlotsInPlace() {
        val template = listOf("-o", "{output}", "{kit}/objects/001_a.o", "{game:runtime}", "--start-lib", "{game:translated}", "--end-lib", "-lm")
        val arguments = BuildRecipe.linkArguments(
            template,
            mapOf("kit" to "/k", "output" to "/w/libmain.so"),
            mapOf("runtime" to listOf("/o/data.o", "/o/blob.o"), "translated" to listOf("/o/s1.o", "/o/s2.o")),
        )
        assertEquals(
            listOf("-o", "/w/libmain.so", "/k/objects/001_a.o", "/o/data.o", "/o/blob.o", "--start-lib", "/o/s1.o", "/o/s2.o", "--end-lib", "-lm"),
            arguments,
        )
        assertThrows(IllegalArgumentException::class.java) {
            BuildRecipe.linkArguments(listOf("{game:product}"), emptyMap(), emptyMap())
        }
    }

    @Test
    fun readsSourceListsFromShardsCmake() {
        val shards = """
            set(MKW_TRANSLATED_SHARD_ROOT "/w/generated/build_shards")
            set(MKW_BASE_COMMON_SHARDS
              "/w/generated/build_shards/base_common/shard_1.cpp"
              "/w/generated/build_shards/base_common/shard_2.cpp"
            )
            set(MKW_BASE_PORTABLE_SENSITIVE_SHARDS
            )
            set(MKW_BASE_REGISTRATION_SOURCES
              "C:\w\generated\build_shards\base_registration\registration_00.cpp"
            )
        """.trimIndent()
        assertEquals(
            listOf("/w/generated/build_shards/base_common/shard_1.cpp", "/w/generated/build_shards/base_common/shard_2.cpp"),
            BuildRecipe.sourceList(shards, "MKW_BASE_COMMON_SHARDS"),
        )
        assertEquals(emptyList<String>(), BuildRecipe.sourceList(shards, "MKW_BASE_PORTABLE_SENSITIVE_SHARDS"))
        assertEquals(emptyList<String>(), BuildRecipe.sourceList(shards, "MKW_RETRO_MOD_SHARDS"))
        assertEquals(listOf("C:/w/generated/build_shards/base_registration/registration_00.cpp"), BuildRecipe.sourceList(shards, BuildRecipe.REGISTRATION_LIST))
    }

    @Test
    fun readsTheEntryPointFromTheGameManifest() {
        val manifest = "translation:\n  entry_points:\n    - 0x800060A4\n  function_map:\n    path: MAP.txt\n"
        assertEquals("0x800060A4", BuildRecipe.entryPoint(manifest))
        assertThrows(IllegalArgumentException::class.java) { BuildRecipe.entryPoint("translation:\n") }
    }

    @Test
    fun rewritesWindowsBlobSectionsForElfOnly() {
        val windows = "// blobs\n.section .rdata,\"dr\"\n.incbin \"a.bin\"\n"
        assertEquals(
            "// blobs\n.section .rodata,\"a\",@progbits\n.incbin \"a.bin\"\n\n.section .note.GNU-stack,\"\",@progbits\n",
            BuildRecipe.elfBlobAssembly(windows),
        )
        val elf = "// blobs\n.section .rodata,\"a\",@progbits\n"
        assertEquals(elf, BuildRecipe.elfBlobAssembly(elf))
    }

    @Test
    fun namesObjectsAfterTheSourceStem() {
        assertEquals("shard_8b98.o", BuildRecipe.objectName("/w/generated/build_shards/base_common/shard_8b98.cpp"))
        assertEquals("data_sections_init_blobs_android.o", BuildRecipe.objectName("/w/data_sections_init_blobs_android.S"))
    }

    @Test
    fun digestsTheFileListLikePrepareQuestToolchain() {
        // SHA-256 of "a/x.h 11\nb.so 22", the lines sorted by path and joined with \n.
        assertEquals(
            java.security.MessageDigest.getInstance("SHA-256").digest("a/x.h 11\nb.so 22".toByteArray()).joinToString("") { "%02x".format(it) },
            BuildRecipe.listDigest(mapOf("b.so" to "22", "a/x.h" to "11")),
        )
    }
}
