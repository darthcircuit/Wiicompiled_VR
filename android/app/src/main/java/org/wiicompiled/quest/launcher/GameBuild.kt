package org.wiicompiled.quest.launcher

import android.app.ActivityManager
import android.content.Context
import android.os.StatFs
import android.util.Log
import java.io.File
import java.io.IOException
import java.io.InterruptedIOException
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.Executors
import java.util.concurrent.Future
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.atomic.AtomicReference
import java.util.zip.ZipInputStream
import org.json.JSONArray
import org.json.JSONObject
import org.wiicompiled.quest.BuildConfig
import org.wiicompiled.quest.GameLibrary
import org.wiicompiled.quest.GameStorage

/**
 * Builds the game on the headset from the player's extracted disc, the way the PC launcher's
 * Build for Quest does (android/Build-QuestGame.ps1), for players without a PC:
 *
 *  1. unpack the toolchain (assets/quest_toolchain, android/Prepare-QuestToolchain.ps1) and the
 *     game kit (assets/game_kit) into private storage;
 *  2. download the Android NDK files the build needs from Google, checked against the pins the
 *     toolchain carries (ndk.json);
 *  3. translate main.dol and StaticR.rel: translate-recursive, generate-data-init, emit-build-shards;
 *  4. compile the generated sources with the kit's flags, several at a time;
 *  5. link them with the kit's objects and archives (kit.json link.lld);
 *  6. install libmain.so and its game.json like an imported game.
 *
 * About half an hour on a Quest 3. Work survives a cancelled or failed run: a translation whose
 * inputs have not changed is reused and finished objects are not compiled again. A successful
 * build removes everything it unpacked and generated.
 */
object GameBuild {

    enum class Step { Prepare, Download, Translate, Compile, Link, Install }

    /** [permille] of the whole build; [done] of [total] items in [step]. False stops the build. */
    fun interface Reporter {
        fun update(permille: Int, step: Step, done: Int, total: Int): Boolean
    }

    private const val TAG = "WiiCompiledLauncher"
    private const val TOOLCHAIN_ASSETS = "quest_toolchain"
    private const val KIT_ASSETS = "game_kit"
    private const val MARKER = ".complete"
    private const val REQUIRED_SPACE = 2_500_000_000L
    // Translation peaks near 3.1 GB of memory with the runtime's default heap; this limit keeps it
    // near 2.1 GB for the same output, at some cost in time.
    private const val MONO_GC_PARAMS = "soft-heap-limit=1200m"
    private const val TRANSLATOR_THREADS = 4
    private const val EXPECTED_TRANSLATION_SECONDS = 600.0
    private const val COMPILE_MEMORY_BYTES = 700L * 1024 * 1024

    /** Null on success, otherwise the message to show. */
    fun run(context: Context, reporter: Reporter, cancelled: () -> Boolean, finishing: () -> Unit): String? {
        val disc = GameStorage.discDirectory(context)
        if (GameStorage.discStatus(context) != GameStorage.DiscStatus.Ready) {
            return "The game files (DATA) are needed to build the game. Select your disc image first."
        }
        GameFiles.validateData(disc)?.let { return it }
        val root = File(context.filesDir, "build")
        root.mkdirs()
        val available = StatFs(root.absolutePath).availableBytes
        if (available < REQUIRED_SPACE) {
            return "Not enough free space: building needs ${GameFiles.gigabytes(REQUIRED_SPACE)}, and ${GameFiles.gigabytes(available)} is free."
        }

        val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val log = BuildLog(File(GameStorage.logsDirectory(context), "build_$stamp.log"))
        try {
            return Build(context, root, disc, reporter, cancelled, finishing, log).run()
        } catch (e: InterruptedIOException) {
            throw e
        } catch (e: Exception) {
            Log.e(TAG, "Game build failed", e)
            log.line("Build failed: $e")
            return "${e.message ?: e.toString()}\n\nThe build log is ${log.file.absolutePath}"
        } finally {
            log.close()
        }
    }

