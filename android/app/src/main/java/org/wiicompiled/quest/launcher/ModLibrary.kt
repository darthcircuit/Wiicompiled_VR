package org.wiicompiled.quest.launcher

import android.os.Handler
import android.os.Looper
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.util.Locale
import java.util.concurrent.Executors
import java.util.zip.ZipFile

/**
 * The mods imported on the Patches page, kept the way WheelWizard VR keeps them on a computer
 * (Features/Mods): one folder per mod under Mods/, holding the mod's files and a `<name>.ini` with
 * its state, so a Mods folder copied from one launcher reads the same in the other.
 *
 * Mods only change Retro Rewind. Before it starts, [plan] and [sync] flatten the enabled mods into
 * the pack's Patches folder, which the pack's Riivolution XML maps onto the disc (/patches, /sound),
 * as the PC launcher's ModsLaunchService does before every Retro Rewind launch. A file two mods
 * both carry comes from the one higher in the list, which is the one with the lower priority.
 */
object ModLibrary {

    /** One imported mod, as its `.ini` describes it. Author and ModID only come from the PC's mod browser. */
    data class Mod(
        val title: String,
        val enabled: Boolean,
        val priority: Int,
        val author: String = NO_ID,
        val modId: Int = -1,
    )

    /** One file picked for an import: its display name and how to read it. */
    class Source(val name: String, val open: () -> InputStream)

    enum class NameProblem { Empty, Exists, IllegalCharacters }

    private const val SECTION = "Mod"
    private const val NO_ID = "-1"

    /** ModManager._illegalChars plus Windows' invalid file name characters, so a name travels to the PC. */
    private val ILLEGAL_NAME_CHARACTERS = ".~/\\<>:\"|?*".toSet()

    /** Archives the PC unpacks with SharpCompress; without it, only .zip can be opened here. */
    private val UNSUPPORTED_ARCHIVES = listOf(".7z", ".rar")

    // Metadata

    /** Every mod under [modsDir] (`<name>/<name>.ini`), in list order: by priority, top first. */
    fun load(modsDir: File): List<Mod> =
        modsDir.listFiles { file -> file.isDirectory }
            .orEmpty()
            .mapNotNull { folder ->
                val ini = File(folder, "${folder.name}.ini")
                if (!ini.isFile) return@mapNotNull null
                runCatching { parseIni(ini.readText()) }.getOrNull()
            }
            .sortedWith(compareBy<Mod> { it.priority }.thenBy { it.title.lowercase(Locale.ROOT) })

    /**
     * Reads what Mod.LoadFromIniAsync reads, with its defaults: enabled unless it says otherwise,
     * priority 0 and ModID -1 when absent. Null without a name, which the PC skips as well.
     */
    fun parseIni(text: String): Mod? {
        val values = mutableMapOf<String, String>()
        var section = ""
        for (raw in text.removePrefix("﻿").lineSequence()) {
            val line = raw.trim()
            when {
                line.isEmpty() || line.startsWith(";") || line.startsWith("#") -> Unit
                line.startsWith("[") && line.endsWith("]") -> section = line.substring(1, line.length - 1).trim()
                section.equals(SECTION, ignoreCase = true) && '=' in line ->
                    values[line.substringBefore('=').trim().lowercase(Locale.ROOT)] = line.substringAfter('=').trim()
            }
        }
        val title = values["name"]?.takeIf { it.isNotBlank() } ?: return null
        return Mod(
            title = title,
            // bool.TryParse: either word in any case, and anything else keeps the default.
            enabled = when (values["isenabled"]?.lowercase(Locale.ROOT)) {
                "false" -> false
                else -> true
            },
            priority = values["priority"]?.toIntOrNull() ?: 0,
            author = values["author"] ?: NO_ID,
            modId = values["modid"]?.toIntOrNull() ?: -1,
        )
    }

