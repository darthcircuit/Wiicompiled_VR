package org.wiicompiled.quest.launcher

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.net.Uri
import android.os.IBinder
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import kotlin.concurrent.thread
import org.wiicompiled.quest.R

/**
 * Runs a [GameSetup] task as a foreground service. Extracting a disc or importing a game takes
 * minutes and building one half an hour, and a panel app's process is otherwise fair game once the
 * panel is closed; taking the headset off also sleeps the CPU, hence the partial wake lock.
 */
class GameSetupService : Service() {

    private var lastNotified = 0L
    private val listener: (GameSetup.State) -> Unit = { state ->
        val now = SystemClock.elapsedRealtime()
        if (state !is GameSetup.State.Working || now - lastNotified >= NOTIFY_INTERVAL_MS) {
            lastNotified = now
            getSystemService(NotificationManager::class.java)?.notify(NOTIFICATION_ID, notification(state))
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // startForegroundService obliges the service to go foreground even when it has nothing
        // to do, or Android 12+ ends the app.
        createChannel()
        val task = intent?.getStringExtra(EXTRA_TASK)?.let { name -> GameSetup.Task.entries.firstOrNull { it.name == name } }
            ?: GameSetup.Task.ExtractDisc
        startForeground(NOTIFICATION_ID, notification(GameSetup.State.Checking(task)), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        val uri = intent?.data
        if ((uri == null && task != GameSetup.Task.BuildGame) || !GameSetup.begin(task)) {
            // A task already going keeps the service, and ends it itself.
            if (!GameSetup.isRunning) {
                stopForeground(STOP_FOREGROUND_REMOVE)
                stopSelf(startId)
            }
            return START_NOT_STICKY
        }
        GameSetup.addListener(listener)
        val deleteSource = intent?.getBooleanExtra(EXTRA_DELETE_SOURCE, false) ?: false

        val wakeLock = getSystemService(PowerManager::class.java)
            .newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "WiiCompiled:GameSetup")
        wakeLock.acquire(if (task == GameSetup.Task.BuildGame) BUILD_WAKE_LOCK_TIMEOUT_MS else WAKE_LOCK_TIMEOUT_MS)
        thread(name = "GameSetup") {
            try {
                GameSetup.run(applicationContext, task, uri, deleteSource)
            } finally {
                uri?.let(::releaseReadPermission)
                if (wakeLock.isHeld) wakeLock.release()
                mainExecutor.execute {
                    GameSetup.removeListener(listener)
                    stopForeground(STOP_FOREGROUND_REMOVE)
                    stopSelf()
                }
            }
        }
        return START_NOT_STICKY
    }

    private fun releaseReadPermission(uri: Uri) {
        if (uri.scheme != "content") return
        try {
            contentResolver.releasePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: SecurityException) {
            // Never persisted: the provider does not offer persistable grants.
        }
    }

    private fun createChannel() {
        val manager = getSystemService(NotificationManager::class.java) ?: return
        manager.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, getString(R.string.disc_setup_channel), NotificationManager.IMPORTANCE_LOW),
        )
    }

    private fun notification(state: GameSetup.State): Notification {
        val building = state is GameSetup.State.Checking && state.task == GameSetup.Task.BuildGame ||
            state is GameSetup.State.Working && state.task == GameSetup.Task.BuildGame
        val builder = Notification.Builder(this, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_wheel)
            .setContentTitle(getString(if (building) R.string.build_notification_title else R.string.disc_setup_notification_title))
            .setOngoing(true)
            .setOnlyAlertOnce(true)
        if (state is GameSetup.State.Working) {
            val percent = if (state.total > 0) (state.done * 100 / state.total).toInt() else 0
            val text = when (state.task) {
                GameSetup.Task.ImportPackage -> R.string.disc_setup_importing
                GameSetup.Task.BuildGame -> R.string.disc_setup_building
                GameSetup.Task.ExtractDisc -> R.string.disc_setup_extracting
            }
            builder.setContentText(getString(text, percent)).setProgress(100, percent, false)
        } else {
            builder.setContentText(getString(if (building) R.string.build_preparing else R.string.disc_setup_checking)).setProgress(0, 0, true)
        }
        return builder.build()
    }

    companion object {
        private const val TAG = "WiiCompiledLauncher"
        private const val CHANNEL_ID = "disc_setup"
        private const val NOTIFICATION_ID = 1
        private const val NOTIFY_INTERVAL_MS = 1000L
        private const val EXTRA_TASK = "task"
        private const val EXTRA_DELETE_SOURCE = "deleteSource"
        // Longer than any real task; only a hung run would reach it.
        private const val WAKE_LOCK_TIMEOUT_MS = 60L * 60 * 1000
        private const val BUILD_WAKE_LOCK_TIMEOUT_MS = 3L * 60 * 60 * 1000

        /**
         * Starts [task] on [uri]: a document the player picked, or a file in the Import folder,
         * which [deleteSource] removes once it has been imported.
         */
        /** Builds the game from DATA on this headset ([GameBuild]). */
        fun startBuild(context: Context) {
            val intent = Intent(context, GameSetupService::class.java).putExtra(EXTRA_TASK, GameSetup.Task.BuildGame.name)
            context.startForegroundService(intent)
        }

        fun start(context: Context, task: GameSetup.Task, uri: Uri, deleteSource: Boolean = false) {
            if (uri.scheme == "content") {
                try {
                    context.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                } catch (e: SecurityException) {
                    Log.i(TAG, "No persistable read grant for $uri; relying on the picker's grant")
                }
            }
            val intent = Intent(context, GameSetupService::class.java)
                .setData(uri)
                .putExtra(EXTRA_TASK, task.name)
                .putExtra(EXTRA_DELETE_SOURCE, deleteSource)
                .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            context.startForegroundService(intent)
        }
    }
}
