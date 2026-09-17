package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.ActivityManager
import android.app.AlertDialog
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.res.ColorStateList
import android.net.Uri
import android.os.Bundle
import android.text.format.Formatter
import android.util.Log
import android.view.View
import android.widget.ImageView
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import org.wiicompiled.quest.BuildConfig
import org.wiicompiled.quest.GameLibrary
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.QuestActivity
import org.wiicompiled.quest.R

/**
 * The app's entry point on the headset: a 2D panel modelled on the PC launcher (WheelWizard VR),
 * with a Home page that sets up and starts the game and a Settings page that edits Config.toml.
 *
 * The APK carries no game code. Playing needs two things the player owns: the game files (DATA,
 * extracted from their disc image here or on a PC) and the game itself (libmain.so, built from
 * their disc on a PC and brought over with Import from computer). Home's main button is always
 * the next of those steps, and Play once both are there.
 *
 * The game runs as [QuestActivity], an immersive activity in its own `:game` process. SDL and the
 * runtime cannot start twice in one process, so that process ends with every game session, and
 * this panel stays behind to relaunch it.
 */
class LauncherActivity : Activity() {

    private enum class Page { Home, Settings }

    /** What Home's main and secondary buttons do. */
    private enum class Action { Play, Resume, SelectDisc, ImportGame }

    private lateinit var navHome: View
    private lateinit var navSettings: View
    private lateinit var homePage: View
    private lateinit var settingsView: View
    private lateinit var trails: WheelTrailsView
    private lateinit var playButton: View
    private lateinit var playIcon: ImageView
    private lateinit var playText: TextView
    private lateinit var progress: ProgressBar
    private lateinit var homeStatus: TextView
    private lateinit var secondary: TextView
    private lateinit var cancel: View
    private lateinit var dataBanner: View
    private lateinit var dataBannerIcon: ImageView
    private lateinit var dataBannerText: TextView
    private lateinit var settings: SettingsPage

    private var page = Page.Home
    private var launching = false
    private var trailsAway = true
    private var setupKind: Class<*>? = null
    private var mainAction = Action.Play
    private var secondaryAction: Action? = null

    private val setupListener: (GameSetup.State) -> Unit = { state ->
        when {
            page == Page.Home -> refreshHome()
            // Rows only change when a task starts or ends, not with every progress step.
            state.javaClass != setupKind -> settings.refresh()
        }
        setupKind = state.javaClass
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_launcher)
        try {
            GameStorage.prepare(this)
        } catch (e: Exception) {
            Log.w(TAG, "Cannot prepare the game directory", e)
        }

        navHome = findViewById(R.id.nav_home)
        navSettings = findViewById(R.id.nav_settings)
        homePage = findViewById(R.id.page_home)
        settingsView = findViewById(R.id.page_settings)
        trails = findViewById(R.id.home_trails)
        playButton = findViewById(R.id.home_play)
        playIcon = findViewById(R.id.home_play_icon)
        playText = findViewById(R.id.home_play_text)
        progress = findViewById(R.id.home_progress)
        homeStatus = findViewById(R.id.home_status)
        secondary = findViewById(R.id.home_secondary)
        cancel = findViewById(R.id.home_cancel)
        dataBanner = findViewById(R.id.home_data_banner)
        dataBannerIcon = findViewById(R.id.home_data_banner_icon)
        dataBannerText = findViewById(R.id.home_data_banner_text)

        findViewById<TextView>(R.id.home_title).setText(
            if (BuildConfig.PROFILE == "retro_rewind") R.string.home_title_retro_rewind else R.string.home_title_base,
        )
        findViewById<TextView>(R.id.launcher_version).text = getString(R.string.launcher_version, BuildConfig.VERSION_NAME)

        settings = SettingsPage(
            this,
            settingsView,
            ConfigStore(GameStorage.configFile(this)),
            gameRunning = ::isGameRunning,
            selectDiscImage = ::selectDiscImage,
            importGame = ::importGame,
        )
        savedInstanceState?.getString(KEY_TAB)?.let { name ->
            SettingsPage.Tab.entries.firstOrNull { it.name == name }?.let(settings::select)
        }