    /** What Mod.SaveToIniAsync writes: the same keys, with .NET's True/False. */
    fun iniText(mod: Mod): String = buildString {
        append("[").append(SECTION).append("]\n")
        append("Name = ").append(mod.title).append('\n')
        append("Author = ").append(mod.author).append('\n')
        append("ModID = ").append(mod.modId).append('\n')
        append("IsEnabled = ").append(if (mod.enabled) "True" else "False").append('\n')
        append("Priority = ").append(mod.priority).append('\n')
    }

    fun save(modsDir: File, mod: Mod) {
        val folder = folder(modsDir, mod)
        if (!folder.isDirectory) throw IOException("The folder of ${mod.title} is missing: ${folder.absolutePath}")
        val ini = File(folder, "${mod.title}.ini")
        val temporary = File(folder, "${mod.title}.ini.tmp")
        temporary.writeText(iniText(mod))
        if (!temporary.renameTo(ini)) {
            temporary.delete()
            ini.writeText(iniText(mod))
        }
    }

    fun folder(modsDir: File, mod: Mod): File = File(modsDir, mod.title)

    /** ModManager.ValidateModName: a new name must be non-empty, unused (any case) and a valid folder name. */
    fun validateName(name: String, mods: List<Mod>): NameProblem? {
        val trimmed = name.trim()
        return when {
            trimmed.isEmpty() -> NameProblem.Empty
            mods.any { it.title.equals(trimmed, ignoreCase = true) } -> NameProblem.Exists
            trimmed.any { it in ILLEGAL_NAME_CHARACTERS || it.code < 32 } -> NameProblem.IllegalCharacters
            else -> null
        }
    }

    /** A starting name for the import dialog: the file's name without its extensions, made valid. */
    fun suggestName(fileName: String, mods: List<Mod>): String {
        val base = fileName.substringAfterLast('/').substringBefore('.').trim()
            .map { if (it in ILLEGAL_NAME_CHARACTERS || it.code < 32) ' ' else it }
            .joinToString("").trim()
        return base.takeIf { validateName(it, mods) == null } ?: ""
    }

    // Changes

    /**
     * ModManager.ImportModFilesAsync: the picked files become one new mod, enabled, below every
     * existing one. A picked .zip is unpacked into it, as the PC does with a mod it downloads.
     * The files are gathered beside the Mods folder's other entries and only moved into place once
     * all are in, so a failed import leaves nothing behind.
     */
    fun import(modsDir: File, title: String, sources: List<Source>, mods: List<Mod>): Mod {
        val name = title.trim()
        validateName(name, mods)?.let { throw IOException("The name $name cannot be used ($it).") }
        if (sources.isEmpty()) throw IOException("No files were chosen.")
        sources.firstOrNull { source -> UNSUPPORTED_ARCHIVES.any { source.name.endsWith(it, ignoreCase = true) } }?.let {
            throw IOException("${it.name} is an archive this headset cannot open. Unpack it on a computer and import its files, or import a .zip.")
        }
        modsDir.mkdirs()
        val staging = File(modsDir, ".$name.importing")
        staging.deleteRecursively()
        if (!staging.mkdirs()) throw IOException("Could not create ${staging.absolutePath}")
        try {
            for (source in sources) {
                val fileName = source.name.substringAfterLast('/').substringAfterLast('\\')
                if (fileName.isBlank() || fileName == "." || fileName == "..") throw IOException("A chosen file has no usable name.")
                if (fileName.endsWith(".zip", ignoreCase = true)) {
                    unpack(source, staging)
                } else {
                    source.open().use { input -> File(staging, fileName).outputStream().use { input.copyTo(it) } }
                }
            }
            if (staging.walkTopDown().none { it.isFile }) throw IOException("There was nothing to import.")
            val mod = Mod(name, enabled = true, priority = (mods.maxOfOrNull { it.priority } ?: 0) + 1)
            val target = folder(modsDir, mod)
            // No mod has this name, so a folder under it is what an interrupted import left.
            target.deleteRecursively()
            if (!staging.renameTo(target)) throw IOException("Could not move the mod into ${target.absolutePath}")
            save(modsDir, mod)
            return mod
        } finally {
            staging.deleteRecursively()
        }
    }

