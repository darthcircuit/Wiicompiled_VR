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
import android.text.method.ScrollingMovementMethod
import android.util.Log
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import org.wiicompiled.quest.BuildConfig
import org.wiicompiled.quest.GameLibrary
import org.wiicompiled.quest.GameProfile
import org.wiicompiled.quest.GameStorage
import org.wiicompiled.quest.QuestActivity
import org.wiicompiled.quest.R

/**
 * The app's entry point on the headset: a 2D panel modelled on the PC launcher (WheelWizard VR),
 * with a Home page that sets up and starts the game and a Settings page that edits Config.toml.
 *
 * The APK carries no game code. Playing needs two things the player owns: the game files (DATA,
 * extracted from their disc image here or on a PC) and the game itself (libmain.so, built from
 * their disc on this headset, or on a PC and brought over with Import from computer). Home's main
 * button is always the next of those steps, and Play once both are there.
 *
 * The game runs as [QuestActivity], an immersive activity in its own `:game` process. SDL and the
 * runtime cannot start twice in one process, so that process ends with every game session, and
 * this panel stays behind to relaunch it.
 */
class LauncherActivity : Activity() {

    private enum class Page { Home, Settings }

    /** What Home's main and secondary buttons do. */
    private enum class Action { Play, Resume, SelectDisc, ImportGame, BuildGame, DownloadModPack }

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
    private lateinit var homeTitle: TextView
    private lateinit var gameToggle: LinearLayout
    private lateinit var settings: SettingsPage

    /** The games this APK carries a kit for, and the one the player picked. */
    private val profiles: List<GameProfile> by lazy { GameProfile.available(this) }
    private var profile = GameProfile.Base

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
        // The banner floats over the page, so a long message scrolls inside it rather than growing
        // over the buttons underneath.
        dataBannerText.movementMethod = ScrollingMovementMethod()

        homeTitle = findViewById(R.id.home_title)
        gameToggle = findViewById(R.id.home_game_toggle)
        buildGameToggle()
        findViewById<TextView>(R.id.launcher_version).text = getString(R.string.launcher_version, BuildConfig.VERSION_NAME)

        settings = SettingsPage(
            this,
            settingsView,
            ConfigStore(GameStorage.configFile(this)),
            gameRunning = ::isGameRunning,
            selectDiscImage = ::selectDiscImage,
            importGame = ::importGame,
            buildGame = ::buildGame,
            downloadModPack = ::downloadModPack,
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

        // Unattended headset tests (docs/quest-port.md): debug builds start a build from adb.
        if (BuildConfig.DEBUG && savedInstanceState == null && intent.getBooleanExtra(EXTRA_DEBUG_BUILD_GAME, false) &&
            !GameSetup.isRunning && GameStorage.discStatus(this) == GameStorage.DiscStatus.Ready
        ) {
            Log.i(TAG, "Starting a game build requested over adb")
            GameSetupService.startBuild(this, GameProfile.selected(this))
        }
    }

