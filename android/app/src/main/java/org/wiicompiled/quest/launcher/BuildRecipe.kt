package org.wiicompiled.quest.launcher

import java.security.MessageDigest

/**
 * The text-in, text-out parts of building the game on the headset ([GameBuild]): reading the
 * translator's outputs and turning the game kit's recipe (kit.json) into compiler and linker
 * command lines. They mirror android/QuestGameKit.psm1, which does the same on a PC.
 */
object BuildRecipe {

    /**
     * One argument of a response file for clang or lld on Android, which read them with GNU
     * quoting: a backslash escapes the next character.
     */
    fun responseFileArgument(argument: String): String = buildString {
        for (c in argument) {
            if (c == '\\' || c == '"' || c == '\'' || c.isWhitespace()) append('\\')
            append(c)
        }
    }

    fun responseFile(arguments: List<String>): String =
        arguments.joinToString(separator = "\n", postfix = "\n") { responseFileArgument(it) }

    /** Replaces the kit recipe's {name} placeholders; an unknown placeholder is an error. */
    fun expand(argument: String, values: Map<String, String>): String {
        var result = argument
        for ((name, value) in values) result = result.replace("{$name}", value)
        PLACEHOLDER.find(result)?.let { throw IllegalArgumentException("Unexpanded ${it.value} in the game kit recipe") }
        return result
    }

    /**
     * The link.lld arguments with every {game:slot} replaced by that slot's object files
     * (runtime, product, translated) and the other placeholders expanded.
     */
    fun linkArguments(template: List<String>, values: Map<String, String>, objects: Map<String, List<String>>): List<String> {
        val arguments = ArrayList<String>(template.size + objects.values.sumOf { it.size })
        for (argument in template) {
            val slot = GAME_SLOT.matchEntire(argument)?.groupValues?.get(1)
            if (slot != null) {
                arguments += objects[slot] ?: throw IllegalArgumentException("The game kit recipe names an unknown slot {game:$slot}")
            } else {
                arguments += expand(argument, values)
            }
        }
        return arguments
    }

    /** The quoted entries of `set(NAME ...)` in a shards.cmake, or an empty list without it. */
    fun sourceList(shardsCmake: String, name: String): List<String> {
        val body = Regex("""set\(${Regex.escape(name)}\s(.*?)\)""", RegexOption.DOT_MATCHES_ALL).find(shardsCmake)
            ?.groupValues?.get(1) ?: return emptyList()
        return Regex("\"([^\"]+)\"").findAll(body).map { it.groupValues[1].replace('\\', '/') }.toList()
    }

    /** The first translation entry point in recomp.yml, as the translator expects it (0x...). */
    fun entryPoint(manifest: String): String =
        Regex("""\n\s*entry_points:\s*\n\s*-\s*(0x[0-9A-Fa-f]{8})""").find(manifest)?.groupValues?.get(1)
            ?: throw IllegalArgumentException("recomp.yml names no translation entry point")

    /**
     * Where recomp.yml's Retro Rewind profile expects the mod, relative to the workspace. A base
     * translation only accounts for a mod whose Code.pul is there.
     */
    fun modRoot(manifest: String): String =
        Regex("""\n\s*mod_root:\s*(\S+)""").find(manifest)?.groupValues?.get(1)
            ?: throw IllegalArgumentException("recomp.yml names no Retro Rewind mod_root")

    /**
     * The blob assembly for ELF, as runtime/cmake/PublicProducts.cmake rewrites a Windows-generated
     * one. The headset's translator already writes ELF sections, so this normally changes nothing.
     */
    fun elfBlobAssembly(text: String): String {
        val elf = text.replace(".section .rdata,\"dr\"", ".section .rodata,\"a\",@progbits")
        return if (elf == text) text else elf + "\n.section .note.GNU-stack,\"\",@progbits\n"
    }

    /** An object file name for a generated source: its stem, which the translator keeps unique. */
    fun objectName(source: String): String = source.substringAfterLast('/').substringBeforeLast('.') + ".o"

    /** SHA-256 over "path sha256" lines sorted by path (ordinal), joined with \n. */
    fun listDigest(files: Map<String, String>): String {
        val lines = files.entries.map { "${it.key} ${it.value}" }.sorted()
        return hex(MessageDigest.getInstance("SHA-256").digest(lines.joinToString("\n").toByteArray(Charsets.UTF_8)))
    }

    fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it) }

    private val PLACEHOLDER = Regex("""\{[a-z:]+\}""")
    private val GAME_SLOT = Regex("""\{game:(\w+)\}""")
}