    /** Unpacks a picked .zip into [destination], refusing entries that would land outside it. */
    private fun unpack(source: Source, destination: File) {
        // ZipFile reads the central directory, which every zip has; streaming fails on some.
        val archive = File(destination, ".archive.zip")
        try {
            source.open().use { input -> archive.outputStream().use { input.copyTo(it) } }
            ZipFile(archive).use { zip ->
                for (entry in zip.entries()) {
                    if (entry.isDirectory) continue
                    val relative = entry.name.replace('\\', '/').trimStart('/')
                    if (relative.isEmpty() || relative.split('/').any { it == ".." }) {
                        throw IOException("${source.name} has a file outside its own folder (${entry.name}).")
                    }
                    val file = File(destination, relative)
                    file.parentFile?.mkdirs()
                    zip.getInputStream(entry).use { input -> file.outputStream().use { input.copyTo(it) } }
                }
            }
        } finally {
            archive.delete()
        }
    }

    fun delete(modsDir: File, mod: Mod) {
        val folder = folder(modsDir, mod)
        if (folder.exists() && !folder.deleteRecursively()) throw IOException("Could not delete ${folder.absolutePath}")
    }

    /** ModManager.RenameModAsync: the folder and its `.ini` take the new name. */
    fun rename(modsDir: File, mod: Mod, newTitle: String, mods: List<Mod>): Mod {
        val name = newTitle.trim()
        if (name == mod.title) return mod
        validateName(name, mods)?.let { throw IOException("The name $name cannot be used ($it).") }
        val renamed = mod.copy(title = name)
        val from = folder(modsDir, mod)
        val to = folder(modsDir, renamed)
        if (!from.renameTo(to)) throw IOException("Could not rename ${from.absolutePath}")
        File(to, "${mod.title}.ini").delete()
        save(modsDir, renamed)
        return renamed
    }

    /**
     * ModManager.DecreasePriorityAsync (up) and IncreasePriorityAsync (down): the mod swaps
     * priorities with its neighbour. Returns the two changed mods, or nothing at either end.
     */
    fun move(mods: List<Mod>, mod: Mod, up: Boolean): List<Mod> {
        val neighbour = if (up) {
            mods.filter { it.priority < mod.priority }.maxByOrNull { it.priority }
        } else {
            mods.filter { it.priority > mod.priority }.minByOrNull { it.priority }
        } ?: return emptyList()
        return listOf(mod.copy(priority = neighbour.priority), neighbour.copy(priority = mod.priority))
    }

    // Launch

    /**
     * ModsLaunchService.PrepareModsForLaunch: the file name each enabled mod's files take in the
     * Patches folder, and where each comes from. Mods are walked from the bottom of the list up and
     * a later one replaces an earlier one's file, so the top of the list wins. Names compare
     * without case, as on the PC and on the headset's shared storage.
     */
    fun plan(modsDir: File, mods: List<Mod>): Map<String, File> {
        val files = LinkedHashMap<String, Pair<String, File>>()
        for (mod in mods.sortedWith(compareByDescending<Mod> { it.priority }.thenByDescending { it.title.lowercase(Locale.ROOT) })) {
            if (!mod.enabled) continue
            val folder = folder(modsDir, mod)
            if (!folder.isDirectory) continue
            val metadata = File(folder, "${mod.title}.ini").absolutePath
            val contents = folder.walkTopDown()
                .filter { it.isFile && !it.absolutePath.equals(metadata, ignoreCase = true) }
                .sortedBy { it.relativeTo(folder).path.lowercase(Locale.ROOT) }
            for (file in contents) {
                val name = launchName(mod.priority, file.name)
                val key = name.lowercase(Locale.ROOT)
                files[key] = (files[key]?.first ?: name) to file
            }
        }
        return files.values.associate { it }
    }

