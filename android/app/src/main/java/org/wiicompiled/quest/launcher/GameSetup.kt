package org.wiicompiled.quest.launcher

import android.content.Context
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.File
import java.io.InterruptedIOException

/**
 * The long-running steps that make the game playable from what the player owns, one at a time,
 * process-wide. [GameSetupService] runs them in the foreground and the launcher watches [state].
 *
 *  - [Task.ExtractDisc]: the player's disc image becomes DATA ([DiscExtraction]).
 *  - [Task.ImportPackage]: a .wcgame built on a PC becomes the game library, and DATA too when
 *    the package carries it ([GamePackageImport]).
 *
 * Every task stages its output next to the destination and swaps it in only after it has been
 * checked, so a failed, cancelled or killed run never costs working files.
 */
object GameSetup {

    enum class Task { ExtractDisc, ImportPackage }

    sealed interface State {
        data object Idle : State
        data class Checking(val task: Task) : State
        data class Working(val task: Task, val done: Long, val total: Long) : State
        data class Finishing(val task: Task) : State
        data class Done(val task: Task) : State
        data class Failed(val task: Task, val message: String) : State
        data class Cancelled(val task: Task) : State
    }

    /** What a task step reports; returning false from [update] asks it to stop. */
    fun interface Progress {
        fun update(done: Long, total: Long): Boolean
    }

    private const val TAG = "WiiCompiledLauncher"

    private val main = Handler(Looper.getMainLooper())
    private val listeners = mutableSetOf<(State) -> Unit>()

    @Volatile
    var state: State = State.Idle
        private set

    @Volatile
    private var cancelRequested = false

    val isRunning: Boolean
        get() = state is State.Checking || state is State.Working || state is State.Finishing

    /** Listeners are called on the main thread, with the current state first. */
    fun addListener(listener: (State) -> Unit) {
        listeners += listener
        listener(state)
    }

    fun removeListener(listener: (State) -> Unit) {
        listeners -= listener
    }

    fun cancel() {
        cancelRequested = true
    }

    /** Claims the single slot for [task]; false when another task is running. */
    fun begin(task: Task): Boolean {
        synchronized(this) {
            if (isRunning) return false
            cancelRequested = false
            publish(State.Checking(task))
            return true
        }
    }

    /** The whole task, on a worker thread. Always ends in Done, Failed or Cancelled. */
    fun run(context: Context, task: Task, uri: Uri, deleteSource: Boolean) {
        val progress = Progress { done, total ->
            publish(State.Working(task, done, total))
            !cancelRequested
        }
        val finishing = { publish(State.Finishing(task)) }
        try {
            val failure = context.contentResolver.openFileDescriptor(uri, "r")?.use { descriptor ->
                when (task) {
                    Task.ExtractDisc -> DiscExtraction.run(context, descriptor.fd, displayName(context, uri), progress, finishing)
                    Task.ImportPackage -> GamePackageImport.run(context, descriptor, progress, finishing)
                }
            } ?: "The selected file could not be opened."
            if (failure != null) {
                publish(State.Failed(task, failure))
                return
            }
            if (deleteSource && uri.scheme == "file") {
                uri.path?.let { File(it).delete() }
            }
            publish(State.Done(task))
        } catch (e: InterruptedIOException) {
            publish(State.Cancelled(task))
        } catch (e: Exception) {
            Log.e(TAG, "$task failed", e)
            publish(State.Failed(task, e.message ?: e.toString()))
        }
    }

    fun displayName(context: Context, uri: Uri): String? =
        runCatching {
            context.contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                if (cursor.moveToFirst()) cursor.getString(0) else null
            }
        }.getOrNull() ?: uri.lastPathSegment

    private fun publish(next: State) {
        state = next
        main.post {
            for (listener in listeners.toList()) listener(next)
        }
    }
}