        navHome.setOnClickListener { showPage(Page.Home) }
        navSettings.setOnClickListener { showPage(Page.Settings) }
        playButton.setOnClickListener { perform(mainAction) }
        secondary.setOnClickListener { secondaryAction?.let(::perform) }
        cancel.setOnClickListener { GameSetup.cancel() }

        val restored = savedInstanceState?.getString(KEY_PAGE)?.let { name -> Page.entries.firstOrNull { it.name == name } }
        showPage(restored ?: Page.Home)
    }

    override fun onResume() {
        super.onResume()
        launching = false
        importDroppedPackage()
        setupKind = GameSetup.state.javaClass
        refresh()
        GameSetup.addListener(setupListener)
        // The game's process can take a moment to go away after its activity
        // closes, which would still read as running.
        val runningAtResume = isGameRunning()
        window.decorView.postDelayed({
            if (!isFinishing && isGameRunning() != runningAtResume) refresh()
        }, PROCESS_EXIT_GRACE_MS)
    }

    override fun onPause() {
        GameSetup.removeListener(setupListener)
        super.onPause()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putString(KEY_PAGE, page.name)
        outState.putString(KEY_TAB, settings.tab.name)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        val uri = data?.data ?: return
        if (resultCode != RESULT_OK) return
        val task = when (requestCode) {
            REQUEST_DISC_IMAGE -> GameSetup.Task.ExtractDisc
            REQUEST_GAME_PACKAGE -> GameSetup.Task.ImportPackage
            else -> return
        }
        GameSetupService.start(this, task, uri)
        // Progress and the outcome are shown on Home, wherever the picker was opened from.
        showPage(Page.Home)
    }

    private fun showPage(target: Page) {
        page = target
        navHome.isSelected = target == Page.Home
        navSettings.isSelected = target == Page.Settings
        homePage.visibility = if (target == Page.Home) View.VISIBLE else View.GONE
        settingsView.visibility = if (target == Page.Settings) View.VISIBLE else View.GONE
        if (target == Page.Home) {
            trailsAway = true
        }
        refresh()
    }

    private fun refresh() {
        when (page) {
            Page.Home -> refreshHome()
            Page.Settings -> settings.refresh()
        }
    }

    private fun refreshHome() {
        val disc = GameStorage.discDirectory(this).absolutePath
        val discStatus = GameStorage.discStatus(this)
        val gameStatus = GameLibrary.status(this)
        val running = isGameRunning()
        val setup = GameSetup.state
        val settingUp = GameSetup.isRunning
        val importing = setupTask(setup) == GameSetup.Task.ImportPackage

        mainAction = when {
            running -> Action.Resume
            gameStatus != GameLibrary.Status.Ready -> Action.ImportGame
            discStatus != GameStorage.DiscStatus.Ready -> Action.SelectDisc
            else -> Action.Play
        }
        // Without anything yet, a player with only a headset starts from their disc image.
        secondaryAction = when {
            settingUp || running -> null
            mainAction == Action.ImportGame && discStatus != GameStorage.DiscStatus.Ready -> Action.SelectDisc
            mainAction == Action.SelectDisc -> Action.ImportGame
            else -> null
        }

        playButton.isEnabled = !settingUp
        playIcon.setImageResource(if (!settingUp && (mainAction == Action.Play || mainAction == Action.Resume)) R.drawable.ic_play else R.drawable.ic_disc)
        playText.text = when {
            setup is GameSetup.State.Checking -> getString(if (importing) R.string.home_checking_package else R.string.home_checking)
            setup is GameSetup.State.Working -> getString(if (importing) R.string.home_importing else R.string.home_extracting, percent(setup))
            setup is GameSetup.State.Finishing -> getString(R.string.home_finishing)
            else -> getString(label(mainAction))
        }
        secondary.visibility = if (secondaryAction != null) View.VISIBLE else View.GONE
        secondaryAction?.let { secondary.setText(label(it)) }

        progress.visibility = if (settingUp) View.VISIBLE else View.GONE
        progress.isIndeterminate = setup !is GameSetup.State.Working
        if (setup is GameSetup.State.Working && setup.total > 0) {
            progress.progress = (setup.done * progress.max / setup.total).toInt()
        }
        cancel.visibility = if (settingUp && setup !is GameSetup.State.Finishing) View.VISIBLE else View.GONE

        homeStatus.text = when {
            setup is GameSetup.State.Working -> getString(
                R.string.home_extract_progress,
                Formatter.formatShortFileSize(this, setup.done),
                Formatter.formatShortFileSize(this, setup.total),
            )
            setup is GameSetup.State.Checking -> getString(if (importing) R.string.home_checking_package_status else R.string.home_checking_status)
            settingUp || setup is GameSetup.State.Failed -> ""
            setup is GameSetup.State.Cancelled -> getString(R.string.home_setup_cancelled)
            running -> getString(R.string.home_running)
            mainAction == Action.Play && setup is GameSetup.State.Done -> getString(R.string.home_setup_done)
            mainAction == Action.Play -> getString(R.string.home_put_on_headset)
            else -> ""
        }

        when {
            settingUp -> showBanner(null)
            setup is GameSetup.State.Failed -> showBanner(getString(R.string.home_setup_failed, setup.message), warning = true)
            discStatus == GameStorage.DiscStatus.Incomplete -> showBanner(getString(R.string.home_data_incomplete, disc), warning = true)
            gameStatus == GameLibrary.Status.Stale -> showBanner(getString(R.string.home_game_stale), warning = true)
            gameStatus == GameLibrary.Status.Missing && discStatus == GameStorage.DiscStatus.Missing ->
                showBanner(getString(R.string.home_setup_intro), warning = false)
            gameStatus == GameLibrary.Status.Missing -> showBanner(getString(R.string.home_game_missing), warning = false)
            discStatus == GameStorage.DiscStatus.Missing -> showBanner(getString(R.string.home_data_missing, disc), warning = false)
            else -> showBanner(null)
        }

        if (trailsAway && !launching) {
            trailsAway = false
            trails.enter()
        }
    }

    private fun label(action: Action): Int = when (action) {
        Action.Play -> R.string.home_play
        Action.Resume -> R.string.home_resume
        Action.SelectDisc -> R.string.home_select_disc
        Action.ImportGame -> R.string.home_import
    }

    private fun perform(action: Action) {
        if (GameSetup.isRunning) return
        when (action) {
            Action.Play, Action.Resume -> play()
            Action.SelectDisc -> selectDiscImage()
            Action.ImportGame -> importGame()
        }
    }

    private fun showBanner(text: String?, warning: Boolean = false) {
        if (text == null) {
            dataBanner.visibility = View.GONE
            return
        }
        dataBannerText.text = text
        dataBanner.setBackgroundResource(if (warning) R.drawable.bg_banner_warning else R.drawable.bg_banner_info)
        dataBannerIcon.setImageResource(if (warning) R.drawable.ic_warning else R.drawable.ic_disc)
        dataBannerIcon.imageTintList = ColorStateList.valueOf(getColor(if (warning) R.color.warning_500 else R.color.primary_400))
        dataBanner.visibility = View.VISIBLE
    }

    /** Opens the document picker for a disc image, asking first when it would replace DATA. */
    private fun selectDiscImage() {
        if (GameSetup.isRunning) return
        if (GameStorage.discStatus(this) == GameStorage.DiscStatus.Missing) {
            openPicker(REQUEST_DISC_IMAGE)
            return
        }
        confirm(R.string.home_replace_title, R.string.home_replace_message, R.string.home_select_disc) {
            openPicker(REQUEST_DISC_IMAGE)
        }
    }

    /** Opens the document picker for a .wcgame, asking first when it would replace the game. */
    private fun importGame() {
        if (GameSetup.isRunning) return
        if (GameLibrary.status(this) != GameLibrary.Status.Ready) {
            openPicker(REQUEST_GAME_PACKAGE)
            return
        }
        confirm(R.string.home_replace_game_title, R.string.home_replace_game_message, R.string.home_import) {
            openPicker(REQUEST_GAME_PACKAGE)
        }
    }

    private fun confirm(title: Int, message: Int, positive: Int, onConfirm: () -> Unit) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setMessage(message)
            .setPositiveButton(positive) { _, _ -> onConfirm() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun openPicker(requestCode: Int) {
        // Disc images and game files have no reliable MIME type, so every file is offered
        // and the task checks the name and contents.
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT)
            .addCategory(Intent.CATEGORY_OPENABLE)
            .setType("*/*")
        try {
            @Suppress("DEPRECATION")
            startActivityForResult(intent, requestCode)
        } catch (e: ActivityNotFoundException) {
            Log.w(TAG, "No document picker", e)
            Toast.makeText(this, R.string.home_no_picker, Toast.LENGTH_LONG).show()
        }
    }

    /**
     * Imports the newest .wcgame in the Import folder, where Build-QuestGame.ps1 -Install and adb
     * put them. Each package is tried once, whether it imports or not, until it changes: a file
     * adb pushed belongs to the shell user, so the app cannot always delete it afterwards.
     */
    private fun importDroppedPackage() {
        if (GameSetup.isRunning) return
        val dropped = GameStorage.importDirectory(this)
            .listFiles { file -> file.isFile && file.name.endsWith(".wcgame", ignoreCase = true) }
            ?.maxByOrNull { it.lastModified() }
            ?: return
        val identity = "${dropped.absolutePath}:${dropped.length()}:${dropped.lastModified()}"
        val preferences = getSharedPreferences(PREFERENCES, MODE_PRIVATE)
        if (preferences.getString(KEY_LAST_DROPPED_IMPORT, null) == identity) return
        preferences.edit().putString(KEY_LAST_DROPPED_IMPORT, identity).apply()
        Log.i(TAG, "Importing dropped game package ${dropped.absolutePath}")
        GameSetupService.start(this, GameSetup.Task.ImportPackage, Uri.fromFile(dropped), deleteSource = true)
    }

    private fun play() {
        if (launching) {
            return
        }
        launching = true
        trailsAway = true
        trails.leave {
            try {
                startActivity(Intent(this, QuestActivity::class.java))
            } catch (e: ActivityNotFoundException) {
                Log.e(TAG, "Cannot start the game activity", e)
                Toast.makeText(this, R.string.home_launch_failed, Toast.LENGTH_LONG).show()
                launching = false
                refreshHome()
            }
        }
    }

    private fun isGameRunning(): Boolean {
        val manager = getSystemService(ActivityManager::class.java) ?: return false
        val gameProcess = "$packageName$GAME_PROCESS_SUFFIX"
        return manager.runningAppProcesses.orEmpty().any { it.processName == gameProcess }
    }

    private fun setupTask(state: GameSetup.State): GameSetup.Task? = when (state) {
        is GameSetup.State.Checking -> state.task
        is GameSetup.State.Working -> state.task
        is GameSetup.State.Finishing -> state.task
        is GameSetup.State.Done -> state.task
        is GameSetup.State.Failed -> state.task
        is GameSetup.State.Cancelled -> state.task
        GameSetup.State.Idle -> null
    }

    private fun percent(state: GameSetup.State.Working): Int =
        if (state.total > 0) (state.done * 100 / state.total).toInt() else 0

    private companion object {
        const val TAG = "WiiCompiledLauncher"
        // Must match QuestActivity's android:process in AndroidManifest.xml.
        const val GAME_PROCESS_SUFFIX = ":game"
        const val KEY_PAGE = "page"
        const val KEY_TAB = "settingsTab"
        const val PROCESS_EXIT_GRACE_MS = 1000L
        const val REQUEST_DISC_IMAGE = 1
        const val REQUEST_GAME_PACKAGE = 2
        const val PREFERENCES = "launcher"
        const val KEY_LAST_DROPPED_IMPORT = "lastDroppedImport"
    }
}