    /**
     * ModsLaunchService.GetLaunchPatchFileName: a modding archive (`<name>.<tag>.szs`) is prefixed
     * with its mod's priority, so Pulsar can resolve two mods patching the same archive.
     */
    fun launchName(priority: Int, fileName: String): String {
        if (!isModdingArchive(fileName)) return fileName
        return "$priority.${stripPriorityPrefix(fileName)}"
    }

    private fun isModdingArchive(fileName: String): Boolean {
        if (!fileName.endsWith(".szs", ignoreCase = true)) return false
        val stem = fileName.substring(0, fileName.length - ".szs".length)
        val separator = stem.lastIndexOf('.')
        return separator > 0 && separator + 1 < stem.length
    }

    private fun stripPriorityPrefix(fileName: String): String {
        val digits = fileName.takeWhile { it.isDigit() }.length
        return if (digits > 0 && digits < fileName.length && fileName[digits] == '.') fileName.substring(digits + 1) else fileName
    }

    /** ModsLaunchService.ShouldAskToClearTargetFolder: no mod enabled, yet the Patches folder holds files. */
    fun shouldAskToClear(mods: List<Mod>, patchesDir: File): Boolean =
        mods.none { it.enabled } && patchesDir.listFiles { file -> file.isFile }.orEmpty().isNotEmpty()

    /**
     * ModsLaunchService.CopyFinalFiles: the Patches folder ends up holding exactly [plan]'s files.
     * Loose files no mod provides go; a file whose size and time already match is not copied again.
     */
    fun sync(patchesDir: File, plan: Map<String, File>) {
        patchesDir.mkdirs()
        if (!patchesDir.isDirectory) throw IOException("Could not create ${patchesDir.absolutePath}")
        val wanted = plan.keys.map { it.lowercase(Locale.ROOT) }.toSet()
        for (file in patchesDir.listFiles { file -> file.isFile }.orEmpty()) {
            if (file.name.lowercase(Locale.ROOT) !in wanted && !file.delete()) {
                throw IOException("Could not remove ${file.absolutePath}")
            }
        }
        for ((name, source) in plan) {
            val target = File(patchesDir, name)
            if (target.isFile && target.length() == source.length() && target.lastModified() == source.lastModified()) continue
            source.copyTo(target, overwrite = true)
            // Without the time, every start would copy everything again; that is all it costs.
            target.setLastModified(source.lastModified())
        }
    }

    /** ModsLaunchService with the folder-clearing answer: null on success, otherwise the message. */
    fun prepareForLaunch(modsDir: File, patchesDir: File, mods: List<Mod>, clear: Boolean): String? = try {
        when {
            mods.any { it.enabled } -> sync(patchesDir, plan(modsDir, mods))
            clear && patchesDir.exists() && !patchesDir.deleteRecursively() ->
                throw IOException("Could not clear ${patchesDir.absolutePath}")
        }
        null
    } catch (e: IOException) {
        e.message ?: e.toString()
    }

    // Background work

    private val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "ModLibrary").apply { isDaemon = true } }
    private val main by lazy { Handler(Looper.getMainLooper()) }

    /** True while an import runs; imports and launch preparation take turns on one thread. */
    @Volatile
    var importing = false
        private set

    /** Runs [work] off the main thread, one task at a time, and hands its result to [done] on the main thread. */
    fun <T> background(work: () -> T, done: (Result<T>) -> Unit) {
        worker.execute {
            val result = runCatching(work)
            main.post { done(result) }
        }
    }

    fun importInBackground(modsDir: File, title: String, sources: List<Source>, done: (Result<Mod>) -> Unit) {
        importing = true
        background({ import(modsDir, title, sources, load(modsDir)) }) { result ->
            importing = false
            done(result)
        }
    }
}