    override fun onResume() {
        super.onResume()
        launching = false
        // An import can install the other game and select it, so the toggle follows the file.
        profile = GameProfile.selected(this)
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
        val gameStatus = GameLibrary.status(this, profile)
        homeTitle.setText(profile.title)
        for ((index, entry) in profiles.withIndex()) {
            gameToggle.getChildAt(index).isSelected = entry == profile
        }
        val running = isGameRunning()
        val setup = GameSetup.state
        val settingUp = GameSetup.isRunning
        val task = setupTask(setup)
        val importing = task == GameSetup.Task.ImportPackage
        val building = task == GameSetup.Task.BuildGame
        val downloadingPack = task == GameSetup.Task.DownloadModPack

        // Without anything yet, a player with only a headset starts from their disc image, and
        // builds the game once its files are there; a PC-built game can always be imported instead.
        // Retro Rewind also needs its own pack, which neither building nor playing can do without.
        mainAction = when {
            running -> Action.Resume
            discStatus != GameStorage.DiscStatus.Ready -> Action.SelectDisc
            !GameStorage.modContentReady(this, profile) -> Action.DownloadModPack
            gameStatus != GameLibrary.Status.Ready -> Action.BuildGame
            else -> Action.Play
        }
        secondaryAction = when {
            settingUp || running -> null
            mainAction == Action.ImportGame -> null
            gameStatus != GameLibrary.Status.Ready || mainAction == Action.SelectDisc -> Action.ImportGame
            else -> null
        }

        playButton.isEnabled = !settingUp
        playIcon.setImageResource(if (!settingUp && (mainAction == Action.Play || mainAction == Action.Resume)) R.drawable.ic_play else R.drawable.ic_disc)
        playText.text = when {
            setup is GameSetup.State.Checking -> getString(
                when {
                    building -> R.string.home_build_preparing
                    importing -> R.string.home_checking_package
                    else -> R.string.home_checking
                },
            )
            setup is GameSetup.State.Working -> getString(
                when {
                    building -> R.string.home_building
                    importing -> R.string.home_importing
                    downloadingPack -> R.string.home_mod_pack_downloading
                    else -> R.string.home_extracting
                },
                percent(setup),
            )
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
            setup is GameSetup.State.Working && building -> buildStatus(setup)
            setup is GameSetup.State.Working -> getString(
                R.string.home_extract_progress,
                Formatter.formatShortFileSize(this, setup.done),
                Formatter.formatShortFileSize(this, setup.total),
            )
            setup is GameSetup.State.Checking -> getString(
                when {
                    building -> R.string.build_step_prepare
                    importing -> R.string.home_checking_package_status
                    else -> R.string.home_checking_status
                },
            )
            settingUp || setup is GameSetup.State.Failed -> ""
            setup is GameSetup.State.Cancelled -> getString(if (building) R.string.home_build_cancelled else R.string.home_setup_cancelled)
            running -> getString(R.string.home_running)
            mainAction == Action.Play && setup is GameSetup.State.Done -> getString(if (building) R.string.home_build_done else R.string.home_setup_done)
            mainAction == Action.Play -> getString(R.string.home_put_on_headset)
            else -> ""
        }

        when {
            settingUp && building -> showBanner(getString(R.string.home_build_running), warning = false)
            settingUp && downloadingPack -> showBanner(getString(R.string.home_mod_pack_running), warning = false)
            settingUp -> showBanner(null)
            setup is GameSetup.State.Failed && building -> showBanner(getString(R.string.home_build_failed, setup.message), warning = true)
            setup is GameSetup.State.Failed && downloadingPack -> showBanner(getString(R.string.home_mod_pack_failed, setup.message), warning = true)
            setup is GameSetup.State.Failed -> showBanner(getString(R.string.home_setup_failed, setup.message), warning = true)
            discStatus == GameStorage.DiscStatus.Incomplete -> showBanner(getString(R.string.home_data_incomplete, disc), warning = true)
            // Only once the disc files are there is the pack the next thing missing; before that a
            // first-time player is still reading how to get those.
            discStatus == GameStorage.DiscStatus.Ready && !GameStorage.modContentReady(this, profile) ->
                showBanner(getString(R.string.home_mod_needed_message), warning = true)
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

    /**
     * Home's game switch, shown only when this APK carries more than one kit. Each game keeps its
     * own built library, so switching is immediate; only the panel's state changes.
     */
    private fun buildGameToggle() {
        for (entry in profiles) {
            val button = TextView(this).apply {
                setText(entry.title)
                textSize = 14f
                setTextColor(getColorStateList(R.color.game_toggle_text))
                setBackgroundResource(R.drawable.bg_game_toggle)
                gravity = android.view.Gravity.CENTER
                // One line: the pill has a fixed height, so a wrapped "Retro Rewind" loses half of itself.
                isSingleLine = true
                setPadding(10.dp(), 0, 10.dp(), 0)
                isClickable = true
                isFocusable = true
                layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, 38.dp())
                setOnClickListener { selectProfile(entry) }
            }
            gameToggle.addView(button)
        }
        gameToggle.visibility = if (profiles.size > 1) View.VISIBLE else View.GONE
    }

    private fun Int.dp(): Int = (this * resources.displayMetrics.density).toInt()

    private fun selectProfile(target: GameProfile) {
        if (target == profile || GameSetup.isRunning) return
        profile = target
        GameProfile.select(this, target)
        refresh()
    }

    private fun label(action: Action): Int = when (action) {
        Action.Play -> R.string.home_play
        Action.Resume -> R.string.home_resume
        Action.SelectDisc -> R.string.home_select_disc
        Action.ImportGame -> R.string.home_import
        Action.BuildGame -> R.string.home_build
        Action.DownloadModPack -> R.string.home_download_mod_pack
    }

    private fun perform(action: Action) {
        if (GameSetup.isRunning) return
        when (action) {
            Action.Play, Action.Resume -> play()
            Action.SelectDisc -> selectDiscImage()
            Action.ImportGame -> importGame()
            Action.BuildGame -> buildGame()
            Action.DownloadModPack -> downloadModPack()
        }
    }

    /**
     * Fetches Retro Rewind's pack from Retro Rewind's own server, as the computer launcher does,
     * after saying where it comes from and how big it is.
     */
    private fun downloadModPack() {
        if (GameSetup.isRunning) return
        val installed = RetroRewindPack.installedVersion(this)
        val message = if (installed == null) {
            getString(R.string.home_mod_pack_message)
        } else {
            getString(R.string.home_mod_pack_update_message, installed)
        }
        confirm(R.string.home_mod_pack_title, message, R.string.home_download_mod_pack) {
            GameSetupService.startModPackDownload(this)
            showPage(Page.Home)
        }
    }

    private fun buildStatus(state: GameSetup.State.Working): String = when (state.step) {
        GameBuild.Step.Download -> getString(R.string.build_step_download, state.stepDone, state.stepTotal)
        GameBuild.Step.Translate -> getString(R.string.build_step_translate, state.stepDone, state.stepTotal)
        GameBuild.Step.Compile -> getString(R.string.build_step_compile, state.stepDone, state.stepTotal)
        GameBuild.Step.Link -> getString(R.string.build_step_link)
        GameBuild.Step.Install -> getString(R.string.build_step_install)
        GameBuild.Step.Prepare, null -> getString(R.string.build_step_prepare)
    }

    /** Builds the selected game on this headset from DATA, after saying what that takes. */
    private fun buildGame() {
        if (GameSetup.isRunning || GameStorage.discStatus(this) != GameStorage.DiscStatus.Ready) return
        if (!GameStorage.modContentReady(this, profile)) {
            confirm(R.string.home_mod_needed_title, R.string.home_mod_needed_message, R.string.home_download_mod_pack) { downloadModPack() }
            return
        }
        val message = if (GameLibrary.status(this, profile) == GameLibrary.Status.Ready) {
            R.string.home_build_replace_message
        } else {
            R.string.home_build_message
        }
        confirm(R.string.home_build_title, message, R.string.home_build) {
            GameSetupService.startBuild(this, profile)
            showPage(Page.Home)
        }
    }

    private fun showBanner(text: String?, warning: Boolean = false) {
        if (text == null) {
            dataBanner.visibility = View.GONE
            return
        }
        dataBannerText.text = text
        dataBannerText.scrollTo(0, 0)
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
        if (GameLibrary.status(this, profile) != GameLibrary.Status.Ready) {
            openPicker(REQUEST_GAME_PACKAGE)
            return
        }
        confirm(R.string.home_replace_game_title, R.string.home_replace_game_message, R.string.home_import) {
            openPicker(REQUEST_GAME_PACKAGE)
        }
    }

    private fun confirm(title: Int, message: Int, positive: Int, onConfirm: () -> Unit) =
        confirm(title, getString(message), positive, onConfirm)

    private fun confirm(title: Int, message: CharSequence, positive: Int, onConfirm: () -> Unit) {
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
        const val EXTRA_DEBUG_BUILD_GAME = "org.wiicompiled.quest.debug.BUILD_GAME"
    }
}