    private class Build(
        val context: Context,
        val root: File,
        val disc: File,
        val reporter: Reporter,
        val cancelled: () -> Boolean,
        val finishing: () -> Unit,
        val log: BuildLog,
    ) {
        val toolchain = File(root, "toolchain")
        val ndk = File(root, "ndk")
        val kit = File(root, "kit")
        val work = File(root, GameLibrary.directory(context).name)
        val workspace = File(work, "ws")
        val objects = File(work, "obj")
        val temporary = File(root, "tmp")
        val failure = AtomicReference<String?>(null)

        fun run(): String? {
            log.line("WiiCompiled Quest ${BuildConfig.VERSION_NAME} building ${BuildConfig.PROFILE} on this headset")
            report(0, Step.Prepare)
            val toolchainManifest = JSONObject(context.assets.open("$TOOLCHAIN_ASSETS/toolchain.json").reader().use { it.readText() })
            unpackToolchain(toolchainManifest)
            report(30, Step.Prepare)
            val kitFingerprint = GameLibrary.kitFingerprint(context) ?: return "This app carries no game kit."
            unpackKit(kitFingerprint)
            val recipe = JSONObject(File(kit, "kit.json").readText())
            report(40, Step.Prepare)
            downloadNdk(JSONObject(context.assets.open("$TOOLCHAIN_ASSETS/${toolchainManifest.getString("ndk")}").reader().use { it.readText() }))

            temporary.mkdirs()
            val tools = ToolProcess(
                mapOf(
                    "LD_LIBRARY_PATH" to File(toolchain, "llvm/lib").absolutePath,
                    "HOME" to temporary.absolutePath,
                    "TMPDIR" to temporary.absolutePath,
                    "MONO_GC_PARAMS" to MONO_GC_PARAMS,
                ),
                log,
                cancelled = { cancelled() || failure.get() != null },
            )
            translate(tools, "$kitFingerprint ${toolchainManifest.getString("fingerprint")}")?.let { return it }
            val objectsBySlot = compile(tools, recipe, toolchainManifest.getString("fingerprint"))
            failure.get()?.let { return it }
            val library = link(tools, recipe, objectsBySlot) ?: return failure.get() ?: "Linking the game failed."

            finishing()
            report(990, Step.Install)
            install(library, kitFingerprint)?.let { return it }
            log.line("Installed the game built on this headset")
            // Everything above is only needed again after an app update, which changes the kit.
            if (!root.deleteRecursively()) Log.w(TAG, "Could not remove all of ${root.absolutePath}")
            return null
        }

        fun report(permille: Int, step: Step, done: Int = 0, total: Int = 0) {
            if (!reporter.update(permille, step, done, total) || cancelled()) throw InterruptedIOException("Build cancelled")
        }

        /** Unpacks files.zip, which holds exactly the files toolchain.json lists. */
        fun unpackToolchain(manifest: JSONObject) {
            val fingerprint = manifest.getString("fingerprint")
            if (File(toolchain, MARKER).takeIf { it.isFile }?.readText() == fingerprint) return
            toolchain.deleteRecursively()
            val listed = manifest.getJSONArray("files")
            val sizes = HashMap<String, Long>()
            for (i in 0 until listed.length()) {
                val file = listed.getJSONObject(i)
                sizes[file.getString("path")] = file.getLong("size")
            }
            var unpacked = 0
            ZipInputStream(context.assets.open("$TOOLCHAIN_ASSETS/${manifest.getString("archive")}").buffered(1 shl 20)).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (entry.isDirectory) continue
                    val expected = sizes[entry.name] ?: throw IOException("The app's toolchain holds an unlisted file ${entry.name}.")
                    val target = File(toolchain, entry.name)
                    target.parentFile?.mkdirs()
                    target.outputStream().use { zip.copyTo(it, 1 shl 20) }
                    if (target.length() != expected) throw IOException("The app's toolchain file ${entry.name} is damaged.")
                    unpacked++
                    report(30 * unpacked / sizes.size, Step.Prepare)
                }
            }
            if (unpacked != sizes.size) throw IOException("The app's toolchain is incomplete.")
            File(toolchain, MARKER).writeText(fingerprint)
        }

        fun unpackKit(fingerprint: String) {
            if (File(kit, MARKER).takeIf { it.isFile }?.readText() == fingerprint) return
            kit.deleteRecursively()
            copyAssetTree(KIT_ASSETS, kit)
            File(kit, MARKER).writeText(fingerprint)
        }

        fun copyAssetTree(asset: String, target: File) {
            val children = context.assets.list(asset).orEmpty()
            if (children.isEmpty()) {
                copyAsset(asset, target)
                return
            }
            for (child in children) copyAssetTree("$asset/$child", File(target, child))
        }

        fun copyAsset(asset: String, target: File) {
            target.parentFile?.mkdirs()
            context.assets.open(asset).use { input -> target.outputStream().use { input.copyTo(it, 1 shl 20) } }
        }

        /** The NDK subset ndk.json lists, from Google's zip, each file checked against its SHA-256. */
        fun downloadNdk(subset: JSONObject) {
            val digest = subset.getString("digest")
            if (File(ndk, MARKER).takeIf { it.isFile }?.readText() == digest) return
            report(40, Step.Download)
            ndk.deleteRecursively()
            val url = subset.getString("url")
            val prefix = subset.getString("prefix")
            val expected = HashMap<String, String>()
            val listed: JSONArray = subset.getJSONArray("files")
            for (i in 0 until listed.length()) {
                val file = listed.getJSONObject(i)
                expected[prefix + file.getString("path")] = file.getString("sha256")
            }
            log.line("Downloading ${expected.size} Android NDK files from $url")
            val zip = RemoteZip(subset.getLong("size")) { start, end -> fetchRange(url, start, end) }
            val wanted = zip.entries().filter { it.name in expected }
            if (wanted.size != expected.size) throw IOException("Google's Android NDK download no longer has the expected files.")
            val totalBytes = wanted.sumOf { it.end - it.offset }
            var doneBytes = 0L
            val actual = HashMap<String, String>()
            zip.read(wanted) { entry, bytes ->
                val sha256 = BuildRecipe.hex(MessageDigest.getInstance("SHA-256").digest(bytes))
                if (sha256 != expected[entry.name]) throw IOException("The downloaded Android NDK file ${entry.name} is damaged. Try again.")
                val path = entry.name.removePrefix(prefix)
                File(ndk, path).apply { parentFile?.mkdirs() }.writeBytes(bytes)
                actual[path] = sha256
                doneBytes += entry.end - entry.offset
                report(40 + (10 * doneBytes / totalBytes).toInt(), Step.Download, (doneBytes shr 20).toInt(), (totalBytes shr 20).toInt())
            }
            if (BuildRecipe.listDigest(actual) != digest) throw IOException("The downloaded Android NDK files do not match this app's pins.")
            File(ndk, MARKER).writeText(digest)
        }

        fun fetchRange(url: String, start: Long, end: Long): ByteArray {
            if (cancelled()) throw InterruptedIOException("Build cancelled")
            val connection = URL(url).openConnection() as HttpURLConnection
            try {
                connection.connectTimeout = 30_000
                connection.readTimeout = 60_000
                connection.setRequestProperty("Range", "bytes=$start-$end")
                // Android asks for gzip by default, and Google's server then serves a gzip-encoded
                // zip whose ranges are not the file's (HTTP 416 near the end).
                connection.setRequestProperty("Accept-Encoding", "identity")
                val code = try {
                    connection.responseCode
                } catch (e: IOException) {
                    throw IOException("The Android NDK files could not be downloaded from Google. Check the headset's internet connection.", e)
                }
                if (code != HttpURLConnection.HTTP_PARTIAL) throw IOException("Google's server answered $code to the Android NDK download.")
                val bytes = connection.inputStream.use { it.readBytes() }
                if (bytes.size.toLong() != end - start + 1) throw IOException("The Android NDK download was cut short. Try again.")
                return bytes
            } finally {
                connection.disconnect()
            }
        }

        /** Translates the disc unless this workspace already holds a translation of the same inputs. */
        fun translate(tools: ToolProcess, identity: String): String? {
            val generated = File(workspace, "generated")
            val provenance = File(generated, "translation-provenance.txt")
            val expected = "$identity ${BuildConfig.DISC_DOL_SHA256} ${BuildConfig.DISC_REL_SHA256}"
            val shards = File(generated, "build_shards/shards.cmake")
            if (provenance.isFile && provenance.readText() == expected && shards.isFile) {
                log.line("Reusing the translation of an earlier build")
                report(400, Step.Translate)
                return null
            }
            provenance.delete()
            val project = File(workspace, "projects/mkwii")
            File(kit, "translation/projects/mkwii").copyRecursively(project, overwrite = true)
            File(workspace, "runtime").deleteRecursively()
            File(kit, "translation/runtime").copyRecursively(File(workspace, "runtime"), overwrite = true)
            File(disc, DiscChecks.DOL_PATH).copyTo(File(workspace, "Assets/main.dol"), overwrite = true)
            File(disc, DiscChecks.REL_PATH).copyTo(File(workspace, "Assets/StaticR.rel"), overwrite = true)
            val manifest = "projects/mkwii/recomp.yml"
            val entryPoint = BuildRecipe.entryPoint(File(workspace, manifest).readText())

            report(50, Step.Translate, 1, 3)
            val timing = Regex("""\(t=([0-9.]+)s\)""")
            translator(
                tools, "translating the game",
                "translate-recursive", entryPoint, "--project", manifest, "--outdir", "generated/functions",
                "--output-metadata", "generated/base_translation_output.json",
                "--production-source-bundle", "generated/base_translation_sources.bin",
                "--no-function-files", "--prune-stale", "--threads", TRANSLATOR_THREADS.toString(),
            ) { line ->
                timing.find(line)?.groupValues?.get(1)?.toDoubleOrNull()?.let { seconds ->
                    val fraction = minOf(seconds / EXPECTED_TRANSLATION_SECONDS, 0.97)
                    reporter.update(50 + (320 * fraction).toInt(), Step.Translate, 1, 3)
                }
            }?.let { return it }
            report(370, Step.Translate, 2, 3)
            translator(tools, "generating the game data", "generate-data-init", "--project", manifest, "--target-os", "android")?.let { return it }
            report(385, Step.Translate, 3, 3)
            translator(
                tools, "preparing the build",
                "emit-build-shards", "--project", manifest, "--base-metadata", "generated/base_translation_output.json",
                "--base-functions-dir", "generated/functions", "--native-source-dir", "runtime/src", "--out", "generated/build_shards",
            )?.let { return it }
            provenance.writeText(expected)
            report(400, Step.Translate, 3, 3)
            return null
        }

        fun translator(tools: ToolProcess, what: String, vararg arguments: String, onLine: (String) -> Unit = {}): String? {
            log.line("== translator ${arguments.first()}")
            val result = tools.run(
                workspace,
                File(toolchain, "translator/translator_host"),
                listOf(File(toolchain, "translator").absolutePath, "Translator.Cli") + arguments,
                onLine,
            )
            if (result.exitCode == 0) return null
            return if (result.killed) {
                "The headset ran out of memory while $what. Close other apps, then build again."
            } else {
                "The translator failed while $what (exit ${result.exitCode}).\n${result.tail.takeLast(6).joinToString("\n")}"
            }
        }

        /** Compiles every generated source; returns their objects by link slot (runtime, product, translated). */
        fun compile(tools: ToolProcess, recipe: JSONObject, toolchainFingerprint: String): Map<String, List<String>> {
            val generated = File(workspace, "generated")
            val shards = File(generated, "build_shards/shards.cmake").readText()
            val stamp = File(objects, ".stamp")
            val identity = "${recipe.getString("fingerprint")} $toolchainFingerprint"
            if (stamp.takeIf { it.isFile }?.readText() != identity) {
                objects.deleteRecursively()
                objects.mkdirs()
                stamp.writeText(identity)
            }

            val blob = File(generated, "data_sections_init_blobs.S")
            val elfBlob = File(work, "data_sections_init_blobs_android.S")
            val blobText = blob.readText()
            val elfText = BuildRecipe.elfBlobAssembly(blobText)
            if (!elfBlob.isFile || elfBlob.readText() != elfText) elfBlob.writeText(elfText)

            data class Job(val kind: String, val source: File, val slot: String)
            val jobs = ArrayList<Job>()
            jobs += Job("runtime", File(generated, "data_sections_init.cpp"), "runtime")
            jobs += Job("runtime", File(generated, "guest_symbol_table.cpp"), "runtime")
            jobs += Job("asm", elfBlob, "runtime")
            BuildRecipe.sourceList(shards, BuildRecipe.REGISTRATION_LIST).forEach { jobs += Job("product", File(it), "product") }
            val translated = BuildRecipe.TRANSLATED_LISTS.flatMap { BuildRecipe.sourceList(shards, it) }
            if (translated.isEmpty()) throw IOException("The translation produced no game code.")
            translated.forEach { jobs += Job("translated", File(it), "translated") }
            if (jobs.map { BuildRecipe.objectName(it.source.path) }.toSet().size != jobs.size) {
                throw IOException("Two generated sources share an object name.")
            }

            val values = mapOf(
                "kit" to kit.absolutePath,
                "sysroot" to File(ndk, "sysroot").absolutePath,
                "workspace" to workspace.absolutePath,
            )
            val compileFlags = recipe.getJSONObject("compile")
            for (kind in listOf("runtime", "asm", "product", "translated")) {
                val flags = compileFlags.getJSONArray(kind).let { array -> (0 until array.length()).map { BuildRecipe.expand(array.getString(it), values) } }
                File(work, "$kind.rsp").writeText(BuildRecipe.responseFile(flags))
            }

            val clang = File(toolchain, "llvm/bin/clang-21")
            val resourceDir = File(toolchain, "llvm/lib/clang/21").absolutePath
            val pending = jobs.filter { job ->
                val obj = File(objects, BuildRecipe.objectName(job.source.path))
                !(obj.isFile && obj.length() > 0 && obj.lastModified() >= job.source.lastModified())
            }
            val finished = AtomicInteger(jobs.size - pending.size)
            log.line("== compiling ${pending.size} of ${jobs.size} sources")
            val pool = Executors.newFixedThreadPool(compileJobs())
            try {
                val futures: List<Future<*>> = pending.map { job ->
                    pool.submit {
                        if (failure.get() != null || cancelled()) return@submit
                        val obj = File(objects, BuildRecipe.objectName(job.source.path))
                        val partial = File(objects, obj.name + ".partial")
                        // The blob assembly needs no preprocessing, and preprocessing would make clang
                        // start itself again, which Android does not allow here.
                        val language = if (job.kind == "asm") listOf("--driver-mode=gcc", "-x", "assembler") else listOf("--driver-mode=g++")
                        val result = tools.run(
                            work, clang,
                            language + listOf(
                                "-resource-dir", resourceDir, "@" + File(work, "${job.kind}.rsp").absolutePath,
                                "-c", job.source.absolutePath, "-o", partial.absolutePath,
                            ),
                        )
                        if (result.exitCode != 0) {
                            partial.delete()
                            failure.compareAndSet(
                                null,
                                if (result.killed) "The headset ran out of memory while compiling the game. Close other apps, then build again."
                                else "Compiling ${job.source.name} failed.\n${result.tail.takeLast(6).joinToString("\n")}",
                            )
                            return@submit
                        }
                        if (!partial.renameTo(obj)) failure.compareAndSet(null, "Could not store ${obj.name}.")
                        finished.incrementAndGet()
                    }
                }
                while (futures.any { !it.isDone }) {
                    report(400 + 570 * finished.get() / jobs.size, Step.Compile, finished.get(), jobs.size)
                    TimeUnit.MILLISECONDS.sleep(500)
                }
                futures.forEach { future ->
                    try {
                        future.get()
                    } catch (e: java.util.concurrent.ExecutionException) {
                        val cause = e.cause
                        // A failed compile stops the others, which is not the player cancelling.
                        if (cause is InterruptedIOException && failure.get() == null) throw cause
                        failure.compareAndSet(null, cause?.message ?: cause.toString())
                    }
                }
            } finally {
                pool.shutdownNow()
            }
            if (failure.get() == null) report(970, Step.Compile, jobs.size, jobs.size)
            return jobs.groupBy({ it.slot }, { File(objects, BuildRecipe.objectName(it.source.path)).absolutePath })
        }

        fun compileJobs(): Int {
            val memory = ActivityManager.MemoryInfo()
            context.getSystemService(ActivityManager::class.java)?.getMemoryInfo(memory)
            val byMemory = (memory.availMem / COMPILE_MEMORY_BYTES).toInt()
            return minOf(4, Runtime.getRuntime().availableProcessors(), byMemory).coerceAtLeast(1)
        }

        fun link(tools: ToolProcess, recipe: JSONObject, objectsBySlot: Map<String, List<String>>): File? {
            report(970, Step.Link)
            val library = File(work, recipe.getString("output"))
            library.delete()
            val template = recipe.getJSONObject("link").getJSONArray("lld").let { array -> (0 until array.length()).map { array.getString(it) } }
            val arguments = BuildRecipe.linkArguments(
                template,
                mapOf("kit" to kit.absolutePath, "ndk" to ndk.absolutePath, "output" to library.absolutePath),
                objectsBySlot,
            )
            val rsp = File(work, "link.rsp").apply { writeText(BuildRecipe.responseFile(arguments)) }
            log.line("== linking")
            val result = tools.run(work, File(toolchain, "llvm/bin/ld.lld"), listOf("@" + rsp.absolutePath))
            if (result.exitCode != 0 || !library.isFile) {
                failure.compareAndSet(null, "Linking the game failed.\n${result.tail.takeLast(6).joinToString("\n")}")
                return null
            }
            return library
        }

        fun install(library: File, kitFingerprint: String): String? {
            val destination = GameLibrary.directory(context)
            val staging = File(destination.parentFile, "${destination.name}.building")
            staging.deleteRecursively()
            staging.mkdirs()
            try {
                val installed = File(staging, GameLibrary.LIBRARY_NAME)
                library.copyTo(installed)
                val digest = MessageDigest.getInstance("SHA-256")
                installed.inputStream().use { input ->
                    val buffer = ByteArray(1 shl 20)
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        digest.update(buffer, 0, read)
                    }
                }
                val builtAt = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss'Z'", Locale.US).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date())
                val manifest = JSONObject()
                    .put("schema", 1)
                    .put("profile", BuildConfig.PROFILE)
                    .put("gameId", BuildConfig.DISC_GAME_ID)
                    .put("dolSha256", BuildConfig.DISC_DOL_SHA256)
                    .put("relSha256", BuildConfig.DISC_REL_SHA256)
                    .put("kitFingerprint", kitFingerprint)
                    .put("library", GameLibrary.LIBRARY_NAME)
                    .put("librarySha256", BuildRecipe.hex(digest.digest()))
                    .put("includesData", false)
                    .put("builtBy", "this headset (WiiCompiled Quest ${BuildConfig.VERSION_NAME})")
                    .put("builtAt", builtAt)
                File(staging, GameLibrary.MANIFEST_NAME).writeText(manifest.toString(2))
                destination.parentFile?.mkdirs()
                return GameFiles.replace(destination, staging)
            } finally {
                staging.deleteRecursively()
            }
        }
    }
}
